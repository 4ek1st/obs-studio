#include "TwitchDeviceFlow.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>

namespace twitch {

static const QString scope = QStringLiteral("channel:read:stream_key");

static QByteArray form(std::initializer_list<QPair<QString, QString>> fields)
{
	QByteArray result;
	for (const auto &field : fields) {
		if (!result.isEmpty()) result += '&';
		result += QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
	}
	return result;
}

DeviceFlow::DeviceFlow(QObject *parent) : DeviceFlow(Endpoints{}, parent) {}

DeviceFlow::DeviceFlow(const Endpoints &urls, QObject *parent) : QObject(parent), endpoints(urls)
{
	pollTimer.setSingleShot(true);
	expiryTimer.setSingleShot(true);
	validationTimer.setSingleShot(true);
	connect(&pollTimer, &QTimer::timeout, this, &DeviceFlow::poll);
	connect(&expiryTimer, &QTimer::timeout, this, [this] {
		fail(tr("The Twitch confirmation code has expired. Please connect again."));
	});
	connect(&validationTimer, &QTimer::timeout, this, &DeviceFlow::validate);
}

DeviceFlow::~DeviceFlow()
{
	cancel();
}

void DeviceFlow::cancel()
{
	pollTimer.stop();
	expiryTimer.stop();
	validationTimer.stop();
	deviceCode.clear();
	if (reply) {
		disconnect(reply, nullptr, this, nullptr);
		reply->abort();
		reply->deleteLater();
		reply.clear();
	}
}

void DeviceFlow::clearSession()
{
	saved.accessToken.clear();
	saved.refreshToken.clear();
	saved.login.clear();
	saved.userId.clear();
	saved.expiresAt = 0;
	key.clear();
	emit credentialsChanged();
}

void DeviceFlow::fail(const QString &message, bool reauthorize)
{
	cancel();
	if (reauthorize) clearSession();
	else if (!saved.refreshToken.isEmpty()) validationTimer.start(5 * 60 * 1000);
	emit failed(message, reauthorize);
}

void DeviceFlow::request(const QUrl &url, const QByteArray &body, bool post, bool api, Response response)
{
	QNetworkRequest request(url);
	request.setTransferTimeout(15000);
	// Never forward credentials to a redirect target.
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	if (post) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	if (api) {
		request.setRawHeader("Client-ID", saved.clientId.toUtf8());
		request.setRawHeader("Authorization", "Bearer " + saved.accessToken.toUtf8());
	} else if (url == endpoints.validate) {
		request.setRawHeader("Authorization", "OAuth " + saved.accessToken.toUtf8());
	}
	auto pending = post ? network.post(request, body) : network.get(request);
	reply = pending;
	connect(pending, &QNetworkReply::finished, this, [this, pending, response] {
		reply.clear();
		int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QJsonParseError parse;
		auto json = QJsonDocument::fromJson(pending->readAll(), &parse);
		if (parse.error != QJsonParseError::NoError || !json.isObject()) {
			if (status >= 200 && status < 300) status = 0;
		}
		pending->deleteLater();
		response(status, json.object());
	});
}

void DeviceFlow::begin(const QString &clientId)
{
	cancel();
	saved = {};
	key.clear();
	retriedKey = false;
	saved.clientId = clientId.trimmed();
	static const QRegularExpression validId("^[a-zA-Z0-9]{1,128}$");
	if (!validId.match(saved.clientId).hasMatch()) {
		fail(tr("Enter the Client ID of your public Twitch application."));
		return;
	}
	refreshing = false;
	request(endpoints.device, form({{"client_id", saved.clientId}, {"scopes", scope}}), true, false,
		[this](int status, const QJsonObject &json) {
			if (status != 200) {
				fail(status == 400 || status == 401 ? tr("Twitch rejected the application. Check its Client ID and make sure its client type is Public.") : tr("Could not contact Twitch. Please try again."));
				return;
			}
			const QUrl url(json["verification_uri"].toString());
			const QString userCode = json["user_code"].toString();
			const int seconds = json["expires_in"].toInt();
			const int interval = json["interval"].toInt(5);
			deviceCode = json["device_code"].toString();
			if (deviceCode.isEmpty() || userCode.isEmpty() || seconds <= 0 || seconds > 3600 ||
			    interval <= 0 || interval > 300 || url.scheme() != "https" || !url.userInfo().isEmpty() ||
			    (url.port(-1) != -1 && url.port() != 443) ||
			    (url.host() != "www.twitch.tv" && url.host() != "twitch.tv") || url.path() != "/activate") {
				fail(tr("Twitch returned an invalid authorization response. Please try again."));
				return;
			}
			pollInterval = interval * 1000;
			expiryTimer.start(seconds * 1000);
			pollTimer.start(pollInterval);
			emit codeReady(url, userCode);
		});
}

void DeviceFlow::poll()
{
	if (deviceCode.isEmpty()) return;
	request(endpoints.token, form({{"client_id", saved.clientId}, {"scopes", scope}, {"device_code", deviceCode},
		{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}), true, false,
		[this](int status, const QJsonObject &json) {
			if (status == 200) {
				expiryTimer.stop();
				deviceCode.clear();
				acceptTokens(json);
				return;
			}
			const QString error = json["error"].toString().toLower();
			const QString message = json["message"].toString().toLower();
			if (error == "authorization_pending" || message == "authorization_pending") {
				pollTimer.start(pollInterval);
			} else if (error == "slow_down" || message == "slow_down" || status == 429) {
				pollInterval = std::min(pollInterval + 5000, 60000);
				pollTimer.start(pollInterval);
			} else if (status == 0 || status >= 500) {
				pollInterval = std::min(pollInterval * 2, 60000);
				pollTimer.start(pollInterval);
			} else {
				fail(tr("Twitch authorization was declined or the code expired. Please connect again."));
			}
		});
}

void DeviceFlow::acceptTokens(const QJsonObject &json)
{
	const auto access = json["access_token"].toString();
	const auto refresh = json["refresh_token"].toString();
	const int seconds = json["expires_in"].toInt();
	if (access.isEmpty() || refresh.isEmpty() || seconds <= 0 || json["token_type"].toString().compare("bearer", Qt::CaseInsensitive)) {
		fail(tr("Twitch returned incomplete account credentials. Please connect again."), true);
		return;
	}
	saved.accessToken = access;
	saved.refreshToken = refresh;
	saved.expiresAt = QDateTime::currentSecsSinceEpoch() + seconds;
	// Public-client refresh tokens are single-use. Persist the replacement before any further request.
	emit credentialsChanged();
	validate();
}

void DeviceFlow::restore(const Credentials &credentials)
{
	cancel();
	saved = credentials;
	key.clear();
	refreshing = false;
	retriedKey = false;
	validate();
}

void DeviceFlow::validate()
{
	if (reply) return;
	validationTimer.stop();
	if (saved.accessToken.isEmpty()) {
		if (!saved.refreshToken.isEmpty()) refresh();
		else fail(tr("Connect your Twitch account again."), true);
		return;
	}
	request(endpoints.validate, {}, false, false, [this](int status, const QJsonObject &json) {
		if (status == 401) {
			if (!refreshing && !saved.refreshToken.isEmpty()) refresh();
			else fail(tr("Twitch access has been revoked. Connect your account again."), true);
			return;
		}
		if (status != 200) {
			fail(tr("Could not verify your Twitch session. Your saved login has been kept."));
			return;
		}
		const auto scopes = json["scopes"].toArray();
		const auto id = json["user_id"].toString();
		const auto login = json["login"].toString();
		if (json["client_id"].toString() != saved.clientId || id.isEmpty() || login.isEmpty() ||
		    (!saved.userId.isEmpty() && saved.userId != id) || !scopes.contains(scope)) {
			fail(tr("The Twitch session does not belong to this application or lacks streaming permission. Connect again."), true);
			return;
		}
		saved.userId = id;
		saved.login = login;
		saved.expiresAt = QDateTime::currentSecsSinceEpoch() + json["expires_in"].toInt();
		refreshing = false;
		emit credentialsChanged();
		fetchKey();
	});
}

void DeviceFlow::refresh()
{
	refreshing = true;
	request(endpoints.token, form({{"client_id", saved.clientId}, {"grant_type", "refresh_token"},
		{"refresh_token", saved.refreshToken}}), true, false, [this](int status, const QJsonObject &json) {
		if (status == 200) acceptTokens(json);
		else if (status == 400 || status == 401 || status == 403) fail(tr("Twitch access has expired or been revoked. Connect your account again."), true);
		else { refreshing = false; fail(tr("Could not renew your Twitch session. Please try again.")); }
	});
}

void DeviceFlow::fetchKey()
{
	QUrl url = endpoints.api.resolved(QUrl("streams/key"));
	QUrlQuery query;
	query.addQueryItem("broadcaster_id", saved.userId);
	url.setQuery(query);
	request(url, {}, false, true, [this](int status, const QJsonObject &json) {
		if (status == 401 && !retriedKey && !saved.refreshToken.isEmpty()) {
			retriedKey = true;
			refresh();
			return;
		}
		if (status != 200) {
			fail(status == 403 ? tr("Twitch could not provide the stream key. Enable two-factor authentication on your Twitch account.") : tr("Could not retrieve the Twitch stream key. Please try again."), status == 401);
			return;
		}
		const auto data = json["data"].toArray();
		key = data.isEmpty() ? QString{} : data.first().toObject()["stream_key"].toString();
		if (key.isEmpty()) {
			fail(tr("Twitch returned an empty stream key. Please try again."));
			return;
		}
		retriedKey = false;
		validationTimer.start(55 * 60 * 1000);
		emit authenticated();
	});
}

} // namespace twitch
