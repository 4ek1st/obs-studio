#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QHash>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	const auto timingPath = temporary.filePath(QStringLiteral("startup.jsonl"));
	qputenv("OBS_WEBVIEW2_TRACE_PERFORMANCE", timingPath.toUtf8());
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
			stayedLocal = message.value("args").toObject().value("origin") == "https://obs-ui.example";
		if (echoed && stayedLocal) {
			QFile timings(timingPath);
			QHash<qint64, double> navigationStarts;
			double navigationMs = -1;
			if (timings.open(QIODevice::ReadOnly)) {
				while (!timings.atEnd()) {
					const auto entry = QJsonDocument::fromJson(timings.readLine()).object();
					const auto id = entry.value("host").toInteger();
					if (entry.value("stage") == "navigate") navigationStarts.insert(id, entry.value("elapsedMs").toDouble());
					if (entry.value("stage") == "navigation-completed" && navigationStarts.contains(id))
						navigationMs = entry.value("elapsedMs").toDouble() - navigationStarts.value(id);
				}
			}
			std::cout << "TIMING local_navigation_ms=" << navigationMs << '\n';
			if (navigationMs < 0 || navigationMs >= 1500) {
				std::cerr << "FAIL: local navigation must avoid the two-second .local resolution stall\n";
				app.exit(1);
				return;
			}
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
