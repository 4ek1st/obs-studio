// Include outside OBSWebView2's class, only in the disposable integration build.
#include <webview2/AudioMixerBridge.hpp>
#include <components/VolumeControl.hpp>
#include <components/VolumeMeter.hpp>
#include <components/VolumeSlider.hpp>
#include <widgets/OBSBasic.hpp>
#include <webview2/WebView2Widget.hpp>
#include <utility/platform.hpp>
#include <util/platform.h>

#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QKeyEvent>

#include <cmath>
#include <functional>
#include <array>

inline void RunAudioMixerIntegrationChecks(QObject *owner, const std::function<void(bool, const char *)> &check)
{
	auto *main = qobject_cast<OBSBasic *>(owner);
	check(main != nullptr, "Audio integration has the native OBS main window");
	if (!main)
		return;
	static bool registered = false;
	if (!registered) {
		obs_source_info info{};
		info.id = "webview2-integration-audio";
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO;
		info.get_name = [](void *) { return "WebView2 integration audio"; };
		info.create = [](obs_data_t *, obs_source_t *source) -> void * { return source; };
		info.destroy = [](void *) {};
		obs_register_source(&info);
		registered = true;
	}
	OBSSourceAutoRelease source =
		obs_source_create("webview2-integration-audio", "WebView2 mixer fixture", nullptr, nullptr);
	check(source != nullptr, "Audio source fixture exists");
	if (!source)
		return;
	const auto uuid = QString::fromUtf8(obs_source_get_uuid(source));
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	OBSWeb::AudioMixerBridge mixer(main);
	auto stateFor = [&](const QString &id) {
		for (const auto value : mixer.snapshot()) {
			const auto state = value.toObject();
			if (state.value("uuid").toString() == id)
				return state;
		}
		return QJsonObject{};
	};
	check(!stateFor(uuid).isEmpty(), "Real native mixer control appears in WebView2 audio snapshot");
	QString error;
	auto send = [&](const QString &command, const QJsonValue &value) {
		return mixer.execute(command, {{"uuid", uuid}, {"value", value}}, error);
	};
	check(send("audio.volume", 0.5) && obs_source_get_volume(source) > 0.10f &&
		      obs_source_get_volume(source) < 0.13f,
	      "Web volume uses the native logarithmic fader rather than a linear multiplier");
	check(std::abs(stateFor(uuid).value("volume").toDouble() - 0.5) < 0.002,
	      "Audio snapshot reports normalized fader position");
	main->undo_s.undo();
	check(std::abs(obs_source_get_volume(source) - 1.0f) < 0.0001f, "Native undo restores Web fader changes");
	main->undo_s.redo();
	check(obs_source_get_volume(source) > 0.10f && obs_source_get_volume(source) < 0.13f,
	      "Native redo reapplies Web fader changes");
	check(send("audio.mute", true) && obs_source_muted(source), "Web mute changes native source synchronously");
	main->undo_s.undo();
	check(!obs_source_muted(source), "Native undo restores Web mute changes");
	const auto previousVolume = obs_source_get_volume(source);
	check(!send("audio.volume", -0.1) && !send("audio.volume", 1.1) && !send("audio.volume", "0.2") &&
		      obs_source_get_volume(source) == previousVolume,
	      "Malformed Web volume cannot mutate the native fader");
	check(!send("audio.mute", 1) && !obs_source_muted(source), "Numeric mute cannot bypass boolean validation");
	VolumeControl *native = nullptr;
	for (auto *control : main->findChildren<VolumeControl *>()) {
		if (OBSGetStrongRef(control->weakSource()).Get() == source.Get()) {
			native = control;
			break;
		}
	}
	check(native != nullptr, "Audio test resolves the actual VolumeControl by source identity");
	if (native) {
		native->processMixerState();
		auto *nativeMeter = native->findChild<VolumeMeter *>();
		check(nativeMeter != nullptr, "Audio adapter consumes the actual native VolumeMeter");
		if (nativeMeter) {
			nativeMeter->displayState(); // Establish channel layout before the first PCM callback.
			send("audio.volume", 1.0);
			auto output = [&](float amplitude) {
				std::array<float, 1024> left, right;
				left.fill(amplitude); right.fill(amplitude * 0.5f);
				obs_source_audio audio{};
				audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
				audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
				audio.frames = uint32_t(left.size()); audio.speakers = SPEAKERS_STEREO;
				audio.format = AUDIO_FORMAT_FLOAT_PLANAR; audio.samples_per_sec = 48000;
				for (int block = 0; block < 8; ++block) {
					audio.timestamp = os_gettime_ns(); obs_source_output_audio(source, &audio);
				}
			};
			output(0.5f);
			const auto live = mixer.levels().value(uuid).toObject();
			const auto channels = live.value("channels").toArray();
			check(channels.size() == 2 && !live.value("idle").toBool() &&
				std::abs(channels[0].toObject().value("peak").toDouble() + 6.0206) < 0.05 &&
				std::abs(channels[1].toObject().value("peak").toDouble() + 12.0412) < 0.05,
				"Native PCM stereo levels reach WebView with independent left and right peaks");
			check(channels.size() == 2 && channels[0].toObject().contains("magnitude") &&
				channels[0].toObject().value("inputColor").toString().startsWith('#'),
				"WebView receives the native magnitude and input-peak indicator state");
			output(0.125f);
			const auto decayed = mixer.levels().value(uuid).toObject().value("channels").toArray();
			check(decayed.size() == 2 && decayed[0].toObject().value("peak").toDouble() > -18.0 &&
				std::abs(decayed[0].toObject().value("peakHold").toDouble() + 6.0206) < 0.05,
				"WebView uses the original native decay and held peak instead of the latest callback sample");
			nativeMeter->setPeakMeterType(TRUE_PEAK_METER);
			const auto truePeak = stateFor(uuid);
			check(truePeak.value("meterWarning").toDouble() == -13.0 && truePeak.value("meterError").toDouble() == -2.0,
				"True-peak Web meter thresholds are the actual native -13 and -2 dBTP thresholds");
			nativeMeter->setPeakMeterType(config_get_uint(obs_frontend_get_profile_config(), "Audio", "PeakMeterType") == 1 ? TRUE_PEAK_METER : SAMPLE_PEAK_METER);
			send("audio.volume", 0.5);
		}
		auto *nativeSlider = native->findChild<VolumeSlider *>();
		if (nativeSlider) {
			const int initial = nativeSlider->value();
			check(mixer.execute("audio.key", {{"uuid", uuid}, {"key", "PageUp"}}, error) &&
				nativeSlider->value() == initial + nativeSlider->pageStep(),
				"Web fader PageUp uses the native page step");
			const int stepped = nativeSlider->value();
			check(mixer.execute("audio.key", {{"uuid", uuid}, {"key", "ArrowUp"}}, error) && nativeSlider->value() == stepped,
				"Web fader preserves AbsoluteSlider's native Up-arrow suppression");
			check(mixer.execute("audio.wheel", {{"uuid", uuid}, {"value", -120}}, error) && nativeSlider->value() < stepped,
				"Focused Web fader wheel executes the original QSlider wheel handler");
			check(!mixer.execute("audio.key", {{"uuid", uuid}, {"key", "F4"}}, error) &&
				!mixer.execute("audio.wheel", {{"uuid", uuid}, {"value", 120}, {"shift", 1}}, error),
				"Unknown fader keys and coerced wheel modifiers are rejected");
			send("audio.volume", 0.5);
		}
		const auto presentation = stateFor(uuid);
		QLabel *category = nullptr;
		for (auto *label : native->findChildren<QLabel *>())
			if (label->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts).contains("mixer-category"))
				category = label;
		check(category && !category->text().isEmpty() && presentation.value("category").toString() == category->text(),
		      "Web mixer category is the exact localized native label");
		check(category && category->property("class").metaType() == QMetaType::fromType<QString>() &&
			      category->property("class").toString().contains("mixer-category "),
		      "Native category fixture exercises Idian space-separated QString classes");
		auto *volumeLabel = native->findChild<QLabel *>(QStringLiteral("volLabel"));
		check(volumeLabel && presentation.value("dbText").toString() == volumeLabel->text(),
		      "Web mixer dB text matches the native fader label");
		const auto ticks = presentation.value("faderTicks").toArray();
		check(ticks.size() == 9 && ticks.first().toDouble() > ticks.last().toDouble() &&
			      ticks.first().toDouble() < 1.0 && ticks.last().toDouble() > 0.0,
		      "Web mixer receives the native logarithmic fader tick positions");
		check(presentation.value("channels").toInt() == 2 &&
			      presentation.value("meterColors").toObject().contains("foregroundNominalColor") &&
			      presentation.value("muteIcon").toString().startsWith("data:image/png;base64,") &&
			      presentation.value("monitorIcon").toString().startsWith("data:image/png;base64,"),
		      "Web mixer receives native stereo geometry, theme colors and both control icons");
		native->setLocked(true);
		check(!mixer.execute("audio.key", {{"uuid", uuid}, {"key", "End"}}, error) &&
			!mixer.execute("audio.wheel", {{"uuid", uuid}, {"value", 120}}, error),
			"Native volume lock also blocks Web keyboard and wheel adjustment");
		check(!send("audio.volume", 0.8) && obs_source_get_volume(source) == previousVolume &&
			      !stateFor(uuid).value("volumeEnabled").toBool(),
		      "Native volume lock also blocks Web volume changes");
		native->setLocked(false);
		native->setPinnedInMixer(true);
		check(stateFor(uuid).value("pinned").toBool(), "Web mixer reflects native pinned state");
		native->setPinnedInMixer(false);
		native->setHiddenInMixer(true);
		check(stateFor(uuid).value("hidden").toBool(), "Web mixer reflects native hidden state");
		native->setHiddenInMixer(false);
	}
	check(!send("audio.monitor", 1) && !obs_source_get_monitoring_enabled(source),
	      "Removed OBS33 monitor-only mode is rejected explicitly");
	if (obs_audio_monitoring_available()) {
		check(send("audio.monitor", 2) && obs_source_get_monitoring_enabled(source),
		      "Web monitoring enables native OBS33 monitoring state");
		if (native) {
			native->processMixerState();
			check(stateFor(uuid).value("monitorIcon").toString().startsWith("data:image/png;base64,"),
			      "Native checked monitor icon remains available with multiple Idian classes");
		}
		main->undo_s.undo();
		check(!obs_source_get_monitoring_enabled(source), "Native undo restores Web monitoring changes");
	}
	check(send("audio.volume", 0.0) && obs_source_get_volume(source) == 0.0f &&
		      stateFor(uuid).value("db").isDouble(),
	      "Silent fader retains a finite JSON dB value");
	obs_source_remove(source);
	check(!send("audio.mute", true) && stateFor(uuid).isEmpty() && !mixer.levels().contains(uuid),
	      "Removal invalidates Web commands and levels before queued QWidget deletion");
	source = nullptr;
	obs_wait_for_destroy_queue();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	OBSSourceAutoRelease replacement =
		obs_source_create("webview2-integration-audio", "WebView2 mixer fixture", nullptr, nullptr);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	check(replacement && !send("audio.mute", true) && !obs_source_muted(replacement),
	      "An expired UUID cannot target a replacement audio source with the same name");
	obs_source_remove(replacement);
}

