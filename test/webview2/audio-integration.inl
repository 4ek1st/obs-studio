// Include outside OBSWebView2's class, only in the disposable integration build.
#include <webview2/AudioMixerBridge.hpp>
#include <components/VolumeControl.hpp>
#include <widgets/OBSBasic.hpp>

#include <QCoreApplication>
#include <QEvent>

#include <cmath>
#include <functional>

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
		native->setLocked(true);
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
