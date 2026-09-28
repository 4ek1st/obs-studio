#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <Windows.h>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (argc < 2 || argc > 3) return 2;
	const QString expectedFile = argc == 3 ? QFileInfo(QString::fromLocal8Bit(argv[2])).canonicalFilePath() : QString();
	if (argc == 3 && expectedFile.isEmpty()) return 2;
	QTemporaryDir temporary;
	if (!temporary.isValid()) return 2;
	QDialog dialog;
	dialog.resize(640,400); dialog.show();
	WebView2Widget widget(&dialog, QString::fromLocal8Bit(argv[1]), temporary.path()+"/profile");
	// Match a dialog overlay whose controller starts at the native child's
	// default size, then receives its final geometry only when the page is ready.
	widget.resize(100,30); widget.hide();
	bool captured = false, received = false, rejected = false, fileReceived = expectedFile.isEmpty();
	auto finish = [&] { if (captured && received && rejected && fileReceived) { std::cout << "PASS: rendered PNG, external text, rejected invented paths, and requested native file round trip\n"; app.exit(0); } };
	QObject::connect(&widget,&WebView2Widget::ready,&app,[&] {
		widget.setGeometry(dialog.rect()); widget.show(); widget.raise();
		QTimer::singleShot(500,&widget,[&] {
			const QString path = temporary.path()+"/capture.png";
			widget.capturePreview(path,[&,path](bool ok) {
				QImage image(path);
				RECT client{}; GetClientRect(reinterpret_cast<HWND>(widget.winId()), &client);
				const QSize expected(qRound(widget.width()*widget.devicePixelRatioF()),qRound(widget.height()*widget.devicePixelRatioF()));
				std::cout << "child bounds Qt=" << widget.width() << 'x' << widget.height() << " Win32=" << client.right << 'x' << client.bottom
					<< " PNG=" << image.width() << 'x' << image.height() << " expected=" << expected.width() << 'x' << expected.height() << '\n';
				if (!ok || image.isNull() || qAbs(image.width()-expected.width())>2 || qAbs(image.height()-expected.height())>2 || image.pixelColor(50,50).red()<180) {
					std::cerr << "FAIL: WebView2 capture must contain the red rendered fixture\n"; app.exit(1); return;
				}
				captured = true; finish();
			});
		});
		widget.postMessage({{"event","test.drop"}});
	});
	QObject::connect(&widget,&WebView2Widget::externalDrop,&app,[&](const QString &id,const OBSWeb::ExternalDropData &drop) {
		if (id == "fixture-file" && !expectedFile.isEmpty()) {
			if (drop.urls.size() != 1 || !drop.text.isEmpty() ||
			    QFileInfo(drop.urls[0].toLocalFile()).canonicalFilePath().compare(expectedFile, Qt::CaseInsensitive) != 0) {
				std::cerr << "FAIL: native File object path did not match the selected file\n"; app.exit(1); return;
			}
			fileReceived = true;
		} else if (id == "fixture-text" && drop.text == "External text from WebView2" && drop.urls.isEmpty()) {
			received = true;
		} else {
			std::cerr << "FAIL: untrusted drop reached the native import signal\n"; app.exit(1); return;
		}
		widget.postMessage({{"version",1},{"id",id},{"ok",true}});
		finish();
	});
	QObject::connect(&widget,&WebView2Widget::messageReceived,&app,[&](const QJsonObject &request) {
		if (request.value("command").toString() != "test.validation") { app.exit(1); return; }
		rejected = request.value("args").toObject().value("rejected").toBool();
		if (!rejected) { std::cerr << "FAIL: forged path accepted\n"; app.exit(1); return; }
		finish();
	});
	QObject::connect(&widget,&WebView2Widget::failed,&app,[&](const QString &error){std::cerr << error.toStdString() << '\n';app.exit(1);});
	QTimer::singleShot(15000,&app,[&]{std::cerr << "FAIL: drop/capture timed out\n";app.exit(1);});
	return app.exec();
}
