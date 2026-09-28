#include "AudioMixerBridge.hpp"

#include <components/VolumeControl.hpp>
#include <components/VolumeMeter.hpp>
#include <QApplication>
#include <QKeyEvent>
#include <QWheelEvent>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QLayout>
#include <QBuffer>
#include <QColor>
#include <QFontInfo>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace OBSWeb {
namespace {
constexpr double SilenceDb = -100.0;

double JsonDb(float value)
{
	return std::isfinite(value) ? std::max(SilenceDb, double(value)) : SilenceDb;
}

bool IsMixerSource(obs_source_t *source)
{
	return source && !obs_source_removed(source) && (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO);
}

VolumeSlider *FindSlider(VolumeControl *control)
{
	for (auto *slider : control->findChildren<QSlider *>()) {
		if (slider->inherits("VolumeSlider"))
			return static_cast<VolumeSlider *>(slider);
	}
	return nullptr;
}

bool BelongsToMixer(QObject *object, QObject *root)
{
	for (auto *parent = object->parent(); parent; parent = parent->parent()) {
		if (parent->inherits("AudioMixer"))
			return true;
		if (parent == root)
			break;
	}
	return false;
}
} // namespace

namespace AudioMixerDetail {
std::optional<Request> ParseCommand(const QString &command, const QJsonObject &args, QString &error)
{
	error.clear();
	Command kind;
	if (command == QStringLiteral("audio.volume"))
		kind = Command::Volume;
	else if (command == QStringLiteral("audio.mute"))
		kind = Command::Mute;
	else if (command == QStringLiteral("audio.monitor"))
		kind = Command::Monitor;
	else if (command == QStringLiteral("audio.key"))
		kind = Command::Key;
	else if (command == QStringLiteral("audio.wheel"))
		kind = Command::Wheel;
	else {
		error = QStringLiteral("UnknownAudioCommand");
		return std::nullopt;
	}

	const auto id = args.value(QStringLiteral("uuid"));
	const auto uuid = id.toString();
	if (!id.isString() || uuid.trimmed().isEmpty() || uuid.size() > 256 || uuid.contains(QChar::Null)) {
		error = QStringLiteral("InvalidAudioSource");
		return std::nullopt;
	}

	const auto value = args.value(QStringLiteral("value"));
	if (kind == Command::Key || kind == Command::Wheel) {
		for (const auto *modifier : {"control", "shift"}) {
			if (args.contains(modifier) && !args.value(modifier).isBool()) {
				error = QStringLiteral("InvalidAudioValue");
				return std::nullopt;
			}
		}
		const int modifiers = (args.value("control").toBool() ? Qt::ControlModifier : 0) |
			(args.value("shift").toBool() ? Qt::ShiftModifier : 0);
		const auto key = args.value("key").toString();
		if (kind == Command::Key && QStringList{"ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
			"PageUp", "PageDown", "Home", "End"}.contains(key))
			return Request{kind, uuid, 0, key, modifiers};
		if (kind == Command::Wheel && value.isDouble() && std::isfinite(value.toDouble()) &&
			value.toDouble() != 0 && std::abs(value.toDouble()) <= 12000 && std::trunc(value.toDouble()) == value.toDouble())
			return Request{kind, uuid, value.toDouble(), {}, modifiers};
		error = QStringLiteral("InvalidAudioValue");
		return std::nullopt;
	}
	if (kind == Command::Mute) {
		if (value.isBool())
			return Request{kind, uuid, double(value.toBool())};
	} else if (value.isDouble() && std::isfinite(value.toDouble())) {
		const double number = value.toDouble();
		if (kind == Command::Volume && number >= 0.0 && number <= 1.0)
			return Request{kind, uuid, number};
		if (kind == Command::Monitor) {
			if (number == 0.0 || number == 2.0)
				return Request{kind, uuid, number};
			if (number == 1.0) {
				// OBS33's deprecated enum API silently maps 1 to enabled. Do
				// not promise a monitor-only mode the current engine lacks.
				error = QStringLiteral("UnsupportedMonitoringMode");
				return std::nullopt;
			}
		}
	}
	error = QStringLiteral("InvalidAudioValue");
	return std::nullopt;
}

struct LevelMeter::Impl {
	OBSWeakSource source;
	obs_volmeter_t *meter = nullptr;
	mutable std::mutex mutex;
	std::array<float, MAX_AUDIO_CHANNELS> peak{};
	std::chrono::steady_clock::time_point updated{};
	bool hasSample = false;
	bool truePeak = false;

