#include "TwitchNativeDocks.hpp"

#include <QLabel>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace twitch {

static void cancelReply(QPointer<QNetworkReply> &reply)
{
	if (!reply) return;
	auto pending = reply;
	reply.clear();
	pending->abort();
	pending->deleteLater();
}

ChatLine parseChatLine(const QByteArray &raw)
{
	ChatLine event;
	QByteArray line = raw;
	while (line.endsWith('\r') || line.endsWith('\n')) line.chop(1);
	if (line.contains('\n') || line.contains('\r')) return event;
	QByteArray tags, prefix;
	int position = 0;
	if (line.startsWith('@')) {
		const int end = line.indexOf(' ');
		if (end < 0) return event;
		tags = line.mid(1, end - 1);
		position = end + 1;
	}
	if (line.mid(position, 1) == ":") {
		const int end = line.indexOf(' ', position);
		if (end < 0) return event;
		prefix = line.mid(position + 1, end - position - 1);
		position = end + 1;
	}
	const int commandEnd = line.indexOf(' ', position);
	const QByteArray command = commandEnd < 0 ? line.mid(position) : line.mid(position, commandEnd - position);
	const QByteArray parameters = commandEnd < 0 ? QByteArray{} : line.mid(commandEnd + 1);
	if (command == "PING" && parameters.startsWith(':')) event.pong = "PONG " + parameters + "\r\n";
	event.reconnect = command == "RECONNECT";
	event.welcomed = command == "001";
	if (command == "366" || command == "ROOMSTATE") {
		for (const auto &parameter : parameters.split(' ')) {
			if (parameter.startsWith('#')) {
				event.joinedChannel = parameter;
				break;
			}
		}
	}
	if (command != "PRIVMSG" && command != "NOTICE") return event;
	const int textStart = parameters.indexOf(" :");
	if (textStart < 0) return event;
	const QString body = QString::fromUtf8(parameters.mid(textStart + 2));
	if (command == "NOTICE") {
		event.notice = body;
		return event;
	}
	event.text = body;
	if (!tags.isEmpty()) {
		for (const auto &tag : tags.split(';')) {
			if (tag.startsWith("display-name=")) {
				event.sender = QString::fromUtf8(tag.mid(sizeof("display-name=") - 1));
				event.sender.replace("\\s", " ").replace("\\:", ";").replace("\\\\", "\\");
				break;
			}
		}
	}
	if (event.sender.isEmpty()) {
		const int nickEnd = prefix.indexOf('!');
		if (nickEnd > 0) event.sender = QString::fromUtf8(prefix.left(nickEnd));
	}
	return event;
}

QByteArray encodeChatMessage(const QString &channel, const QString &message)
{
	static const QRegularExpression validChannel("^[A-Za-z0-9_]{1,25}$");
	if (!validChannel.match(channel).hasMatch() || message.trimmed().isEmpty()) return {};
	for (const QChar character : message) if (character.unicode() < 0x20 || character.unicode() == 0x7f) return {};
	const auto encoded = message.toUtf8();
	if (encoded.size() > 500) return {};
	return "PRIVMSG #" + channel.toUtf8() + " :" + encoded + "\r\n";
}

