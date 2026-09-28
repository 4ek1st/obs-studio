// Include outside any class, only in the disposable portable integration build.
#ifndef OBS_WEBVIEW2_INTEGRATION_TESTS
#error "Output workflow checks require the disposable OBS integration build"
#endif

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
class OutputWorkflowChecks final : public QObject {
	enum class Stage { Idle, Hide, Show, Starting, Recording, RejectDialog, RejectUnwind, AcceptDialog, Stopping, Cleanup };
	struct SavedValue {
		config_t *config;
		const char *section;
		const char *name;
		std::string value;
		bool hadUserValue;
	};
	QPointer<OBSBasic> main;
	QPointer<QWidget> frontend;
	QPointer<QWidget> central;
	std::function<void(bool, const char *)> check;
	std::function<void()> done;
	QTimer poll;
	QElapsedTimer elapsed;
	QElapsedTimer total;
	Stage stage = Stage::Idle;
	std::vector<SavedValue> saved;
	OBSOutput recordingOutput;
	QString directory;
	QString recordingPath;
	QString runId = QString::number(QDateTime::currentMSecsSinceEpoch());
	QString startError;
	bool completed = false;
	bool failed = false;
	bool fallback = false;
	bool outputsRebuilt = false;
	bool nativeCall = false;
	bool startReturned = false;
	bool startedSignal = false;
	bool stoppedSignal = false;
	bool attemptedRecording = false;
	bool forcedStop = false;
	bool passedCancellation = false;
	uint32_t encodedFrames = 0;

	void enter(Stage next)
	{
		stage = next;
		elapsed.restart();
	}

	void remember(config_t *config, const char *section, const char *name)
	{
		for (const auto &entry : saved)
			if (entry.config == config && !strcmp(entry.section, section) && !strcmp(entry.name, name))
				return;
		const char *value = config_get_string(config, section, name);
		saved.push_back({config, section, name, value ? value : "", config_has_user_value(config, section, name)});
	}

	void setString(config_t *config, const char *section, const char *name, const QByteArray &value)
	{
		remember(config, section, name);
		config_set_string(config, section, name, value.constData());
	}

	void setBool(config_t *config, const char *section, const char *name, bool value)
	{
		remember(config, section, name);
		config_set_bool(config, section, name, value);
	}