// Runs only after the guarded portable integration test above registered the
// in-process source. It outputs known PCM to libobs without opening any device.
inline void RunAudioMixerVisualChecks(OBSBasic *main, WebView2Widget *browser,
				     const std::function<void()> &publish,
				     const std::function<void(bool, const char *)> &check,
				     const std::function<void()> &done)
{
	if (!browser) { check(false, "Native mixer dock owns its WebView2 surface"); done(); return; }
	struct Fixture {
		OBSSourceAutoRelease source;
		OBSScene scene;
		OBSSceneItem item;
		QPointer<QTimer> producer;
		uint64_t sample = 0;
		bool completed = false;
	};
	auto fixture = std::make_shared<Fixture>();
	fixture->source = obs_source_create("webview2-integration-audio", "WebView2 stereo fixture", nullptr, nullptr);
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	fixture->scene = current ? obs_scene_from_source(current) : nullptr;
	check(fixture->source && fixture->scene, "Visual mixer fixture has a real source and the active native scene");
	if (!fixture->source || !fixture->scene) { done(); return; }
	fixture->item = obs_scene_add(fixture->scene, fixture->source);
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(fixture->source));
	const bool wasVertical = config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "VerticalVolumeControl");
	const QPointer<QWidget> window = main;
	const QPointer<WebView2Widget> browserGuard = browser;
	const QSize originalSize = window->size();
	const QPointer<QDockWidget> dock = main->findChild<QDockWidget *>(QStringLiteral("mixerDock"));
	const bool wasFloating = dock && dock->isFloating();
	const bool wasHidden = dock && dock->isHidden();
	if (dock) { dock->setFloating(false); dock->show(); dock->raise(); }
	check(dock && !dock->isFloating() && !dock->isHidden(),
	      "Visual mixer fixture uses the visible attached native mixer dock");
	if (!wasVertical) main->toggleMixerLayout();
	fixture->producer = new QTimer(browser);
	fixture->producer->setTimerType(Qt::PreciseTimer);
	QObject::connect(fixture->producer, &QTimer::timeout, browser, [fixture] {
		if (fixture->completed || !fixture->source) return;
		std::array<float, 960> left, right;
		for (size_t n = 0; n < left.size(); ++n) {
			const auto phase = 2.0 * 3.14159265358979323846 * 440.0 * double(fixture->sample++) / 48000.0;
			left[n] = float(std::sin(phase) * 0.35);
			right[n] = float(std::sin(phase) * 0.18);
		}
		obs_source_audio audio{};
		audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
		audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
		audio.frames = uint32_t(left.size());
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = 48000;
		audio.timestamp = os_gettime_ns();
		obs_source_output_audio(fixture->source, &audio);
	});
	fixture->producer->start(20);
	auto finish = [fixture, main = QPointer<OBSBasic>(main), browserGuard, window, originalSize,
		       dock, wasFloating, wasHidden, wasVertical, publish, done] {
		if (fixture->completed) return;
		fixture->completed = true;
		if (fixture->producer) { fixture->producer->stop(); fixture->producer->deleteLater(); }
		if (fixture->item) { obs_sceneitem_remove(fixture->item); fixture->item = nullptr; }
		if (fixture->source) { obs_source_remove(fixture->source); fixture->source = nullptr; }
		if (main && config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "VerticalVolumeControl") != wasVertical)
			main->toggleMixerLayout();
		if (dock) { dock->setFloating(wasFloating); dock->setVisible(!wasHidden); }
		if (window) window->resize(originalSize);
		// CapturePreview can complete through qApp after its QWidget was closed.
		// Always release the source, but never call the root's callbacks afterward.
		if (main && browserGuard && window) { publish(); done(); }
	};
	QObject::connect(browser, &QObject::destroyed, main, [finish] { finish(); });
	QTimer::singleShot(10000, browser, [fixture, check, finish] {
		if (!fixture->completed) { check(false, "Native mixer visual capture completes before its timeout"); finish(); }
	});
	QTimer::singleShot(900, browser, [fixture, main, browser, browserGuard, window, dock, uuid, check, publish, finish] {
		if (fixture->completed) return;
		publish();
		QTimer::singleShot(500, browser, [fixture, main, browser, browserGuard, window, dock, uuid, check, publish, finish] {
			if (fixture->completed) return;
			QString artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
			if (artifacts.isEmpty()) {
				BPtr<char> path = GetAppConfigPathPtr("obs-studio/webview2-audio-artifacts");
				artifacts = QString::fromUtf8(path.Get());
			}
			if (!QDir().mkpath(artifacts)) { check(false, "Mixer visual artifact directory is writable"); finish(); return; }
			VolumeControl *native = nullptr;
			for (auto *control : main->findChildren<VolumeControl *>())
				if (OBSGetStrongRef(control->weakSource()).Get() == fixture->source.Get()) native = control;
			OBSWeb::AudioMixerBridge adapter(main);
			QJsonObject presentation;
			for (const auto value : adapter.snapshot())
				if (value.toObject().value("uuid").toString() == uuid) presentation = value.toObject();
			check(dock && !dock->isFloating() && !dock->isHidden() && native && !native->isHidden() &&
				      native->isVertical() && presentation.value("active").toBool() &&
				      presentation.value("visible").toBool() && !presentation.value("category").toString().isEmpty(),
			      "Actual vertical mixer has a visible active native stereo channel with a localized category");
			const auto metrics = presentation.value("verticalMetrics").toObject();
			auto *meter = native ? native->findChild<VolumeMeter *>() : nullptr;
			check(native && meter && metrics.value("minimumHeight").toInt() == native->minimumSizeHint().height() &&
				metrics.value("meterMinimumHeight").toInt() == meter->minimumSizeHint().height() &&
				metrics.value("devicePixelRatio").toDouble() == native->devicePixelRatioF() &&
				metrics.value("buttonsHeight").toInt() > 0 && metrics.value("nameFontSize").toInt() > 0,
				"Vertical mixer exports original channel and meter minimums, button height, font and pixel scale");
			if (native) check(native->grab().save(QDir(artifacts).filePath("mixer-vertical-native.png")),
					  "Original Qt vertical mixer channel is captured for visual comparison");
			QFile manifest(QDir(artifacts).filePath("mixer-vertical.json"));
			if (manifest.open(QIODevice::WriteOnly)) manifest.write(QJsonDocument(presentation).toJson());
			const auto png = QDir(artifacts).filePath("mixer-vertical.png");
			browser->capturePreview(png, [fixture, browserGuard, window, native = QPointer<VolumeControl>(native), check, publish, finish, png, artifacts](bool saved) {
				if (fixture->completed) return;
				if (!browserGuard || !window) { finish(); return; }
				const QImage pixels(png);
				check(saved && !pixels.isNull() && pixels.width() >= 80 && pixels.height() >= 100,
				      "Actual OBS WebView2 mixer dock with active PCM is captured as a PNG");
				window->resize(1280, 720);
				publish();
				QTimer::singleShot(700, browserGuard, [fixture, browserGuard, window, native, check, finish, artifacts] {
					if (fixture->completed) return;
					check(native && native->grab().save(QDir(artifacts).filePath("mixer-vertical-compact-native.png")),
					      "Original Qt compact channel is captured at the same dock height as WebView");
					const auto compact = QDir(artifacts).filePath("mixer-vertical-compact.png");
					browserGuard->capturePreview(compact, [fixture, browserGuard, window, check, finish, compact](bool saved) {
						if (fixture->completed) return;
						if (!browserGuard || !window) { finish(); return; }
						check(saved && !QImage(compact).isNull(),
						      "Actual OBS compact vertical mixer remains captured before fixture disposal");
						finish();
					});
				});
			});
		});
	});
}
