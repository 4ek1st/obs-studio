#include "TwitchDeviceLogin.hpp"
#include "TwitchDeviceFlow.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace twitch {

DeviceLogin::DeviceLogin(DeviceFlow &flow, const QString &clientId, QWidget *parent, Translate translate)
	: QDialog(parent)
{
	auto text = [translate](const char *key, const char *fallback) {
		return translate ? translate(key) : QString::fromUtf8(fallback);
	};
	setObjectName("TwitchDeviceLogin");
	setWindowTitle(text("TwitchAuth.Device.Title", "Connect Twitch"));
	setMinimumWidth(420);
	resize(480, 290);
	auto layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 24, 24, 24);
	layout->setSpacing(14);
	auto description = new QLabel(text("TwitchAuth.Device.Description", "Confirm the connection on Twitch in your browser. OBS will receive your stream key automatically."), this);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto applicationLabel = new QLabel(text("TwitchAuth.Device.ClientId", "Twitch application Client ID (Public)"), this);
	auto client = new QLineEdit(clientId, this);
	client->setObjectName("twitchClientId");
	client->setMaxLength(128);
	client->setPlaceholderText("Client ID");
	layout->addWidget(applicationLabel);
	layout->addWidget(client);
	auto changeApplication = new QPushButton(text("TwitchAuth.Device.ChangeApplication", "Change application"), this);
	changeApplication->setFlat(true);
	layout->addWidget(changeApplication, 0, Qt::AlignLeft);
	applicationLabel->setVisible(clientId.isEmpty());
	client->setVisible(clientId.isEmpty());
	changeApplication->setVisible(!clientId.isEmpty());
	connect(changeApplication, &QPushButton::clicked, this, [=] {
		applicationLabel->show(); client->show(); client->setFocus(); changeApplication->hide();
	});
	auto status = new QLabel(this);
	status->setObjectName("twitchLoginStatus");
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	layout->addWidget(status);
	auto userCode = new QLineEdit(this);
	userCode->setObjectName("twitchUserCode");
	userCode->setReadOnly(true);
	userCode->setAlignment(Qt::AlignCenter);
	auto font = userCode->font();
	font.setPointSize(font.pointSize() + 6);
	font.setBold(true);
	userCode->setFont(font);
	userCode->hide();
	layout->addWidget(userCode);
	auto open = new QPushButton(text("TwitchAuth.Device.OpenBrowser", "Open Twitch in browser"), this);
	open->setObjectName("twitchOpenBrowser");
	open->hide();
	layout->addWidget(open);
	auto buttons = new QDialogButtonBox(this);
	auto start = buttons->addButton(text("TwitchAuth.Device.Connect", "Connect account"), QDialogButtonBox::ActionRole);
	start->setObjectName("twitchConnect");
	auto cancel = buttons->addButton(text("Cancel", "Cancel"), QDialogButtonBox::RejectRole);
	cancel->setObjectName("twitchCancel");
	layout->addWidget(buttons);
	auto valid = [client] { return QRegularExpression("^[a-zA-Z0-9]{1,128}$").match(client->text().trimmed()).hasMatch(); };
	start->setEnabled(valid());
	connect(client, &QLineEdit::textChanged, this, [=] { start->setEnabled(client->isEnabled() && valid()); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(this, &QDialog::rejected, &flow, &DeviceFlow::cancel);
	connect(start, &QPushButton::clicked, this, [=, &flow] {
		start->setEnabled(false);
		client->setEnabled(false);
		changeApplication->setEnabled(false);
		status->setText(text("TwitchAuth.Device.Connecting", "Contacting Twitch…"));
		userCode->hide();
		open->hide();
		flow.begin(client->text());
	});
	connect(&flow, &DeviceFlow::codeReady, this, [=](const QUrl &url, const QString &code) {
		status->setText(text("TwitchAuth.Device.Waiting", "Confirm access in your browser. If Twitch asks for a code, use the one below. This window will close when the connection is complete."));
		userCode->setText(code);
		userCode->show();
		open->setProperty("verificationUrl", url);
		open->show();
		emit openBrowser(url);
	});
	connect(open, &QPushButton::clicked, this, [=] { emit openBrowser(open->property("verificationUrl").toUrl()); });
	connect(&flow, &DeviceFlow::authenticated, this, &QDialog::accept);
	connect(&flow, &DeviceFlow::failed, this, [=](const QString &message, bool) {
		status->setText(message);
		client->setEnabled(true);
		changeApplication->setEnabled(true);
		start->setEnabled(valid());
		userCode->clear();
		userCode->hide();
		open->hide();
	});
}

} // namespace twitch
