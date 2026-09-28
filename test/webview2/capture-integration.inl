// Real OBS properties and capture source regression. Only compiled into QA builds.
#include <QFile>
#include <QJsonDocument>
#include <QScrollArea>
#include <QScrollBar>
#include <widgets/OBSQTDisplay.hpp>
#include <Windows.h>

namespace {
class CaptureDialogChecks : public QObject {
	QPointer<OBSBasic> main;
	std::function<void(bool, const char *)> check;
	std::function<void()> done;
	OBSSourceAutoRelease source;
	QPointer<QDialog> dialog;
	int kind = 0;
	const char *types[4]{"game_capture", "window_capture", "wasapi_process_output_capture", "monitor_capture"};
	QString artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");

	void finishSource()
	{
		if (dialog) {
			// Dynamic capture properties may pick a default window themselves. Use
			// the real OK action so this disposable fixture cannot block on Discard.
			auto *box = dialog->findChild<QDialogButtonBox *>();
			if (box && box->button(QDialogButtonBox::Ok)) box->button(QDialogButtonBox::Ok)->click();
			else dialog->accept();
		}
		dialog = nullptr;
		if (source) { obs_source_dec_active(source); obs_source_dec_showing(source); }
		source = nullptr;
		if (++kind < 4) QTimer::singleShot(200, this, [this] { startSource(); });
		else { done(); deleteLater(); }
	}
	void inspect(const QString &stage, std::function<void()> next)
	{
		if (!dialog) { check(false, "Capture properties remain open"); finishSource(); return; }
		OBSWeb::QtDialogBridge bridge(dialog);
		const auto state = bridge.snapshot();
		QJsonObject evidence{{"snapshot", state}};
		if (auto *preview = dialog->findChild<OBSQTDisplay *>("preview"); preview && preview->isVisible()) {
			POINT origin{};
			MapWindowPoints(reinterpret_cast<HWND>(preview->winId()), reinterpret_cast<HWND>(dialog->winId()), &origin, 1);
			const auto position = preview->mapTo(dialog, QPoint());
			const qreal scale = preview->devicePixelRatioF();
			const bool aligned = qAbs(origin.x - qRound(position.x() * scale)) <= 1 &&
				qAbs(origin.y - qRound(position.y() * scale)) <= 1;
			check(aligned, "Real capture preview HWND origin matches its Qt layout origin");
			uint32_t displayWidth = 0, displayHeight = 0;
			if (preview->GetDisplay()) obs_display_size(preview->GetDisplay(), &displayWidth, &displayHeight);
			RECT client{}; GetClientRect(reinterpret_cast<HWND>(preview->winId()), &client);
			evidence.insert("preview", QJsonObject{{"qtX", position.x()}, {"qtY", position.y()},
				{"nativeX", int(origin.x)}, {"nativeY", int(origin.y)}, {"width", preview->width()}, {"height", preview->height()},
				{"displayWidth", int(displayWidth)}, {"displayHeight", int(displayHeight)},
				{"clientWidth", int(client.right)}, {"clientHeight", int(client.bottom)}});
			check(displayWidth == uint32_t(client.right) && displayHeight == uint32_t(client.bottom),
				"Capture preview swapchain dimensions match its HWND client area");
			auto *web = dialog->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
			const auto center = preview->mapTo(dialog, preview->rect().center());
			check(web && !web->mask().contains(center), "Dialog WebView mask leaves the actual GPU preview visible");
		}
		if (!artifacts.isEmpty()) {
			QDir().mkpath(artifacts);
			const auto stem = QString::fromLatin1(types[kind]) + "-" + stage;
			const auto screenOrigin = dialog->mapToGlobal(QPoint());
			const auto screenCenter = dialog->mapToGlobal(dialog->rect().center());
			const auto topWindow = WindowFromPoint(POINT{screenCenter.x(), screenCenter.y()});
			const auto ownWindow = reinterpret_cast<HWND>(dialog->winId());
			if (topWindow == ownWindow || IsChild(ownWindow, topWindow))
				dialog->screen()->grabWindow(0, screenOrigin.x(), screenOrigin.y(), dialog->width(), dialog->height())
					.save(QDir(artifacts).filePath(stem + "-composed.png"));
			QFile json(QDir(artifacts).filePath(stem + ".json"));
			if (json.open(QIODevice::WriteOnly)) json.write(QJsonDocument(evidence).toJson());
			auto *web = dialog->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
			if (web) { web->capturePreview(QDir(artifacts).filePath(stem + ".png"), [this, next](bool saved) {
				check(saved, "Capture properties HTML image saved for visual inspection"); next();
			}); return; }
		}
		next();
	}
	void loaded()
	{
		// Only this disposable QA dialog stays above unrelated apps during its
		// bounded visual capture. Never save pixels when another window occludes it.
		SetWindowPos(reinterpret_cast<HWND>(dialog->winId()), HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		dialog->resize(720, 580);
		QTimer::singleShot(500, this, [this] {
			inspect("compact-top", [this] {
				auto *scroll = dialog->findChild<QScrollArea *>();
				if (scroll) scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
				QTimer::singleShot(250, this, [this] {
					inspect("compact-bottom", [this] {
						dialog->resize(1000, 760);
						if (auto *scroll = dialog->findChild<QScrollArea *>()) scroll->verticalScrollBar()->setValue(0);
						QTimer::singleShot(300, this, [this] {
							inspect("large", [this] {
								if (kind >= 2 || qEnvironmentVariableIsEmpty("OBS_WEBVIEW2_CAPTURE_FIXTURE")) { finishSource(); return; }
								auto *poll = new QTimer(this); poll->setInterval(200);
								auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
								connect(poll, &QTimer::timeout, this, [this, poll, elapsed] {
									const auto width = obs_source_get_width(source), height = obs_source_get_height(source);
									if ((width && height) || elapsed->elapsed() > 20000) {
										poll->stop(); poll->deleteLater();
										blog(LOG_INFO, "[Capture QA] %s frame dimensions %ux%u", types[kind], width, height);
										check(width == 640 && height == 360, "Real graphics fixture is captured at 640x360 without a tiny or zero-size source");
										finishSource();
									}
								}); poll->start();
							});
						});
					});
				});
			});
		});
	}
	void startSource()
	{
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "capture_mode", "window");
		const auto target = qEnvironmentVariable("OBS_WEBVIEW2_CAPTURE_FIXTURE").toUtf8();
		if (!target.isEmpty() && kind < 3) obs_data_set_string(settings, "window", target.constData());
		if (kind == 1) { obs_data_set_int(settings, "method", 2); obs_data_set_bool(settings, "client_area", true); }
		source = obs_source_create_private(types[kind], "WebView capture QA", settings);
		check(bool(source), "Bundled capture source is available in the fork");
		if (!source) { finishSource(); return; }
		obs_source_inc_active(source); obs_source_inc_showing(source);
		auto *poll = new QTimer(this); poll->setInterval(100);
		auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
		connect(poll, &QTimer::timeout, this, [this, poll, elapsed] {
			for (auto *widget : QApplication::topLevelWidgets()) {
				if (!widget->inherits("OBSBasicProperties") || !widget->isVisible()) continue;
				auto *web = widget->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
				if (!web || !web->isVisible()) continue;
				dialog = qobject_cast<QDialog *>(widget);
				poll->stop(); poll->deleteLater(); loaded(); return;
			}
			if (elapsed->elapsed() > 20000) { poll->stop(); poll->deleteLater(); check(false, "Capture properties WebView opens"); finishSource(); }
		});
		poll->start();
		obs_frontend_open_source_properties(source);
	}
public:
	CaptureDialogChecks(OBSBasic *window, std::function<void(bool, const char *)> report, std::function<void()> complete)
		: QObject(window), main(window), check(std::move(report)), done(std::move(complete)) {}
	void run() { startSource(); }
};
}

static void RunCaptureDialogChecks(OBSBasic *main, const std::function<void(bool, const char *)> &check, const std::function<void()> &done)
{
	(new CaptureDialogChecks(main, check, done))->run();
}