	static void Updated(void *parameter, const float *, const float *peaks, const float *)
	{
		auto &self = *static_cast<Impl *>(parameter);
		std::lock_guard<std::mutex> lock(self.mutex);
		std::copy_n(peaks, MAX_AUDIO_CHANNELS, self.peak.begin());
		self.updated = std::chrono::steady_clock::now();
		self.hasSample = true;
	}

	explicit Impl(obs_source_t *source_)
	{
		if (!IsMixerSource(source_))
			return;
		source = OBSGetWeakRef(source_);
		meter = obs_volmeter_create(OBS_FADER_LOG);
		if (!meter)
			return;
		obs_volmeter_add_callback(meter, Updated, this);
		obs_volmeter_attach_source(meter, source_);
	}

	~Impl()
	{
		if (!meter)
			return;
		// The native callback mutex waits out any Updated call before its
		// parameter storage can disappear. Never hold our mutex here: the
		// audio thread takes it while it holds that native callback mutex.
		OBSSource keepAlive = OBSGetStrongRef(source);
		obs_volmeter_remove_callback(meter, Updated, this);
		obs_volmeter_destroy(meter);
	}
};

LevelMeter::LevelMeter(obs_source *source) : impl(std::make_unique<Impl>(source)) {}
LevelMeter::~LevelMeter() = default;

QJsonArray LevelMeter::peaks() const
{
	OBSSource source = OBSGetStrongRef(impl->source);
	if (!impl->meter || !IsMixerSource(source))
		return {};
	const int channels = std::clamp(obs_volmeter_get_nr_channels(impl->meter), 0, MAX_AUDIO_CHANNELS);
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->hasSample)
		return {};
	const bool idle = std::chrono::steady_clock::now() - impl->updated > std::chrono::milliseconds(500);
	QJsonArray result;
	for (int channel = 0; channel < channels; ++channel)
		result.append(idle ? SilenceDb : JsonDb(impl->peak[channel]));
	return result;
}

int LevelMeter::channelCount() const
{
	if (!impl->meter)
		return 0;
	const int channels = std::clamp(obs_volmeter_get_nr_channels(impl->meter), 0, MAX_AUDIO_CHANNELS);
	if (channels)
		return channels;
	obs_audio_info info{};
	return obs_get_audio_info(&info) && info.speakers == SPEAKERS_MONO ? 1 : 2;
}

void LevelMeter::setTruePeak(bool enabled)
{
	if (impl->meter && impl->truePeak != enabled) {
		obs_volmeter_set_peak_meter_type(impl->meter, enabled ? TRUE_PEAK_METER : SAMPLE_PEAK_METER);
		impl->truePeak = enabled;
	}
}
} // namespace AudioMixerDetail

struct AudioMixerBridge::Impl {
	struct Entry {
		QPointer<VolumeControl> control;
		OBSWeakSource source;
		QPointer<VolumeMeter> meter;
	};
	QPointer<QObject> root;
	std::map<QString, Entry> entries;
	std::vector<QString> order;
	QHash<qint64, QString> icons;

	explicit Impl(QObject *root_) : root(root_) {}

