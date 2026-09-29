#pragma once

#include "TwitchDeviceFlow.hpp"

#include <QDockWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QNetworkReply;

namespace twitch {

struct ChatLine {
	QString sender;
	QString text;
	QString notice;
	QByteArray pong;
	QByteArray joinedChannel;
	bool welcomed = false;
	bool reconnect = false;
};

ChatLine parseChatLine(const QByteArray &line);
QByteArray encodeChatMessage(const QString &channel, const QString &message);

class ChatDock : public QDockWidget {
public:
	using Translate = std::function<QString(const char *)>;
	explicit ChatDock(QWidget *parent = nullptr, Translate translate = {});
	void setCredentials(const Credentials &credentials);

private:
	QString text(const char *key, const char *fallback) const;
	void connectChat();
	void readChat();
	void sendChat();
	void appendMessage(const QString &message);
	Credentials credentials;
	Translate translator;
	QSslSocket socket;
	QTimer reconnectTimer;
	QByteArray input;
	QPlainTextEdit *messages = nullptr;
	QLineEdit *composer = nullptr;
	QLabel *status = nullptr;
	bool joined = false;
	bool stopRetry = false;
};

class StreamInfoDock : public QDockWidget {
public:
	using Translate = std::function<QString(const char *)>;
	explicit StreamInfoDock(const QUrl &apiBase = QUrl("https://api.twitch.tv/helix/"),
				QWidget *parent = nullptr, Translate translate = {});
	void setCredentials(const Credentials &credentials);
	void refresh();

private:
	QString text(const char *key, const char *fallback) const;
	QNetworkRequest request(const QUrl &url) const;
	void searchCategories();
	void save();
	void updateSaveButton();
	Credentials credentials;
	Translate translator;
	QUrl apiBase;
	QNetworkAccessManager network;
	QPointer<QNetworkReply> channelReply, searchReply, saveReply;
	QTimer searchTimer;
	QLineEdit *title = nullptr;
	QLineEdit *category = nullptr;
	QListWidget *results = nullptr;
	QPushButton *saveButton = nullptr;
	QLabel *status = nullptr;
	QString loadedTitle, loadedCategoryId, selectedCategoryId, selectedCategoryName;
	bool loaded = false;
};

} // namespace twitch
