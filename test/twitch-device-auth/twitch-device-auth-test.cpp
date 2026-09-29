#include "TwitchDeviceFlow.hpp"
#include "TwitchTokenStore.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QUrlQuery>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace twitch;

static void waitMs(int milliseconds)
{
	QEventLoop loop;
	QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
	loop.exec();
}
static bool waitUntil(const std::function<bool()> &predicate, int timeout = 3000)
{
	QElapsedTimer timer;
	timer.start();
	while (!predicate() && timer.elapsed() < timeout) waitMs(10);
	return predicate();
}
#define QVERIFY(value) do { if (!(value)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #value); } while (0)
#define QCOMPARE(actual, expected) QVERIFY((actual) == (expected))
#define QTRY_COMPARE_WITH_TIMEOUT(actual, expected, timeout) QVERIFY(waitUntil([&] { return (actual) == (expected); }, timeout))
#define QTRY_COMPARE(actual, expected) QTRY_COMPARE_WITH_TIMEOUT(actual, expected, 3000)

class QSignalSpy : public QList<QList<QVariant>> {
public:
	QSignalSpy(DeviceFlow *flow, void (DeviceFlow::*signal)())
	{
		connection = QObject::connect(flow, signal, flow, [this] { push_back({}); });
	}
	QSignalSpy(DeviceFlow *flow, void (DeviceFlow::*signal)(const QUrl &, const QString &))
	{
		connection = QObject::connect(flow, signal, flow, [this](const QUrl &url, const QString &code) { push_back({url, code}); });
	}
	QSignalSpy(DeviceFlow *flow, void (DeviceFlow::*signal)(const QString &, bool))
	{
		connection = QObject::connect(flow, signal, flow, [this](const QString &message, bool reauthorize) { push_back({message, reauthorize}); });
	}
	~QSignalSpy() { QObject::disconnect(connection); }
private:
	QMetaObject::Connection connection;
};

static const QString clientId = QStringLiteral("testclient012345678901234567890");
static QJsonObject tokens(QString refresh = QStringLiteral("refresh+/=%second"))
{
	return {{"access_token", "access-second"}, {"refresh_token", refresh}, {"expires_in", 14400},
		{"token_type", "bearer"}, {"scope", QJsonArray{"channel:read:stream_key"}}};
}
static QJsonObject validation(QString id = clientId)
{
	return {{"client_id", id}, {"login", "example_streamer"}, {"user_id", "12345"},
		{"expires_in", 14000}, {"scopes", QJsonArray{"channel:read:stream_key"}}};
}

struct Request {
	QByteArray method, path, body;
	QMap<QByteArray, QByteArray> headers;
};
struct Response {
	int status = 200;
	QJsonObject json;
};

class Server : public QTcpServer {
public:
	QList<Request> requests;
	std::function<Response(const Request &)> respond;
	Server()
	{
		listen(QHostAddress::LocalHost);
		connect(this, &QTcpServer::newConnection, this, [this] {
			while (hasPendingConnections()) {
				auto socket = nextPendingConnection();
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
					buffer += socket->readAll();
					int split = buffer.indexOf("\r\n\r\n");
					if (split < 0) return;
					Request request;
					auto lines = buffer.left(split).split('\n');
					auto first = lines.takeFirst().trimmed().split(' ');
					request.method = first.value(0);
					request.path = first.value(1);
					for (auto line : lines) {
						int colon = line.indexOf(':');
						if (colon > 0) request.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
					}
					int length = request.headers.value("content-length").toInt();
					if (buffer.size() < split + 4 + length) return;
					request.body = buffer.mid(split + 4, length);
					requests.push_back(request);
					auto response = respond ? respond(request) : Response{500, {}};
					auto body = QJsonDocument(response.json).toJson(QJsonDocument::Compact);
					socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
					socket->disconnectFromHost();
				});
			}
		});
	}
	Endpoints endpoints() const
	{
		auto root = QString("http://127.0.0.1:%1/").arg(serverPort());
		return {QUrl(root + "device"), QUrl(root + "token"), QUrl(root + "validate"), QUrl(root + "helix/")};
	}
	Response normal(const Request &r)
	{
		if (r.path == "/device") return {200, {{"device_code", "private-device-code"}, {"user_code", "ABCD-EFGH"}, {"verification_uri", "https://www.twitch.tv/activate?public=true&device-code=ABCD-EFGH"}, {"expires_in", 1800}, {"interval", 1}}};
		if (r.path == "/token") return {200, tokens()};
		if (r.path == "/validate") return {200, validation()};
		if (r.path == "/helix/streams/key?broadcaster_id=12345") return {200, {{"data", QJsonArray{QJsonObject{{"stream_key", "fixture-stream-key"}}}}}};
		return {404, {}};
	}
};