	QString iconData(const QIcon &icon)
	{
		if (icon.isNull())
			return {};
		if (!icons.contains(icon.cacheKey())) {
			if (icons.size() > 64)
				icons.clear();
			QByteArray bytes;
			QBuffer buffer(&bytes);
			if (buffer.open(QIODevice::WriteOnly) && icon.pixmap(32, 32).save(&buffer, "PNG"))
				icons.insert(icon.cacheKey(), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
		}
		return icons.value(icon.cacheKey());
	}

	void refresh()
	{
		std::vector<VolumeControl *> controls;
		if (root) {
			for (auto *frame : root->findChildren<QFrame *>()) {
				if (frame->inherits("VolumeControl") && BelongsToMixer(frame, root))
					controls.push_back(static_cast<VolumeControl *>(frame));
			}
		}
		std::stable_sort(controls.begin(), controls.end(), [](VolumeControl *a, VolumeControl *b) {
			auto *parent = a->parentWidget();
			if (parent != b->parentWidget())
				return std::less<QWidget *>{}(parent, b->parentWidget());
			if (parent && parent->layout())
				return parent->layout()->indexOf(a) < parent->layout()->indexOf(b);
			return a->getCachedName().compare(b->getCachedName(), Qt::CaseInsensitive) < 0;
		});
		std::set<QString> current;
		order.clear();
		for (auto *control : controls) {
			OBSSource source = OBSGetStrongRef(control->weakSource());
			if (!IsMixerSource(source))
				continue;
			const auto uuid = QString::fromUtf8(obs_source_get_uuid(source));
			if (uuid.isEmpty() || !current.insert(uuid).second)
				continue;
			order.push_back(uuid);
			auto found = entries.find(uuid);
			if (found == entries.end() || found->second.control != control ||
			    OBSGetStrongRef(found->second.source).Get() != source.Get()) {
				entries.erase(uuid);
				Entry entry{control, OBSGetWeakRef(source), control->findChild<VolumeMeter *>()};
				found = entries.emplace(uuid, std::move(entry)).first;
			}
		}
		for (auto it = entries.begin(); it != entries.end();) {
			if (current.find(it->first) == current.end())
				it = entries.erase(it);
			else
				++it;
		}
	}
};

AudioMixerBridge::AudioMixerBridge(QObject *parent) : QObject(parent), impl(std::make_unique<Impl>(parent)) {}
AudioMixerBridge::~AudioMixerBridge() = default;

QJsonArray AudioMixerBridge::snapshot()
{
	if (QThread::currentThread() != thread())
		return {};
	impl->refresh();
	QJsonArray result;
	for (const auto &uuid : impl->order) {
		auto &entry = impl->entries.at(uuid);
		OBSSource source = OBSGetStrongRef(entry.source);
		auto *control = entry.control.data();
		if (!control || !IsMixerSource(source))
			continue;
		auto *slider = FindSlider(control);
		OBSDataAutoRelease settings = obs_source_get_private_settings(source);
		const bool locked = obs_data_get_bool(settings, "volume_locked");
		const auto &status = control->mixerStatus();
		const double deflection = slider ? obs_fader_get_deflection(slider->fad) : 0.0;
		const auto meter = entry.meter ? entry.meter->displayState() : VolumeMeter::DisplayState{};
		QJsonObject channel{
			{"uuid", uuid},
			{"name", QString::fromUtf8(obs_source_get_name(source))},
			{"volume", std::isfinite(deflection) ? std::clamp(deflection, 0.0, 1.0) : 0.0},
			{"db", JsonDb(obs_mul_to_db(obs_source_get_volume(source)))},
			{"muted", obs_source_muted(source)},
			{"monitoring", obs_source_get_monitoring_enabled(source) ? 2 : 0},
			{"monitoringSupported", QJsonArray{0, 2}},
			{"monitoringAvailable", obs_audio_monitoring_available()},
			{"enabled", control->isEnabled()},
			{"volumeEnabled", slider && slider->isEnabled() && !locked},
			{"sourceEnabled", obs_source_enabled(source)},
			{"visible", !control->isHidden()},
			{"locked", locked},
			{"hidden", obs_data_get_bool(settings, "mixer_hidden")},
			{"pinned", obs_data_get_bool(settings, "mixer_pinned")},
			{"global", status.has(VolumeControl::MixerStatus::Global)},
			{"preview", status.has(VolumeControl::MixerStatus::Preview)},
			{"active", obs_source_active(source) && obs_source_audio_active(source)},
			{"unassigned", !(obs_source_get_audio_mixers(source) & ((1 << MAX_AUDIO_MIXES) - 1))},
			{"channels", meter.channelCount},
			{"meterMinimum", meter.minimum},
			{"meterWarning", meter.warning},
			{"meterError", meter.error},
			{"meterDisabledColors", meter.disabledColors},
			{"meterThickness", meter.thickness},
			{"preferredWidth", std::clamp(control->sizeHint().width(), 70, 110)},
		};
		QJsonObject verticalMetrics;
		if (control->isVertical() && entry.meter) {
			auto *name = control->firstWidget();
			auto *nameLabel = name ? name->findChild<QLabel *>() : nullptr;
			auto *frame = control->findChild<QWidget *>(QStringLiteral("volMeterFrame"));
			verticalMetrics = {{"devicePixelRatio", control->devicePixelRatioF()},
				{"minimumHeight", control->minimumSizeHint().height()},
				{"meterMinimumHeight", entry.meter->minimumSizeHint().height()},
				{"meterFontSize", QFontInfo(entry.meter->font()).pixelSize()},
				{"bodyMinimumHeight", frame ? frame->minimumSizeHint().height() : entry.meter->minimumSizeHint().height()},
				{"bottomPadding", control->contentsMargins().bottom()},
				{"nameHeight", name ? name->minimumSizeHint().height() : 0},
				{"nameFontSize", nameLabel ? QFontInfo(nameLabel->font()).pixelSize() : QFontInfo(control->font()).pixelSize()}};
		}
		// Read the native presentation, including localized status and theme icons.
		// Do not infer a category from names or duplicate the mixer's grouping rules.
		for (auto *label : control->findChildren<QLabel *>()) {
			if (label->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts).contains("mixer-category")) {
				channel.insert("category", label->text());
				channel.insert("categoryColor", label->palette().color(QPalette::WindowText).name());
				channel.insert("categoryBackground", label->palette().color(QPalette::Window).name());
				verticalMetrics.insert("categoryHeight", label->minimumSizeHint().height());
				verticalMetrics.insert("categoryFontSize", QFontInfo(label->font()).pixelSize());
			} else if (label->objectName() == QStringLiteral("volLabel")) {
				channel.insert("dbText", label->text());
				verticalMetrics.insert("dbHeight", label->minimumSizeHint().height());
				verticalMetrics.insert("dbFontSize", QFontInfo(label->font()).pixelSize());
			}
		}
		for (auto *button : control->findChildren<QPushButton *>()) {
			const auto classes = button->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts);
			const QString prefix = classes.contains("btn-mute") ? QStringLiteral("mute") :
				classes.contains("btn-monitor") ? QStringLiteral("monitor") : QString();
			if (!prefix.isEmpty()) {
				channel.insert(prefix + "Icon", impl->iconData(button->icon()));
				channel.insert(prefix + "Tooltip", button->toolTip());
				channel.insert(prefix + "Enabled", button->isEnabled());
				verticalMetrics.insert("buttonsHeight", std::max(verticalMetrics.value("buttonsHeight").toInt(), button->sizeHint().height()));
				verticalMetrics.insert("buttonWidth", std::max(verticalMetrics.value("buttonWidth").toInt(), button->sizeHint().width()));
			}
		}
		for (auto *widget : control->findChildren<QWidget *>()) {
			if (!widget->inherits("VolumeMeter"))
				continue;
			QJsonObject colors;
			for (const auto *property : {"backgroundNominalColor", "backgroundWarningColor", "backgroundErrorColor",
				"foregroundNominalColor", "foregroundWarningColor", "foregroundErrorColor",
				"backgroundNominalColorDisabled", "backgroundWarningColorDisabled", "backgroundErrorColorDisabled",
				"foregroundNominalColorDisabled", "foregroundWarningColorDisabled", "foregroundErrorColorDisabled",
				"magnitudeColor", "majorTickColor", "minorTickColor"}) {
				const auto color = widget->property(property).value<QColor>();
				if (color.isValid())
					colors.insert(QLatin1String(property), color.name());
			}
			// Native accessibility overrides are applied to the painted colors,
			// while the Q_PROPERTY getters retain the underlying theme colors.
			const QString suffix = meter.disabledColors ? QStringLiteral("Disabled") : QString();
			const QStringList regions{"NominalColor", "WarningColor", "ErrorColor"};
			for (int region = 0; region < 3; ++region) {
				colors.insert("background" + regions[region] + suffix, meter.background[region].name());
				colors.insert("foreground" + regions[region] + suffix, meter.foreground[region].name());
			}
			channel.insert("meterColors", colors);
			break;
		}
		if (control->isVertical()) channel.insert("verticalMetrics", verticalMetrics);
		QJsonArray ticks;
		if (slider) {
			const auto convert = obs_fader_db_to_def(slider->fad);
			for (int db = -10; db >= -90; db -= 10)
				ticks.append(double(convert(float(db))));
		}
		channel.insert("faderTicks", ticks);
		result.append(channel);
	}
	return result;
}

