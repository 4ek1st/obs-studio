#include "TwitchDeviceFlow.hpp"
#include "TwitchTokenStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSslSocket>
#include <QTimer>
#include <iostream>

// Explicit, opt-in live probe. It never starts a broadcast or prints tokens/stream keys.
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (app.arguments().size() != 4) return 2;
	const auto mode = app.arguments()[1];
	if (mode == "probe") QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath()});
	const auto client = app.arguments()[2];
	QDir directory(app.arguments()[3]);
	if (!directory.exists()) return 2;
	twitch::DeviceFlow flow;
	auto write = [&](const QString &name, const QByteArray &bytes) {
		QSaveFile file(directory.filePath(name));
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
	};
	QObject::connect(&flow, &twitch::DeviceFlow::codeReady, &app, [&](const QUrl &url, const QString &code) {
		if (mode == "probe") {
			flow.cancel();
			const QJsonObject report{{"deviceEndpointReached", true}, {"tlsBackend", QSslSocket::activeBackend()},
				{"clientId", client}, {"authenticated", false}};
			write("probe-report.json", QJsonDocument(report).toJson());
			std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
			app.exit(0);
			return;
		}
		write("authorization.json", QJsonDocument(QJsonObject{{"url", url.toString()}, {"code", code}}).toJson());
		std::cout << "Waiting for Twitch approval in browser.\n" << std::flush;
	});
	QObject::connect(&flow, &twitch::DeviceFlow::credentialsChanged, &app, [&] {
		auto encrypted = twitch::protectCredentials(flow.credentials());
		if (!encrypted.isEmpty()) write("session.dpapi", encrypted);
	});
	QObject::connect(&flow, &twitch::DeviceFlow::authenticated, &app, [&] {
		auto credentials = flow.credentials();
		const QJsonObject report{{"authenticated", true}, {"login", credentials.login}, {"clientId", credentials.clientId},
			{"streamKeyReceived", !flow.streamKey().isEmpty()}, {"refreshTokenReceived", !credentials.refreshToken.isEmpty()}, {"mode", mode}};
		write(mode + "-report.json", QJsonDocument(report).toJson());
		std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
		app.exit(0);
	});
	QObject::connect(&flow, &twitch::DeviceFlow::failed, &app, [&](const QString &message, bool) {
		std::cerr << message.toStdString() << '\n'; app.exit(1);
	});
	QTimer::singleShot(15 * 60 * 1000, &app, [&] { app.exit(3); });
	QTimer::singleShot(0, &app, [&] {
		if (mode == "login" || mode == "probe") flow.begin(client);
		else if (mode == "restore" || mode == "refresh") {
			QFile file(directory.filePath("session.dpapi"));
			if (!file.open(QIODevice::ReadOnly)) { app.exit(2); return; }
			auto credentials = twitch::unprotectCredentials(file.readAll());
			if (!credentials || credentials->clientId != client) { app.exit(2); return; }
			if (mode == "refresh") credentials->accessToken.clear();
			flow.restore(*credentials);
		} else app.exit(2);
	});
	return app.exec();
}
