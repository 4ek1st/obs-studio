// Include outside any class, only in the disposable portable integration build.
#ifndef OBS_WEBVIEW2_INTEGRATION_TESTS
#error "Replay checks require the disposable OBS integration build"
#endif

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <webview2/ControlBridge.hpp>
#include <webview2/WebView2Widget.hpp>
#include <utility/platform.hpp>
#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
class ReplayWorkflowChecks final : public QObject {
	enum class Stage { Starting, Active, Saving, Stopping, CapturingStopped, Cleanup };
	struct Setting {
		const char *section, *name;
		std::string value;
		bool existed;
	};
	QPointer<OBSBasic> main;
	QPointer<WebView2Widget> browser;
	std::function<void()> publish, done;
	std::function<void(bool, const char *)> check;
	QTimer poll;
	QElapsedTimer elapsed, total;
	Stage stage = Stage::Starting;
	std::vector<Setting> settings;
	OBSOutput output;
	QString directory, replayPath, activeImage, stoppedImage;
	QString runId = QString::number(QDateTime::currentMSecsSinceEpoch());
	bool failed = false, completed = false, rebuilt = false, attempted = false;
	bool started = false, stopped = false, captureComplete = false, nativeCall = false, forced = false;

	static bool colorOnly(obs_source_t *source, std::set<obs_source_t *> &visited, bool &hasColor)
	{
		if (!source) return false;
		if (!visited.insert(source).second) return true;
		if (QByteArray(obs_source_get_unversioned_id(source)) == "color_source") {
			hasColor = true;
			return true;
		}
		auto *scene = obs_source_is_group(source) ? obs_group_from_source(source) : obs_scene_from_source(source);
		if (!scene) return false;
		struct Context { std::set<obs_source_t *> &visited; bool &hasColor; bool safe = true; } context{visited, hasColor};
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *parameter) {
			auto &state = *static_cast<Context *>(parameter);
			state.safe = colorOnly(obs_sceneitem_get_source(item), state.visited, state.hasColor);
			return state.safe;
		}, &context);
		return context.safe;
	}
	bool active() const { return output && obs_output_active(output); }
	void enter(Stage next) { stage = next; elapsed.restart(); }
	void set(const char *section, const char *name, const QByteArray &value)
	{
		auto *config = main->Config();
		const char *previous = config_get_string(config, section, name);
		settings.push_back({section, name, previous ? previous : "", config_has_user_value(config, section, name)});
		config_set_string(config, section, name, value.constData());
	}
	QPushButton *button(const char *name) const { return main ? main->findChild<QPushButton *>(QLatin1String(name)) : nullptr; }
	void activate(const char *name)
	{
		auto *control = button(name);
		if (!control || !control->isEnabled() || control->isHidden()) {
			fail("Required native replay control is visible and enabled");
			return;
		}
		nativeCall = true;
		OBSWeb::ActivateControlButton(control);
		nativeCall = false;
		if (main && browser) publish();
	}
	void capture(const QString &path)
	{
		captureComplete = false;
		if (!main || !browser) { fail("Replay screenshot retains its live controls WebView"); return; }
		publish();
		const QPointer<ReplayWorkflowChecks> guard(this);
		QTimer::singleShot(250, this, [guard, path] {
			if (!guard || guard->completed || !guard->main || !guard->browser) return;
			guard->browser->capturePreview(path, [guard](bool saved) {
				if (!guard || guard->completed || !guard->main || !guard->browser) return;
				guard->check(saved, "Replay controls WebView state is captured from the actual runtime");
				guard->captureComplete = true;
			});
		});
	}
	void fail(const char *label)
	{
		if (completed || stage == Stage::Cleanup) return;
		check(false, label);
		enter(Stage::Cleanup);
		if (attempted && active()) obs_output_stop(output);
	}
	void finish()
	{
		if (completed) return;
		completed = true;
		poll.stop();
		if (main) {
			for (auto entry = settings.rbegin(); entry != settings.rend(); ++entry) {
				if (entry->existed) config_set_string(main->Config(), entry->section, entry->name, entry->value.c_str());
				else config_remove_value(main->Config(), entry->section, entry->name);
			}
			if (rebuilt && !active()) {
				output = nullptr;
				try { main->ResetOutputs(); }
				catch (...) { check(false, "Replay fixture restores the disposable native output configuration"); }
			}
		}
		if (!directory.isEmpty()) {
			QSaveFile report(QDir(directory).filePath("replay-workflow-" + runId + ".json"));
			if (report.open(QIODevice::WriteOnly)) {
				report.write(QJsonDocument(QJsonObject{{"passed", !failed}, {"path", replayPath},
					{"bytes", double(QFileInfo(replayPath).size())}, {"encoder", "x264_lowcpu"},
					{"started", started}, {"stopped", stopped}, {"forcedCleanup", forced},
					{"activeControls", activeImage}, {"stoppedControls", stoppedImage}}).toJson());
				check(report.commit(), "Replay workflow evidence is saved in the disposable profile");
			} else check(false, "Replay workflow evidence file can be created");
		}
		QTimer::singleShot(0, this, [this] {
			if (main && browser) { publish(); done(); }
			deleteLater();
		});
	}
	void tick()
	{
		if (completed) return;
		if (!main) return; // QObject ownership destroys this helper with the main window.
		if (!browser) fail("Replay fixture retains its native owner and WebView surface");
		for (auto *box : main->findChildren<QMessageBox *>()) {
			if (!box->isVisible()) continue;
			fail("Local replay start, save and stop complete without a native error dialog");
			box->reject();
		}
		if (stage == Stage::Cleanup) {
			if (!nativeCall && !active()) finish();
			else if (elapsed.elapsed() > 3000 && !forced) { forced = true; obs_output_force_stop(output); }
			else if (elapsed.elapsed() > 6000 && !nativeCall) finish();
			return;
		}
		if (total.elapsed() > 25000) { fail("Local replay workflow completes before its bounded timeout"); return; }
		if (stage == Stage::Starting && started && active()) {
			auto *startButton = button("replayBufferButton"), *saveButton = button("saveReplayButton");
			check(startButton && startButton->property("class").toStringList().join(' ').split(' ').contains("state-active") &&
				saveButton && !saveButton->isHidden() && saveButton->isEnabled(),
				"Native replay start activates its button and reveals the enabled Save Replay control");
			enter(Stage::Active);
			capture(activeImage);
		} else if (stage == Stage::Active && elapsed.elapsed() >= 1800 && captureComplete) {
			enter(Stage::Saving);
			activate("saveReplayButton");
		} else if (stage == Stage::Saving) {
			BPtr<char> path = obs_frontend_get_last_replay();
			const QString saved = QString::fromUtf8(path.Get() ? path.Get() : "");
			const QFileInfo file(saved);
			if (file.exists() && file.size() > 0 && file.fileName().contains(runId) &&
				file.absolutePath().compare(QDir(directory).absolutePath(), Qt::CaseInsensitive) == 0) {
				replayPath = file.absoluteFilePath();
				check(active(), "Saving replay leaves the native buffer running");
				check(file.size() > 0, "Native replay save writes a nonempty local MKV in the disposable directory");
				enter(Stage::Stopping);
				activate("replayBufferButton");
			}
		} else if (stage == Stage::Stopping && stopped && !active() && !nativeCall) {
			auto *startButton = button("replayBufferButton"), *saveButton = button("saveReplayButton");
			check(startButton && !startButton->property("class").toStringList().join(' ').split(' ').contains("state-active") &&
				saveButton && saveButton->isHidden(),
				"Native replay stop clears active state and hides Save Replay");
			enter(Stage::CapturingStopped);
			capture(stoppedImage);
		} else if (stage == Stage::CapturingStopped && captureComplete) finish();
	}