	static bool colorSceneOnly(obs_source_t *source, std::set<obs_source_t *> &visited, bool &hasColor)
	{
		if (!source)
			return false;
		if (!visited.insert(source).second)
			return true;
		if (QByteArray(obs_source_get_unversioned_id(source)) == "color_source") {
			hasColor = true;
			return true;
		}
		obs_scene_t *scene = obs_source_is_group(source) ? obs_group_from_source(source) : obs_scene_from_source(source);
		if (!scene)
			return false;
		struct Context {
			std::set<obs_source_t *> &visited;
			bool &hasColor;
			bool safe = true;
		} context{visited, hasColor};
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			auto &context = *static_cast<Context *>(data);
			context.safe = colorSceneOnly(obs_sceneitem_get_source(item), context.visited, context.hasColor);
			return context.safe;
		}, &context);
		return context.safe;
	}

	QMessageBox *messageBox() const
	{
		if (!main)
			return nullptr;
		for (auto *box : main->findChildren<QMessageBox *>())
			if (box->isVisible())
				return box;
		return nullptr;
	}

	bool recordingActive() const
	{
		return recordingOutput && obs_output_active(recordingOutput);
	}

	void writeEvidence()
	{
		if (directory.isEmpty())
			return;
		QSaveFile report(QDir(directory).filePath("output-workflow-" + runId + ".json"));
		if (report.open(QIODevice::WriteOnly)) {
			report.write(QJsonDocument(QJsonObject{{"passed", !failed}, {"recording", recordingPath},
				{"bytes", double(QFileInfo(recordingPath).size())}, {"encodedFrames", int(encodedFrames)},
				{"cancelKeptRecordingActive", passedCancellation}, {"softwareFallback", fallback},
				{"initialEncoderError", startError}, {"forcedCleanup", forcedStop}}).toJson());
			check(report.commit(), "Local recording workflow evidence is saved in the disposable profile");
		} else {
			check(false, "Local recording workflow evidence file can be created");
		}
	}

	void finish()
	{
		if (completed)
			return;
		completed = true;
		poll.stop();
		if (frontend && !frontend->isVisible())
			frontend->show();
		for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
			if (it->hadUserValue)
				config_set_string(it->config, it->section, it->name, it->value.c_str());
			else
				config_remove_value(it->config, it->section, it->name);
		}
		if (main && outputsRebuilt && !recordingActive()) {
			recordingOutput = nullptr;
			try {
				main->ResetOutputs();
			} catch (...) {
				check(false, "Original disposable output configuration can be restored");
			}
		}
		writeEvidence();
		// Return to the harness after any QMessageBox::exec caller unwinds.
		QTimer::singleShot(0, this, [this] {
			done();
			deleteLater();
		});
	}

	void fail(const char *label)
	{
		if (completed || stage == Stage::Cleanup)
			return;
		check(false, label);
		failed = true;
		enter(Stage::Cleanup);
		if (auto *box = messageBox())
			box->done(QMessageBox::No);
		if (attemptedRecording && recordingActive())
			obs_output_stop(recordingOutput);
	}

	void startAttempt(bool useSoftware)
	{
		fallback = useSoftware;
		startReturned = false;
		startedSignal = false;
		stoppedSignal = false;
		auto *config = main->Config();
		try {
			if (useSoftware) {
				setString(config, "Output", "Mode", "Simple");
				setString(config, "SimpleOutput", "StreamEncoder", "x264");
				setString(config, "SimpleOutput", "RecEncoder", "x264_lowcpu");
				setString(config, "SimpleOutput", "RecQuality", "Small");
				setString(config, "SimpleOutput", "RecFormat2", "mkv");
				setString(config, "SimpleOutput", "RecTracks", "1");
				setBool(config, "SimpleOutput", "RecRB", false);
				outputsRebuilt = true;
				main->ResetOutputs();
			}
			recordingOutput = OBSOutputAutoRelease(obs_frontend_get_recording_output()).Get();
		} catch (...) {
			if (!useSoftware) {
				startAttempt(true);
				return;
			}
			fail("Software recording output can be initialized in the disposable profile");
			return;
		}
		if (!recordingOutput) {
			fail("Native recording output exists");
			return;
		}
		enter(Stage::Starting);
		attemptedRecording = true;
		QTimer::singleShot(0, this, [this] {
			if (completed || !main || stage != Stage::Starting)
				return;
			nativeCall = true;
			main->StartRecording();
			nativeCall = false;
			startReturned = true;
		});
	}

	void askStop(bool accept)
	{
		enter(accept ? Stage::AcceptDialog : Stage::RejectDialog);
		QTimer::singleShot(0, this, [this] {
			if (completed || !main || (stage != Stage::RejectDialog && stage != Stage::AcceptDialog))
				return;
			nativeCall = true;
			const bool invoked = QMetaObject::invokeMethod(main, "RecordActionTriggered", Qt::DirectConnection);
			nativeCall = false;
			if (!invoked)
				fail("Native recording action is callable from the integration harness");
		});
	}

	void verifyFile()
	{
		BPtr<char> path = obs_frontend_get_last_recording();
		recordingPath = QString::fromUtf8(path.Get() ? path.Get() : "");
		const QFileInfo file(recordingPath);
		const auto relative = QDir(directory).relativeFilePath(file.absoluteFilePath());
		const bool contained = !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
		check(main->isVisible() && frontend == main && central && central->isVisible() && main->centralWidget() == central,
		      "Recording stop keeps the original OBS shell and its WebView2 central surface visible");
		check(stoppedSignal && !recordingActive(), "Accepting the stop confirmation stops the native recording");
		check(contained && file.isFile() && file.size() > 0 && encodedFrames > 0,
		      "Real encoded recording persists as a nonempty file under the disposable test-recordings directory");
		if (!contained || !file.isFile() || file.size() <= 0 || !encodedFrames || !stoppedSignal)
			failed = true;
		blog(LOG_INFO, "[WebView2 output test] file='%s', bytes=%lld, frames=%u", recordingPath.toUtf8().constData(),
		     static_cast<long long>(file.size()), encodedFrames);
		finish();
	}

	void tick()
	{
		if (completed)
			return;
		if (!main || !frontend || !central) {
			fail("Recording workflow retains its native shell and WebView2 central surface");
			finish();
			return;
		}
		if (stage != Stage::Cleanup && total.elapsed() > 75000) {
			fail("Local output workflow completes within its time limit");
			return;
		}
		if (stage == Stage::Cleanup) {
			if (auto *box = messageBox())
				box->done(QMessageBox::No);
			if (nativeCall)
				return;
			if (!recordingActive()) {
				finish();
			} else if (elapsed.elapsed() > 4000 && !forcedStop) {
				forcedStop = true;
				obs_output_force_stop(recordingOutput);
			} else if (elapsed.elapsed() > 10000) {
				check(false, "Timed-out disposable recording stops after forced cleanup");
				finish();
			}
			return;
		}
		if (stage == Stage::Hide) {
			if (!frontend->isVisible()) {
				check(frontend == main && main->FrontendWindow() == main && !main->IsFrontendVisible() &&
				              !central->isVisible() && main->centralWidget() == central,
				      "Hiding the original shell also hides WebView2 while preserving its central ownership");
				check(QMetaObject::invokeMethod(main, "SetShowing", Qt::DirectConnection, Q_ARG(bool, true)),
				      "Native show operation is callable through its Qt slot");
				enter(Stage::Show);
			} else if (elapsed.elapsed() > 5000) {
				fail("SetShowing(false) hides the WebView2 session window");
			}
			return;
		}
		if (stage == Stage::Show) {
			if (frontend->isVisible() && main->isVisible() && central->isVisible()) {
				check(main->IsFrontendVisible() && main->FrontendWindow() == main && main->centralWidget() == central,
				      "SetShowing(true) restores the original OBS shell with the same WebView2 central surface");
				const bool simple = QByteArray(config_get_string(main->Config(), "Output", "Mode")) == "Simple";
				startAttempt(!simple);
			} else if (elapsed.elapsed() > 5000) {
				fail("SetShowing(true) restores the original shell and WebView2 central surface");
			}
			return;
		}
		if (stage == Stage::Starting) {
			if (auto *box = messageBox()) {
				if (startError.isEmpty())
					startError = box->text();
				box->reject();
			}
			if (recordingActive() && startedSignal && !nativeCall) {
				check(true, "Native local recording starts successfully");
				enter(Stage::Recording);
			} else if (!nativeCall && ((startReturned && !recordingActive()) || elapsed.elapsed() > 15000)) {
				if (!fallback && !recordingActive())
					startAttempt(true);
				else
					fail("Native local recording becomes active within the startup timeout");
			}
			return;
		}
		if (stage == Stage::Recording) {
			if (!recordingActive()) {
				fail("Local recording stays active before stop confirmation");
			} else if (elapsed.elapsed() > 1800) {
				encodedFrames = obs_output_get_total_frames(recordingOutput);
				askStop(false);
			}
			return;
		}
		if (stage == Stage::RejectDialog || stage == Stage::AcceptDialog) {
			if (auto *box = messageBox()) {
				if (box->windowTitle() != QTStr("ConfirmStopRecord.Title") || !box->button(QMessageBox::Yes) ||
				    !box->button(QMessageBox::No)) {
					fail("Stopping recording displays the expected native confirmation");
					return;
				}
				check(frontend == main && main->isVisible() && central->isVisible() && main->IsFrontendVisible(),
				      "Stop-recording confirmation appears over the visible original OBS shell and WebView2");
				check(box->parentWidget() == frontend, "Stop-recording confirmation is owned by the session window");
				const bool accept = stage == Stage::AcceptDialog;
				enter(accept ? Stage::Stopping : Stage::RejectUnwind);
				box->button(accept ? QMessageBox::Yes : QMessageBox::No)->click();
			} else if (elapsed.elapsed() > 8000) {
				fail("Recording stop does not bypass its enabled confirmation");
			}
			return;
		}
		if (stage == Stage::RejectUnwind && !nativeCall && elapsed.elapsed() > 250) {
			passedCancellation = recordingActive();
			check(passedCancellation, "Declining the first stop confirmation leaves the recording active");
			if (!passedCancellation)
				fail("Cancelled recording stop preserves the running output");
			else
				askStop(true);
		} else if (stage == Stage::Stopping) {
			if (auto *box = messageBox(); box && !nativeCall) {
				fail("Accepted recording stop completes without a native error dialog");
			} else if (!nativeCall && !recordingActive() && stoppedSignal) {
				verifyFile();
			} else if (elapsed.elapsed() > 15000) {
				fail("Accepted local recording stops within its timeout");
			}
		}
	}