QJsonObject AudioMixerBridge::levels()
{
	if (QThread::currentThread() != thread())
		return {};
	QJsonObject result;
	for (auto it = impl->entries.begin(); it != impl->entries.end();) {
		OBSSource source = OBSGetStrongRef(it->second.source);
		if (!it->second.control || !it->second.meter || !IsMixerSource(source)) {
			it = impl->entries.erase(it);
			continue;
		}
		const auto state = it->second.meter->displayState();
		QJsonArray channels;
		for (int channel = 0; channel < state.channelCount; ++channel) {
			const auto &value = state.channels[channel];
			channels.append(QJsonObject{{"peak", JsonDb(value.peak)}, {"peakHold", JsonDb(value.peakHold)},
				{"magnitude", JsonDb(value.magnitude)}, {"inputPeak", JsonDb(value.inputPeak)},
				{"inputColor", value.inputColor.name()}});
		}
		result.insert(it->first, QJsonObject{{"channels", channels}, {"minimum", state.minimum},
			{"idle", state.idle}, {"clipping", state.clipping}});
		++it;
	}
	return result;
}

bool AudioMixerBridge::execute(const QString &command, const QJsonObject &args, QString &error)
{
	if (QThread::currentThread() != thread()) {
		error = QStringLiteral("WrongAudioThread");
		return false;
	}
	const auto request = AudioMixerDetail::ParseCommand(command, args, error);
	if (!request)
		return false;
	impl->refresh();
	const auto found = impl->entries.find(request->uuid);
	if (found == impl->entries.end()) {
		error = QStringLiteral("AudioSourceUnavailable");
		return false;
	}
	OBSSource source = OBSGetStrongRef(found->second.source);
	auto *control = found->second.control.data();
	if (!control || !control->isEnabled() || !IsMixerSource(source)) {
		error = QStringLiteral("AudioSourceUnavailable");
		return false;
	}
	bool invoked = false;
	using AudioMixerDetail::Command;
	if (request->command == Command::Volume || request->command == Command::Key || request->command == Command::Wheel) {
		auto *slider = FindSlider(control);
		OBSDataAutoRelease settings = obs_source_get_private_settings(source);
		if (!slider || !slider->isEnabled() || obs_data_get_bool(settings, "volume_locked")) {
			error = QStringLiteral("VolumeLocked");
			return false;
		}
		if (request->command == Command::Key) {
			static const QHash<QString, int> keys{{"ArrowLeft", Qt::Key_Left}, {"ArrowRight", Qt::Key_Right},
				{"ArrowUp", Qt::Key_Up}, {"ArrowDown", Qt::Key_Down}, {"PageUp", Qt::Key_PageUp},
				{"PageDown", Qt::Key_PageDown}, {"Home", Qt::Key_Home}, {"End", Qt::Key_End}};
			QKeyEvent event(QEvent::KeyPress, keys.value(request->key), Qt::KeyboardModifiers(request->modifiers));
			QApplication::sendEvent(slider, &event);
			return true;
		}
		if (request->command == Command::Wheel) {
			const QPoint center = slider->rect().center();
			QWheelEvent event(QPointF(center), QPointF(slider->mapToGlobal(center)), QPoint(),
				QPoint(0, int(request->value)), Qt::NoButton, Qt::KeyboardModifiers(request->modifiers), Qt::NoScrollPhase, false);
			slider->handleFrontendWheel(&event);
			return true;
		}
		const int value = slider->minimum() +
				  int(std::lround(request->value * (slider->maximum() - slider->minimum())));
		const double current = obs_fader_get_deflection(slider->fad);
		const double desired = double(value - slider->minimum()) / (slider->maximum() - slider->minimum());
		if (std::abs(current - desired) < 0.000001)
			return true;
		// Use the native slot for OBS_FADER_LOG and repeatable undo. Explicit
		// invocation also handles a pending native slider refresh after an
		// external source-volume update, when setValue alone might do nothing.
		invoked = QMetaObject::invokeMethod(control, "sliderChanged", Qt::DirectConnection, Q_ARG(int, value));
		if (invoked) {
			QSignalBlocker block(slider);
			slider->setValue(value);
		}
	} else if (request->command == Command::Mute) {
		const bool mute = request->value != 0.0;
		if (obs_source_muted(source) == mute)
			return true;
		invoked = QMetaObject::invokeMethod(control, "handleMuteButton", Qt::DirectConnection, Q_ARG(bool, mute));
	} else {
		const bool enabled = request->value != 0.0;
		if (obs_source_get_monitoring_enabled(source) == enabled)
			return true;
		if (enabled && !obs_audio_monitoring_available()) {
			error = QStringLiteral("MonitoringUnavailable");
			return false;
		}
		invoked = QMetaObject::invokeMethod(control, "handleMonitorButton", Qt::DirectConnection,
						 Q_ARG(bool, enabled));
	}
	if (!invoked)
		error = QStringLiteral("NativeAudioControlUnavailable");
	return invoked;
}

} // namespace OBSWeb