public:
	ReplayWorkflowChecks(OBSBasic *window, WebView2Widget *view, std::function<void()> update,
		std::function<void(bool, const char *)> report, std::function<void()> next)
		: QObject(window), main(window), browser(view), publish(std::move(update)), done(std::move(next)),
		check([this, report = std::move(report)](bool pass, const char *label) { failed |= !pass; report(pass, label); }), poll(this)
	{
		poll.setInterval(40);
		connect(&poll, &QTimer::timeout, this, [this] { tick(); });
		if (window) {
			connect(window, &OBSBasic::ReplayBufStarted, this, [this] { started = true; });
			connect(window, &OBSBasic::ReplayBufStopped, this, [this] { stopped = true; });
		}
	}
	void run()
	{
		const auto args = QCoreApplication::arguments();
		if (!main || !browser || !args.contains("--portable") || !args.contains("--webview2-self-test") ||
			!args.contains("--only-bundled-plugins") || !main->property("webview2NativeDocking").toBool() ||
			!main->isVisible() || main->Active() || obs_frontend_streaming_active() || obs_frontend_recording_active() ||
			obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active()) {
			check(false, "Replay runs only in the disposable visible native-shell self-test with idle outputs"); finish(); return;
		}
		std::set<obs_source_t *> visited;
		bool hasColor = false;
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		bool safe = colorOnly(scene, visited, hasColor) && hasColor;
		OBSSourceAutoRelease program = obs_get_output_source(0);
		if (program && obs_source_get_type(program) == OBS_SOURCE_TYPE_TRANSITION) {
			OBSSourceAutoRelease activeScene = obs_transition_get_active_source(program);
			safe &= activeScene && colorOnly(activeScene, visited, hasColor);
		} else if (program) safe &= colorOnly(program, visited, hasColor);
		for (uint32_t channel = 1; channel < MAX_CHANNELS; ++channel) {
			OBSSourceAutoRelease source = obs_get_output_source(channel); safe &= !source;
		}
		if (!safe) { check(false, "Replay fixture has only color sources and no audio or capture devices"); finish(); return; }
		check(true, "Replay fixture has only color sources and no audio or capture devices");
		BPtr<char> root = GetAppConfigPathPtr("obs-studio/test-recordings");
		directory = QString::fromUtf8(root.Get() ? root.Get() : "");
		if (directory.isEmpty() || !QDir().mkpath(directory)) { check(false, "Disposable replay directory exists"); finish(); return; }
		activeImage = QDir(directory).filePath("replay-active-" + runId + ".png");
		stoppedImage = QDir(directory).filePath("replay-stopped-" + runId + ".png");
		set("Output", "Mode", "Simple"); set("Output", "FilenameFormatting", ("WebView2-replay-" + runId).toUtf8());
		set("Output", "OverwriteIfExists", "false"); set("Video", "AutoRemux", "false");
		set("SimpleOutput", "StreamEncoder", "x264"); set("SimpleOutput", "RecEncoder", "x264_lowcpu");
		set("SimpleOutput", "RecQuality", "Small"); set("SimpleOutput", "RecFormat2", "mkv");
		set("SimpleOutput", "RecTracks", "1"); set("SimpleOutput", "RecRB", "true");
		set("SimpleOutput", "RecRBTime", "2"); set("SimpleOutput", "RecRBSize", "20");
		set("SimpleOutput", "RecRBPrefix", ""); set("SimpleOutput", "RecRBSuffix", "");
		set("SimpleOutput", "FilePath", directory.toUtf8());
		rebuilt = true;
		try { main->ResetOutputs(); output = OBSOutputAutoRelease(obs_frontend_get_replay_buffer_output()).Get(); }
		catch (...) { check(false, "Disposable software replay output initializes"); finish(); return; }
		if (!output) { check(false, "Disposable software replay output exists"); finish(); return; }
		total.start(); enter(Stage::Starting); poll.start(); attempted = true;
		activate("replayBufferButton");
	}
};
} // namespace

inline void RunReplayOutputWorkflowChecks(OBSBasic *main, WebView2Widget *controlsBrowser,
	const std::function<void()> &publish, const std::function<void(bool, const char *)> &check,
	const std::function<void()> &done)
{
	if (!main || !controlsBrowser) { check(false, "Replay workflow has its native main and controls browser"); done(); return; }
	auto *test = new ReplayWorkflowChecks(main, controlsBrowser, publish, check, done);
	QTimer::singleShot(0, test, [test] { test->run(); });
}