ChatDock::ChatDock(QWidget *parent, Translate translate) : QDockWidget(parent), translator(translate)
{
	auto label = [this](const char *key, const char *fallback) { return text(key, fallback); };
	setWindowTitle(label("Auth.Chat", "Chat"));
	setMinimumSize(240, 300);
	auto contents = new QWidget(this);
	auto layout = new QVBoxLayout(contents);
	layout->setContentsMargins(8, 8, 8, 8);
	status = new QLabel(label("TwitchAuth.Native.ChatDisconnected", "Twitch chat is disconnected."), contents);
	status->setObjectName("twitchChatStatus");
	status->setWordWrap(true);
	layout->addWidget(status);
	messages = new QPlainTextEdit(contents);
	messages->setObjectName("twitchChatMessages");
	messages->setReadOnly(true);
	messages->document()->setMaximumBlockCount(500);
	layout->addWidget(messages, 1);
	auto composerRow = new QHBoxLayout;
	composer = new QLineEdit(contents);
	composer->setObjectName("twitchChatComposer");
	composer->setPlaceholderText(label("TwitchAuth.Native.ChatMessage", "Message to your chat"));
	composer->setEnabled(false);
	composerRow->addWidget(composer, 1);
	auto send = new QPushButton(label("TwitchAuth.Native.ChatSend", "Send"), contents);
	send->setObjectName("twitchChatSend");
	send->setEnabled(false);
	composerRow->addWidget(send);
	layout->addLayout(composerRow);
	setWidget(contents);
	connect(send, &QPushButton::clicked, this, [this] { sendChat(); });
	connect(composer, &QLineEdit::returnPressed, this, [this] { sendChat(); });
	connect(composer, &QLineEdit::textChanged, this, [this, send] {
		send->setEnabled(joined && !composer->text().trimmed().isEmpty());
	});
	reconnectTimer.setSingleShot(true);
	reconnectTimer.setInterval(5000);
	connect(&reconnectTimer, &QTimer::timeout, this, [this] { connectChat(); });
	connect(&socket, &QSslSocket::encrypted, this, [this] {
		reconnectTimer.stop();
		socket.write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
		socket.write("PASS oauth:" + credentials.accessToken.toUtf8() + "\r\n");
		socket.write("NICK " + credentials.login.toUtf8() + "\r\n");
		status->setText(text("TwitchAuth.Native.ChatAuthenticating", "Authenticating Twitch chat…"));
	});
	connect(&socket, &QSslSocket::readyRead, this, [this] { readChat(); });
	connect(&socket, &QSslSocket::disconnected, this, [this, send] {
		joined = false;
		composer->setEnabled(false);
		send->setEnabled(false);
		if (!stopRetry && !credentials.accessToken.isEmpty()) {
			status->setText(text("TwitchAuth.Native.ChatReconnecting", "Chat disconnected. Reconnecting…"));
			reconnectTimer.start();
		}
	});
	connect(&socket, &QSslSocket::errorOccurred, this, [this] {
		if (!stopRetry) {
			status->setText(text("TwitchAuth.Native.ChatConnectFailed", "Could not connect to Twitch chat. Retrying…"));
			reconnectTimer.start();
		}
	});
}

QString ChatDock::text(const char *key, const char *fallback) const
{
	return translator ? translator(key) : QString::fromUtf8(fallback);
}

void ChatDock::setCredentials(const Credentials &updated)
{
	if (credentials.accessToken == updated.accessToken && credentials.login == updated.login) return;
	credentials = updated;
	stopRetry = false;
	joined = false;
	reconnectTimer.stop();
	input.clear();
	socket.abort();
	composer->setEnabled(false);
	if (!credentials.accessToken.isEmpty() && !credentials.login.isEmpty()) connectChat();
	else status->setText(text("TwitchAuth.Native.ChatDisconnected", "Twitch chat is disconnected."));
}

void ChatDock::connectChat()
{
	reconnectTimer.stop();
	static const QRegularExpression validLogin("^[A-Za-z0-9_]{1,25}$");
	if (credentials.accessToken.isEmpty() || !validLogin.match(credentials.login).hasMatch()) {
		status->setText(text("TwitchAuth.Native.ChatNeedReconnect", "Reconnect Twitch to use chat."));
		return;
	}
	if (socket.state() != QAbstractSocket::UnconnectedState) socket.abort();
	joined = false;
	status->setText(text("TwitchAuth.Native.ChatConnecting", "Connecting to your Twitch chat…"));
	socket.connectToHostEncrypted("irc.chat.twitch.tv", 6697);
}

void ChatDock::readChat()
{
	input += socket.readAll();
	if (input.size() > 65536) {
		input.clear();
		socket.disconnectFromHost();
		return;
	}
	while (true) {
		const int end = input.indexOf("\r\n");
		if (end < 0) break;
		const auto line = input.left(end);
		input.remove(0, end + 2);
		const auto event = parseChatLine(line);
		if (!event.pong.isEmpty()) socket.write(event.pong);
		if (event.welcomed) socket.write("JOIN #" + credentials.login.toUtf8() + "\r\n");
		if (event.joinedChannel.compare("#" + credentials.login.toUtf8(), Qt::CaseInsensitive) == 0) {
			joined = true;
			composer->setEnabled(true);
			status->setText(text("TwitchAuth.Native.ChatConnected", "Connected to your Twitch chat."));
		}
		if (!event.notice.isEmpty()) {
			appendMessage(event.notice);
			if (event.notice.contains("Login authentication failed", Qt::CaseInsensitive)) {
				stopRetry = true;
				status->setText(text("TwitchAuth.Native.ChatLoginRejected", "Twitch rejected the chat login. Reconnect your account."));
				socket.disconnectFromHost();
			}
		}
		if (!event.sender.isEmpty() && !event.text.isEmpty())
			appendMessage(event.sender + ": " + event.text);
		if (event.reconnect) socket.disconnectFromHost();
	}
}

