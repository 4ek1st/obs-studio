#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <Windows.h>
#include <dwmapi.h>
#include <iostream>

// Sample the composed desktop, including the WebView child HWND. QWidget::grab
// only sees Qt's backing store and cannot detect the blank Chromium startup frame.
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (argc < 3 || argc > 4) return 2;
	const bool expectFallback = argc == 4 && QByteArray(argv[3]) == "--expect-fallback";
	const QString assets = QString::fromLocal8Bit(argv[1]);
	const QString output = QString::fromLocal8Bit(argv[2]);
	QDir().mkpath(output);
	QApplication::setStyle("Fusion");
	QPalette palette;
	palette.setColor(QPalette::Window, QColor("#1d2028"));
	palette.setColor(QPalette::WindowText, Qt::white);
	palette.setColor(QPalette::Base, QColor("#272a32"));
	palette.setColor(QPalette::Text, Qt::white);
	palette.setColor(QPalette::Button, QColor("#3a3d47"));
	palette.setColor(QPalette::ButtonText, Qt::white);
	app.setPalette(palette);
	QFontDatabase::addApplicationFont(QString::fromUtf8(OBS_DIALOG_TEST_FONT));
	app.setFont(QFont("Open Sans", 12));
	QTemporaryDir profile;
	QMainWindow main; main.resize(850, 610); main.setWindowFlag(Qt::WindowStaysOnTopHint);
	const BOOL disableTransitions = TRUE;
	DwmSetWindowAttribute(reinterpret_cast<HWND>(main.winId()), DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
	main.show();
	QObject owner;
	InstallWebView2Dialogs(&owner, assets, profile.path());
	QDialog dialog(&main); dialog.setWindowTitle("OBS opening presentation regression"); dialog.setFixedSize(580, 410);
	auto *layout = new QVBoxLayout(&dialog);
	auto *heading = new QLabel("Complete window from the first frame");
	heading->setFont(QFont("Open Sans", 17, QFont::Bold)); layout->addWidget(heading);
	for (int i = 0; i < 4; ++i) layout->addWidget(new QCheckBox(QString("Stable option %1 - preview and controls remain visible").arg(i + 1)));
	auto *edit = new QLineEdit("Preserved input and selection"); layout->addWidget(edit);
	auto *button = new QPushButton("Close"); layout->addWidget(button);
	QObject::connect(button, &QPushButton::clicked, &dialog, &QDialog::reject);
	QJsonArray samples;
	QJsonArray headingChanges;
	QImage firstHeading, lastHeading;
	QElapsedTimer elapsed; elapsed.start();
	int round = 0, blank = 0, white = 0, frames = 0, occluded = 0, presented = 0, fallback = 0;
	int firstFrame = 0;
	qint64 shownAt = 0;
	QTimer sample;
	auto show = [&] {
		DwmSetWindowAttribute(reinterpret_cast<HWND>(dialog.winId()), DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
		shownAt = elapsed.elapsed(); firstFrame = frames; firstHeading = {}; lastHeading = {};
		dialog.show(); dialog.raise(); dialog.activateWindow();
		edit->setFocus(); edit->selectAll();
	};
	QObject::connect(&sample, &QTimer::timeout, &app, [&] {
		const auto now = elapsed.elapsed();
		if (!dialog.isVisible()) return;
		const auto bounds = QRect(dialog.mapToGlobal(QPoint()), dialog.size());
		RECT physical{}; GetClientRect(reinterpret_cast<HWND>(dialog.winId()), &physical);
		POINT origin{}; ClientToScreen(reinterpret_cast<HWND>(dialog.winId()), &origin);
		const QRect physicalBounds(origin.x, origin.y, physical.right, physical.bottom);
		bool owned = true;
		for (auto point : {physicalBounds.topLeft(), physicalBounds.topRight(), physicalBounds.bottomLeft(), physicalBounds.bottomRight(), physicalBounds.center()}) {
			DWORD pid = 0; GetWindowThreadProcessId(GetAncestor(WindowFromPoint({point.x(), point.y()}), GA_ROOT), &pid);
			if (pid != GetCurrentProcessId()) owned = false;
		}
		if (!owned) { ++occluded; return; }
		if (!dialog.property("webview2Opening").toBool()) {
			const auto screenOrigin = bounds.topLeft() - dialog.screen()->geometry().topLeft();
			const auto image = dialog.screen()->grabWindow(0, screenOrigin.x(), screenOrigin.y(), bounds.width(), bounds.height()).toImage().convertToFormat(QImage::Format_RGB32);
			const qreal scale = qreal(image.width()) / dialog.width();
			const auto titleRect = heading->geometry();
			lastHeading = image.copy(QRect(qRound(titleRect.x()*scale), qRound(titleRect.y()*scale),
				qRound(titleRect.width()*scale), qRound(titleRect.height()*scale)));
			if (firstHeading.isNull()) firstHeading = lastHeading;
			int bright = 0, pale = 0;
			for (int y = 0; y < image.height(); y += 2) {
				const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
				for (int x = 0; x < image.width(); x += 2) {
					bright += qGray(line[x]) > 150;
					pale += qGray(line[x]) > 235;
				}
			}
			const bool empty = bright < 180;
			const bool flash = pale > image.width() * image.height() / 8;
			blank += empty; white += flash;
			if (frames == firstFrame || now - shownAt < 750 || empty || flash || now - shownAt > (expectFallback ? 3600 : 1750))
				if (!image.save(output + QString("/round-%1-frame-%2.png").arg(round).arg(frames, 4, 10, QLatin1Char('0')))) { app.exit(2); return; }
			samples.append(QJsonObject{{"round", round}, {"ms", now - shownAt}, {"bright", bright}, {"blank", empty}, {"white", flash}});
			++frames;
		}
		if (now - shownAt > (expectFallback ? 3750 : 1850)) {
			int changed = 0;
			if (firstHeading.size() != lastHeading.size()) changed = 1000000;
			else for (int y=0; y<firstHeading.height(); ++y) {
				const auto *first = reinterpret_cast<const QRgb *>(firstHeading.constScanLine(y));
				const auto *last = reinterpret_cast<const QRgb *>(lastHeading.constScanLine(y));
				for (int x=0; x<firstHeading.width(); ++x) changed += std::abs(qGray(first[x])-qGray(last[x])) > 30;
			}
			headingChanges.append(changed);
			auto *surface = dialog.findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
			presented += surface && surface->property("webview2Presented").toBool() && !dialog.property("webview2NativeFallback").toBool();
			fallback += dialog.property("webview2NativeFallback").toBool() && surface && !surface->isVisible() && dialog.windowOpacity() == 1;
			dialog.reject(); ++round;
			if (round < (expectFallback ? 1 : 3)) { QTimer::singleShot(150, &app, show); return; }
			bool stable = true;
			for (const auto &pixels : headingChanges) stable = stable && pixels.toInt() <= 100;
			const bool pass = blank == 0 && white == 0 && frames > 30 && occluded == 0 && stable &&
				(expectFallback ? fallback == 1 : presented == 3) && edit->text() == "Preserved input and selection";
			QFile report(output + "/presentation.json"); if (!report.open(QIODevice::WriteOnly)) { app.exit(2); return; }
			report.write(QJsonDocument(QJsonObject{{"pass", pass}, {"blankFrames", blank}, {"whiteFrames", white}, {"frames", frames}, {"occluded", occluded}, {"presented", presented}, {"fallback", fallback}, {"headingChanges", headingChanges}, {"samples", samples}}).toJson());
			std::cout << (pass ? "PASS" : "FAIL") << ": frames=" << frames << " blank=" << blank << " white=" << white << " occluded=" << occluded << " presented=" << presented << std::endl;
			app.exit(pass ? 0 : 1);
		}
	});
	sample.setTimerType(Qt::PreciseTimer); sample.start(16);
	QTimer::singleShot(200, &app, show);
	QTimer::singleShot(12000, &app, [&] { std::cerr << "FAIL: presentation timeout frames=" << frames << " occluded=" << occluded << std::endl; app.exit(1); });
	return app.exec();
}