class DeviceAuthTest : public QObject {
public:
	void emptyClientDoesNotContactTwitch()
	{
		Server server;
		DeviceFlow flow(server.endpoints());
		QSignalSpy failure(&flow, &DeviceFlow::failed);
		flow.begin("  ");
		QCOMPARE(failure.count(), 1);
		QCOMPARE(server.requests.size(), 0);
	}
	void loginWaitsForConsentAndRetrievesTheAuthorizedChannelsKey()
	{
		Server server;
		int polls = 0;
		server.respond = [&](const Request &r) {
			if (r.path == "/token" && ++polls == 1) return Response{400, {{"message", "authorization_pending"}}};
			return server.normal(r);
		};
		DeviceFlow flow(server.endpoints());
		QSignalSpy code(&flow, &DeviceFlow::codeReady), ready(&flow, &DeviceFlow::authenticated), changed(&flow, &DeviceFlow::credentialsChanged);
		flow.begin(clientId);
		QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
		QCOMPARE(code.count(), 1);
		QCOMPARE(code[0][1].toString(), "ABCD-EFGH");
		QCOMPARE(flow.credentials().login, "example_streamer");
		QCOMPARE(flow.streamKey(), "fixture-stream-key");
		QVERIFY(changed.count() >= 1);
		QCOMPARE(server.requests[0].method, "POST");
		QUrlQuery device(QString::fromUtf8(server.requests[0].body));
		QCOMPARE(device.queryItemValue("client_id"), clientId);
		QCOMPARE(device.queryItemValue("scopes", QUrl::FullyDecoded), "channel:read:stream_key");
		QUrlQuery poll(QString::fromUtf8(server.requests[1].body));
		QCOMPARE(poll.queryItemValue("grant_type", QUrl::FullyDecoded), "urn:ietf:params:oauth:grant-type:device_code");
		QCOMPARE(poll.queryItemValue("device_code"), "private-device-code");
		QCOMPARE(server.requests.last().headers.value("client-id"), clientId.toUtf8());
		QCOMPARE(server.requests.last().headers.value("authorization"), QByteArray("Bearer access-second"));
		for (const auto &r : server.requests) QVERIFY(!r.body.contains("client_secret"));
	}
	void foreignAuthorizationPageIsRejected()
	{
		Server server;
		server.respond = [&](const Request &r) {
			auto response = server.normal(r);
			response.json["verification_uri"] = "https://twitch.tv.evil.example/activate";
			return response;
		};
		DeviceFlow flow(server.endpoints());
		QSignalSpy code(&flow, &DeviceFlow::codeReady), failure(&flow, &DeviceFlow::failed);
		flow.begin(clientId);
		QTRY_COMPARE(failure.count(), 1);
		QCOMPARE(code.count(), 0);
	}
	void cancelStopsPollingAndNeverConnectsTheAccount()
	{
		Server server;
		server.respond = [&](const Request &r) { return server.normal(r); };
		DeviceFlow flow(server.endpoints());
		QSignalSpy code(&flow, &DeviceFlow::codeReady), ready(&flow, &DeviceFlow::authenticated);
		flow.begin(clientId);
		QTRY_COMPARE(code.count(), 1);
		flow.cancel();
		waitMs(1200);
		QCOMPARE(server.requests.count(), 1);
		QCOMPARE(ready.count(), 0);
	}
	void expiredDeviceCodeDoesNotPollForever()
	{
		Server server;
		server.respond = [&](const Request &r) {
			auto response = server.normal(r);
			if (r.path == "/device") { response.json["expires_in"] = 1; response.json["interval"] = 2; }
			return response;
		};
		DeviceFlow flow(server.endpoints());
		QSignalSpy failure(&flow, &DeviceFlow::failed);
		flow.begin(clientId);
		QTRY_COMPARE_WITH_TIMEOUT(failure.count(), 1, 2500);
		QCOMPARE(server.requests.size(), 1);
	}
	void refreshRotatesAndPersistsBeforeTheNextApiRequest()
	{
		Server server;
		bool savedRotation = false;
		server.respond = [&](const Request &r) {
			if (r.path == "/validate" && r.headers.value("authorization") == "OAuth access-old") return Response{401, {{"message", "invalid access token"}}};
			if (r.path == "/validate" && !savedRotation) return Response{500, {}};
			return server.normal(r);
		};
		DeviceFlow flow(server.endpoints());
		connect(&flow, &DeviceFlow::credentialsChanged, &flow, [&] { savedRotation = flow.credentials().refreshToken == "refresh+/=%second"; });
		QSignalSpy ready(&flow, &DeviceFlow::authenticated);
		flow.restore({clientId, "access-old", "refresh+/=%first", "example_streamer", "12345", 1});
		QTRY_COMPARE(ready.count(), 1);
		QCOMPARE(server.requests.size(), 4);
		QUrlQuery refresh(QString::fromUtf8(server.requests[1].body));
		QCOMPARE(refresh.queryItemValue("refresh_token", QUrl::FullyDecoded), "refresh+/=%first");
		QCOMPARE(refresh.queryItemValue("grant_type"), "refresh_token");
		QVERIFY(savedRotation);
	}
	void revokedRefreshClearsTheSession()
	{
		Server server;
		server.respond = [](const Request &) { return Response{401, {{"message", "Invalid refresh token"}}}; };
		DeviceFlow flow(server.endpoints());
		QSignalSpy failure(&flow, &DeviceFlow::failed);
		flow.restore({clientId, "access-old", "refresh-old", "example_streamer", "12345", 1});
		QTRY_COMPARE(failure.count(), 1);
		QVERIFY(failure[0][1].toBool());
		QVERIFY(flow.credentials().accessToken.isEmpty());
		QVERIFY(flow.credentials().refreshToken.isEmpty());
		QVERIFY(flow.streamKey().isEmpty());
	}
	void expiredTokenDuringKeyRequestRefreshesOnce()
	{
		for (bool stillUnauthorized : {false, true}) {
			Server server;
			int keyRequests = 0, refreshRequests = 0;
			server.respond = [&](const Request &r) {
				if (r.path == "/token") ++refreshRequests;
				if (r.path.startsWith("/helix/") && (++keyRequests == 1 || stillUnauthorized))
					return Response{401, {{"message", "Invalid OAuth token"}}};
				return server.normal(r);
			};
			DeviceFlow flow(server.endpoints());
			QSignalSpy ready(&flow, &DeviceFlow::authenticated);
			QSignalSpy failure(&flow, &DeviceFlow::failed);
			flow.restore({clientId, "access-old", "refresh-old", "example_streamer", "12345", 1});
			QVERIFY(waitUntil([&] { return !ready.empty() || !failure.empty(); }));
			QCOMPARE(refreshRequests, 1);
			QCOMPARE(keyRequests, 2);
			QCOMPARE(ready.count(), stillUnauthorized ? 0 : 1);
			QCOMPARE(failure.count(), stillUnauthorized ? 1 : 0);
			if (stillUnauthorized) QVERIFY(flow.credentials().refreshToken.isEmpty());
		}
	}
	void networkFailurePreservesRefreshCredentials()
	{
		Server server;
		server.respond = [](const Request &) { return Response{503, {{"message", "unavailable"}}}; };
		DeviceFlow flow(server.endpoints());
		QSignalSpy failure(&flow, &DeviceFlow::failed);
		flow.restore({clientId, "access-old", "refresh-old", "example_streamer", "12345", 1});
		QTRY_COMPARE(failure.count(), 1);
		QVERIFY(!failure[0][1].toBool());
		QCOMPARE(flow.credentials().refreshToken, "refresh-old");
	}
	void tokenForDifferentAppOrMissingScopeNeverFetchesStreamKey()
	{
		for (bool wrongApp : {true, false}) {
			Server server;
			server.respond = [&](const Request &) {
				auto result = validation(wrongApp ? "different-client" : clientId);
				if (!wrongApp) result["scopes"] = QJsonArray{};
				return Response{200, result};
			};
			DeviceFlow flow(server.endpoints());
			QSignalSpy failure(&flow, &DeviceFlow::failed);
			flow.restore({clientId, "access-old", "refresh-old", "example_streamer", "12345", 1});
			QTRY_COMPARE(failure.count(), 1);
			QCOMPARE(server.requests.size(), 1);
			QVERIFY(flow.streamKey().isEmpty());
		}
	}
	void encryptedStorageRoundTripsAndRejectsCorruption()
	{
		Credentials original{clientId, "secret-access-token", "secret-refresh-token", "example_streamer", "12345", 14400};
		auto blob = protectCredentials(original);
		QVERIFY(!blob.isEmpty());
		QVERIFY(!blob.contains("secret-access-token"));
		QVERIFY(!blob.contains("secret-refresh-token"));
		auto restored = unprotectCredentials(blob);
		QVERIFY(restored.has_value());
		QCOMPARE(restored->refreshToken, original.refreshToken);
		QCOMPARE(restored->clientId, original.clientId);
		blob[blob.size() / 2] ^= 0x40;
		QVERIFY(!unprotectCredentials(blob).has_value());
		QVERIFY(!unprotectCredentials("not-an-encrypted-token").has_value());
	}
	void copiedProfileReferencesReadTheLatestRotatedSession()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		SessionStore profileA(directory.path()), profileB(directory.path());
		const auto reference = SessionStore::newId();
		Credentials original{clientId, "secret-access-token", "secret-refresh-token", "example_streamer", "12345", 14400};
		QVERIFY(profileA.write(reference, original));
		auto copied = profileB.read(reference);
		QVERIFY(copied.has_value());
		copied->refreshToken = "rotated-refresh-token";
		QVERIFY(profileB.write(reference, *copied));
		auto restored = profileA.read(reference);
		QVERIFY(restored.has_value());
		QCOMPARE(restored->refreshToken, "rotated-refresh-token");
		QFile file(directory.filePath(reference + ".dpapi"));
		QVERIFY(file.open(QIODevice::ReadOnly));
		QVERIFY(!file.readAll().contains("rotated-refresh-token"));
		file.close();
		QVERIFY(!profileA.write("../outside", original));
		QVERIFY(!profileA.read("../outside"));
		QVERIFY(!profileA.remove("../outside"));
		QVERIFY(profileB.remove(reference));
		QVERIFY(!profileA.read(reference));
		QVERIFY(!SessionStore(QString{}).write(reference, original));
	}
};

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	DeviceAuthTest tests;
	int failures = 0;
	auto run = [&](const char *name, auto method) {
		try { (tests.*method)(); std::cout << "PASS " << name << '\n'; }
		catch (const std::exception &error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
	};
#define RUN(test) run(#test, &DeviceAuthTest::test)
	RUN(emptyClientDoesNotContactTwitch);
	RUN(loginWaitsForConsentAndRetrievesTheAuthorizedChannelsKey);
	RUN(foreignAuthorizationPageIsRejected);
	RUN(cancelStopsPollingAndNeverConnectsTheAccount);
	RUN(expiredDeviceCodeDoesNotPollForever);
	RUN(refreshRotatesAndPersistsBeforeTheNextApiRequest);
	RUN(revokedRefreshClearsTheSession);
	RUN(expiredTokenDuringKeyRequestRefreshesOnce);
	RUN(networkFailurePreservesRefreshCredentials);
	RUN(tokenForDifferentAppOrMissingScopeNeverFetchesStreamKey);
	RUN(encryptedStorageRoundTripsAndRejectsCorruption);
	RUN(copiedProfileReferencesReadTheLatestRotatedSession);
	return failures ? 1 : 0;
}