void ChatDock::sendChat()
{
	if (!joined || socket.state() != QAbstractSocket::ConnectedState) return;
	const auto line = encodeChatMessage(credentials.login, composer->text());
	if (line.isEmpty()) {
		status->setText(text("TwitchAuth.Native.ChatInvalidMessage", "Chat message is empty, too long, or contains an invalid character."));
		return;
	}
	socket.write(line);
	composer->clear();
}

void ChatDock::appendMessage(const QString &message)
{
	messages->appendPlainText(message);
}

StreamInfoDock::StreamInfoDock(const QUrl &base, QWidget *parent, Translate translate)
	: QDockWidget(parent), translator(translate), apiBase(base)
{
	auto label = [this](const char *key, const char *fallback) { return text(key, fallback); };
	setWindowTitle(label("Auth.StreamInfo", "Stream Information"));
	setMinimumSize(240, 300);
	auto contents = new QWidget(this);
	auto layout = new QVBoxLayout(contents);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);
	layout->addWidget(new QLabel(label("TwitchAuth.Native.Title", "Stream title"), contents));
	title = new QLineEdit(contents);
	title->setObjectName("twitchTitle");
	title->setMaxLength(140);
	layout->addWidget(title);
	layout->addWidget(new QLabel(label("TwitchAuth.Native.Category", "Category"), contents));
	category = new QLineEdit(contents);
	category->setObjectName("twitchCategory");
	category->setPlaceholderText(label("TwitchAuth.Native.SearchCategory", "Search Twitch categories"));
	layout->addWidget(category);
	results = new QListWidget(contents);
	results->setObjectName("twitchCategoryResults");
	results->setMaximumHeight(170);
	results->hide();
	layout->addWidget(results);
	auto buttons = new QHBoxLayout;
	auto reload = new QPushButton(label("TwitchAuth.Native.Reload", "Reload"), contents);
	reload->setObjectName("twitchReload");
	buttons->addWidget(reload);
	saveButton = new QPushButton(label("TwitchAuth.Native.Save", "Save changes"), contents);
	saveButton->setObjectName("twitchSave");
	saveButton->setEnabled(false);
	buttons->addWidget(saveButton);
	layout->addLayout(buttons);
	status = new QLabel(contents);
	status->setObjectName("twitchInfoStatus");
	status->setWordWrap(true);
	layout->addWidget(status);
	layout->addStretch();
	setWidget(contents);
	searchTimer.setSingleShot(true);
	searchTimer.setInterval(300);
	connect(&searchTimer, &QTimer::timeout, this, [this] { searchCategories(); });
	connect(title, &QLineEdit::textChanged, this, [this] { updateSaveButton(); });
	connect(category, &QLineEdit::textChanged, this, [this] {
		if (category->text() != selectedCategoryName) {
			results->clear();
			results->hide();
			searchTimer.start();
		} else {
			searchTimer.stop();
			cancelReply(searchReply);
			results->clear();
			results->hide();
		}
		updateSaveButton();
	});
	connect(results, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		selectedCategoryId = item->data(Qt::UserRole).toString();
		selectedCategoryName = item->text();
		searchTimer.stop();
		category->setText(selectedCategoryName);
		results->hide();
		updateSaveButton();
	});
	connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
	connect(saveButton, &QPushButton::clicked, this, [this] { save(); });
}
QString StreamInfoDock::text(const char *key, const char *fallback) const
{
	return translator ? translator(key) : QString::fromUtf8(fallback);
}

void StreamInfoDock::setCredentials(const Credentials &updated)
{
	const bool changedAccount = credentials.userId != updated.userId || credentials.clientId != updated.clientId;
	const bool unavailable = updated.accessToken.isEmpty() || updated.userId.isEmpty();
	const bool shouldRefresh = changedAccount || !loaded;
	credentials = updated;
	if (changedAccount || unavailable) {
		searchTimer.stop();
		cancelReply(channelReply);
		cancelReply(searchReply);
		cancelReply(saveReply);
		loaded = false;
		loadedTitle.clear();
		loadedCategoryId.clear();
		selectedCategoryId.clear();
		selectedCategoryName.clear();
		results->clear();
		results->hide();
		title->clear();
		category->clear();
		updateSaveButton();
	}
	if (unavailable) {
		status->setText(text("TwitchAuth.Native.InfoNeedConnect", "Connect your Twitch account to load stream information."));
		return;
	}
	if (shouldRefresh) refresh();
}

QNetworkRequest StreamInfoDock::request(const QUrl &url) const
{
	QNetworkRequest result(url);
	result.setTransferTimeout(15000);
	result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	result.setRawHeader("Client-ID", credentials.clientId.toUtf8());
	result.setRawHeader("Authorization", "Bearer " + credentials.accessToken.toUtf8());
	return result;
}

