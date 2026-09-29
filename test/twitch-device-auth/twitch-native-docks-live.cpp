#include "TwitchNativeDocks.hpp"
#include "TwitchTokenStore.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <iostream>

// Explicit live check: reads the encrypted local session, joins the owner's chat,
// and reads channel information. It never posts chat or changes channel settings.
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (app.arguments().size() != 2) return 2;
	QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath()});
	QFile file(app.arguments()[1]);
	if (!file.open(QIODevice::ReadOnly)) return 2;
	auto credentials = twitch::unprotectCredentials(file.readAll());
	if (!credentials) return 2;
	twitch::ChatDock chat;
	twitch::StreamInfoDock info;
	chat.setCredentials(*credentials);
	info.setCredentials(*credentials);
	auto composer = chat.findChild<QLineEdit *>("twitchChatComposer");
	auto infoStatus = info.findChild<QLabel *>("twitchInfoStatus");
	if (!composer || !infoStatus) return 2;
	QTimer poll;
	poll.setInterval(100);
	QObject::connect(&poll, &QTimer::timeout, &app, [&] {
		if (composer->isEnabled() && infoStatus->text() == "Current Twitch channel information.") app.exit(0);
	});
	poll.start();
	QTimer::singleShot(30000, &app, [&] { app.exit(3); });
	const int result = app.exec();
	const QJsonObject report{{"account", credentials->login}, {"chatJoined", composer->isEnabled()},
		{"streamInfoLoaded", infoStatus->text() == "Current Twitch channel information."},
		{"sentChatMessage", false}, {"modifiedChannel", false}};
	std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
	return result;
}