public:
	OutputWorkflowChecks(OBSBasic *window, std::function<void(bool, const char *)> report, std::function<void()> next)
		: QObject(window), main(window),
		  check([this, report = std::move(report)](bool passed, const char *label) {
			  failed |= !passed;
			  report(passed, label);
		  }),
		  done(std::move(next)), poll(this)
	{
		poll.setInterval(40);
		connect(&poll, &QTimer::timeout, this, [this] { tick(); });
		if (window) {
			connect(window, &OBSBasic::RecordingStarted, this, [this](bool) { startedSignal = true; });
			connect(window, &OBSBasic::RecordingStopped, this, [this] { stoppedSignal = true; });
		}
	}

	void run()
	{
		const auto arguments = QCoreApplication::arguments();
		if (!main || !arguments.contains("--portable") || !arguments.contains("--webview2-self-test") ||
		    !arguments.contains("--only-bundled-plugins")) {
			check(false, "Output workflow runs only in an explicitly disposable portable self-test process");
			finish();
			return;
		}
		frontend = main->FrontendWindow();
		central = main->centralWidget();
		if (!frontend || frontend != main || !frontend->isVisible() || !central || !central->isVisible() ||
		    central->objectName() != QStringLiteral("obsWebView2Window") || central->isWindow() ||
		    !main->property("webview2NativeDocking").toBool() || main->Active() ||
		    main->RecordingActive() || obs_frontend_streaming_active() || obs_frontend_replay_buffer_active() ||
		    obs_frontend_virtualcam_active()) {
			check(false, "Output fixture starts with WebView2 in the visible original OBS shell and idle outputs");
			finish();
			return;
		}
		std::set<obs_source_t *> visited;
		bool hasColor = false;
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		bool safe = colorSceneOnly(scene, visited, hasColor) && hasColor;
		for (uint32_t channel = 1; channel < MAX_CHANNELS; ++channel) {
			OBSSourceAutoRelease source = obs_get_output_source(channel);
			safe &= !source;
		}
		if (!safe) {
			check(false, "Local recording fixture contains only color video and no global capture or audio devices");
			finish();
			return;
		}
		check(true, "Local recording fixture contains only color video and no global capture or audio devices");
		{
			QWidget nestedParent(main);
			QWidget nested(&nestedParent);
			nested.setObjectName("obsWebView2Window");
			nested.setProperty("webview2OwnsSession", true);
			QWidget secondary(main);
			secondary.setObjectName("obsWebView2Window");
			check(main->FrontendWindow() == main, "Nested or non-owning WebView2 widgets cannot take over native shell identity");
			secondary.setProperty("webview2OwnsSession", true);
			check(main->FrontendWindow() == main && main->centralWidget() == central,
			      "Native docking keeps the original shell identity even when another WebView2 child claims ownership");
		}
		BPtr<char> root = GetAppConfigPathPtr("obs-studio/test-recordings");
		directory = QString::fromUtf8(root.Get() ? root.Get() : "");
		if (directory.isEmpty() || !QDir().mkpath(directory)) {
			check(false, "Disposable test-recordings directory can be created");
			finish();
			return;
		}
		setString(main->Config(), "SimpleOutput", "FilePath", directory.toUtf8());
		setString(main->Config(), "Output", "FilenameFormatting", ("WebView2-output-" + runId).toUtf8());
		setBool(main->Config(), "Output", "OverwriteIfExists", false);
		setBool(main->Config(), "Video", "AutoRemux", false);
		setBool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord", true);
		check(config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord"),
		      "Native stop-recording confirmation is enabled for the workflow test");
		total.start();
		check(QMetaObject::invokeMethod(main, "SetShowing", Qt::DirectConnection, Q_ARG(bool, false)),
		      "Native hide operation is callable through its Qt slot");
		enter(Stage::Hide);
		poll.start();
	}
};
} // namespace

static void RunOutputWorkflowChecks(OBSBasic *main, const std::function<void(bool, const char *)> &check,
				    const std::function<void()> &done)
{
	auto *test = new OutputWorkflowChecks(main, check, done);
	QTimer::singleShot(0, test, [test] { test->run(); });
}