void StreamInfoDock::refresh()
{
	loaded = false;
	updateSaveButton();
	if (credentials.clientId.isEmpty() || credentials.accessToken.isEmpty() || credentials.userId.isEmpty()) {
		status->setText(text("TwitchAuth.Native.InfoNeedConnect", "Connect your Twitch account to load stream information."));
		return;
	}
	cancelReply(channelReply);
	status->setText(text("TwitchAuth.Native.InfoLoading", "Loading Twitch stream information…"));
	QUrl url = apiBase.resolved(QUrl("channels"));
	QUrlQuery query;
	query.addQueryItem("broadcaster_id", credentials.userId);
	url.setQuery(query);
	auto reply = network.get(request(url));
	channelReply = reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		if (channelReply != reply) return;
		channelReply.clear();
		const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->readAll();
		reply->deleteLater();
		const auto data = QJsonDocument::fromJson(body).object()["data"].toArray();
		if (http != 200 || data.isEmpty() || !data.first().isObject()) {
			status->setText(text("TwitchAuth.Native.InfoLoadFailed", "Could not load Twitch stream information. Check your connection or reconnect Twitch."));
			return;
		}
		const auto channel = data.first().toObject();
		loadedTitle = channel["title"].toString();
		loadedCategoryId = channel["game_id"].toString();
		selectedCategoryId = loadedCategoryId;
		selectedCategoryName = channel["game_name"].toString();
		searchTimer.stop();
		results->clear();
		results->hide();
		title->setText(loadedTitle);
		category->setText(selectedCategoryName);
		loaded = true;
		status->setText(text("TwitchAuth.Native.InfoLoaded", "Current Twitch channel information."));
		updateSaveButton();
	});
}

void StreamInfoDock::searchCategories()
{
	const QString term = category->text().trimmed();
	if (term.size() < 2 || credentials.accessToken.isEmpty()) return;
	cancelReply(searchReply);
	QUrl url = apiBase.resolved(QUrl("search/categories"));
	QUrlQuery query;
	query.addQueryItem("query", term);
	query.addQueryItem("first", "10");
	url.setQuery(query);
	auto reply = network.get(request(url));
	searchReply = reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply, term] {
		if (searchReply != reply) return;
		searchReply.clear();
		const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->readAll();
		reply->deleteLater();
		if (category->text().trimmed() != term) return;
		results->clear();
		if (http == 200) {
			for (const auto &value : QJsonDocument::fromJson(body).object()["data"].toArray()) {
				const auto item = value.toObject();
				if (item["id"].toString().isEmpty() || item["name"].toString().isEmpty()) continue;
				auto option = new QListWidgetItem(item["name"].toString(), results);
				option->setData(Qt::UserRole, item["id"].toString());
			}
		} else status->setText(text("TwitchAuth.Native.CategorySearchFailed", "Could not search Twitch categories."));
		results->setVisible(results->count() > 0);
	});
}

void StreamInfoDock::save()
{
	if (!loaded || saveReply || credentials.userId.isEmpty()) return;
	QJsonObject changes;
	const auto newTitle = title->text().trimmed();
	if (newTitle.isEmpty() || category->text() != selectedCategoryName) return;
	if (newTitle != loadedTitle) changes["title"] = newTitle;
	if (selectedCategoryId != loadedCategoryId) changes["game_id"] = selectedCategoryId.isEmpty() ? "0" : selectedCategoryId;
	if (changes.isEmpty()) return;
	QUrl url = apiBase.resolved(QUrl("channels"));
	QUrlQuery query;
	query.addQueryItem("broadcaster_id", credentials.userId);
	url.setQuery(query);
	auto apiRequest = request(url);
	apiRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	auto reply = network.sendCustomRequest(apiRequest, "PATCH", QJsonDocument(changes).toJson(QJsonDocument::Compact));
	saveReply = reply;
	status->setText(text("TwitchAuth.Native.InfoSaving", "Saving Twitch stream information…"));
	updateSaveButton();
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		if (saveReply != reply) return;
		saveReply.clear();
		const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		reply->deleteLater();
		if (http == 204) refresh();
		else {
			status->setText(text("TwitchAuth.Native.InfoSaveFailed", "Twitch did not save the changes. Check your connection or reconnect Twitch."));
			updateSaveButton();
		}
	});
}

void StreamInfoDock::updateSaveButton()
{
	const bool validCategory = category->text() == selectedCategoryName;
	const bool changed = title->text().trimmed() != loadedTitle || selectedCategoryId != loadedCategoryId;
	saveButton->setEnabled(loaded && !saveReply && !title->text().trimmed().isEmpty() && validCategory && changed);
}

} // namespace twitch
