#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>

namespace twitch {

struct Credentials {
	QString clientId;
	QString accessToken;
	QString refreshToken;
	QString login;
	QString userId;
	qint64 expiresAt = 0;
};

struct Endpoints {
	QUrl device = QUrl("https://id.twitch.tv/oauth2/device");
	QUrl token = QUrl("https://id.twitch.tv/oauth2/token");
	QUrl validate = QUrl("https://id.twitch.tv/oauth2/validate");
	QUrl api = QUrl("https://api.twitch.tv/helix/");
};

class DeviceFlow : public QObject {
	Q_OBJECT
public:
	explicit DeviceFlow(QObject *parent = nullptr);
	explicit DeviceFlow(const Endpoints &endpoints, QObject *parent = nullptr);
	~DeviceFlow() override;
	void begin(const QString &clientId);
	void restore(const Credentials &credentials);
	void validate();
	void cancel();
	const Credentials &credentials() const { return saved; }
	const QString &streamKey() const { return key; }

signals:
	void codeReady(const QUrl &url, const QString &userCode);
	void credentialsChanged();
	void authenticated();
	void failed(const QString &message, bool reauthorize);
	void permissionsRequired(const QString &message);

private:
	using Response = std::function<void(int, const QJsonObject &)>;
	void request(const QUrl &url, const QByteArray &body, bool post, bool api, Response response);
	void poll();
	void acceptTokens(const QJsonObject &json);
	void refresh();
	void fetchKey();
	void fail(const QString &message, bool reauthorize = false);
	void clearSession();
	Endpoints endpoints;
	QNetworkAccessManager network;
	QPointer<QNetworkReply> reply;
	QTimer pollTimer, expiryTimer, validationTimer;
	Credentials saved;
	QString deviceCode, key;
	int pollInterval = 5000;
	bool refreshing = false;
	bool retriedKey = false;
};

} // namespace twitch
