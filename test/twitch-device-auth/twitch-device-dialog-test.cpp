#include "TwitchDeviceFlow.hpp"
#include "TwitchDeviceLogin.hpp"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
	std::cerr << "Initializing QApplication\n";
	QApplication app(argc, argv);
	std::cerr << "Constructing Twitch dialog\n";
	twitch::DeviceFlow flow;
	twitch::DeviceLogin dialog(flow, {});
	int failed = 0;
	auto check = [&](bool value, const char *name) { std::cout << (value ? "PASS " : "FAIL ") << name << '\n'; if (!value) ++failed; };
	dialog.show();
	std::cerr << "Processing dialog events\n";
	app.processEvents();
	auto client = dialog.findChild<QLineEdit *>("twitchClientId");
	auto connect = dialog.findChild<QPushButton *>("twitchConnect");
	check(client && connect, "setup exposes Client ID and connect");
	if (!client || !connect) return 1;
	check(!connect->isEnabled(), "empty Client ID cannot start authorization");
	client->setText("testclient012345678901234567890");
	check(connect->isEnabled(), "Client ID enables connect");
	int browserRequests = 0;
	QObject::connect(&dialog, &twitch::DeviceLogin::openBrowser, &dialog, [&](const QUrl &url) { if (url.host() == "www.twitch.tv") ++browserRequests; });
	emit flow.codeReady(QUrl("https://www.twitch.tv/activate?device-code=TEST-CODE"), "TEST-CODE");
	app.processEvents();
	auto code = dialog.findChild<QLineEdit *>("twitchUserCode");
	check(code && code->isVisible() && code->text() == "TEST-CODE", "confirmation code is visible");
	check(browserRequests == 1, "confirmation opens browser once");
	emit flow.failed("The connection could not be completed.", false);
	check(connect->isEnabled(), "error allows retry without reopening settings");
	check(!code->isVisible(), "error hides expired authorization code");
	dialog.reject();
	check(dialog.result() == QDialog::Rejected, "cancel leaves account disconnected");
	return failed ? 1 : 0;
}
