#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	const auto assets = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
	// Close shortly after queued initialization begins. This does not pin a particular COM completion boundary.
	for (int n = 0; n < 4; ++n) {
		auto *closing = new WebView2Widget(nullptr, assets, temporary.path() + QStringLiteral("/closing%1").arg(n));
		QTimer::singleShot(0, closing, [closing] { delete closing; });
	}
	WebView2Widget widget(nullptr, assets, temporary.path() + QStringLiteral("/profile"));
	widget.resize(800, 500);
	bool echoed = false;
	bool stayedLocal = false;
	QObject::connect(&widget, &WebView2Widget::ready, &app, [&] {
		widget.postMessage(QJsonObject{{"event", "test.ping"}, {"token", "native-to-webview"}});
	});
	QObject::connect(&widget, &WebView2Widget::failed, &app, [&](const QString &error) {
		std::cerr << "FAIL: native WebView2 host: " << error.toStdString() << '\n';
		app.exit(1);
	});
	QObject::connect(&widget, &WebView2Widget::messageReceived, &app, [&](const QJsonObject &message) {
		const auto command = message.value("command").toString();
		if (command == "test.echo")
			echoed = message.value("args").toObject().value("token") == "native-to-webview";
		if (command == "test.stayedLocal")
			stayedLocal = message.value("args").toObject().value("origin") == "https://obs-ui.local";
		if (echoed && stayedLocal) {
			std::cout << "PASS: native WebView2 round trip, blocked navigation, early-close smoke\n";
			app.exit(0);
		}
	});
	QTimer::singleShot(20000, &app, [&] {
		std::cerr << "FAIL: native WebView2 handshake timed out\n";
		app.exit(1);
	});
	return app.exec();
}
