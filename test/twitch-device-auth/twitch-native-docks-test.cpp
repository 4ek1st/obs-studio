#include "TwitchNativeDocks.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <iostream>
#include <stdexcept>

using namespace twitch;

#define CHECK(value) do { if (!(value)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #value); } while (0)

static bool waitUntil(const std::function<bool()> &predicate, int timeout = 3000)
{
	QElapsedTimer clock;
	clock.start();
	while (!predicate() && clock.elapsed() < timeout) {
		QEventLoop loop;
		QTimer::singleShot(10, &loop, &QEventLoop::quit);
		loop.exec();
	}
	return predicate();
}

struct Request {
	QByteArray method, path, body;
	QMap<QByteArray, QByteArray> headers;
};

class Fixture : public QTcpServer {
public:
	QList<Request> requests;
	Fixture()
	{
		CHECK(listen(QHostAddress::LocalHost));
		connect(this, &QTcpServer::newConnection, this, [this] {
			while (hasPendingConnections()) {
				auto socket = nextPendingConnection();
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
					buffer += socket->readAll();
					const int split = buffer.indexOf("\r\n\r\n");
					if (split < 0) return;
					Request request;
					auto lines = buffer.left(split).split('\n');
					auto first = lines.takeFirst().trimmed().split(' ');
					request.method = first.value(0);
					request.path = first.value(1);
					for (auto line : lines) {
						const int colon = line.indexOf(':');
						if (colon > 0) request.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
					}
					const int length = request.headers.value("content-length").toInt();
					if (buffer.size() < split + 4 + length) return;
					request.body = buffer.mid(split + 4, length);
					requests.push_back(request);
					QJsonObject json;
					int code = 200;
					if (request.path.startsWith("/channels?") && request.method == "GET")
						json = {{"data", QJsonArray{QJsonObject{{"title", "Old title"}, {"game_id", "123"}, {"game_name", "Old Game"}}}}};
					else if (request.path.startsWith("/search/categories?") && request.method == "GET")
						json = {{"data", QJsonArray{QJsonObject{{"id", "456"}, {"name", "Fortnite"}}}}};
					else if (request.path.startsWith("/channels?") && request.method == "PATCH") code = 204;
					else code = 404;
					const auto body = code == 204 ? QByteArray{} : QJsonDocument(json).toJson(QJsonDocument::Compact);
					socket->write("HTTP/1.1 " + QByteArray::number(code) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
					socket->disconnectFromHost();
				});
			}
		});
	}
	QUrl api() const { return QUrl(QString("http://127.0.0.1:%1/").arg(serverPort())); }
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	try {
		const auto message = parseChatLine("@display-name=Viewer :viewer!viewer@viewer.tmi.twitch.tv PRIVMSG #example_streamer :Hello chat");
		CHECK(message.sender == "Viewer" && message.text == "Hello chat");
		CHECK(parseChatLine("PING :tmi.twitch.tv").pong == "PONG :tmi.twitch.tv\r\n");
		const auto quotedCommand = parseChatLine(":viewer!viewer@viewer.tmi.twitch.tv PRIVMSG #example_streamer :I typed RECONNECT and 001 in chat");
		CHECK(!quotedCommand.reconnect && !quotedCommand.welcomed);
		CHECK(!parseChatLine(":viewer!viewer@viewer.tmi.twitch.tv PRIVMSG #example_streamer :RECONNECT").reconnect);
		CHECK(parseChatLine(":tmi.twitch.tv RECONNECT").reconnect);
		CHECK(parseChatLine(":tmi.twitch.tv 001 example_streamer :Welcome").welcomed);
		CHECK(parseChatLine(":tmi.twitch.tv 366 example_streamer #example_streamer :End of /NAMES list").joinedChannel == "#example_streamer");
		CHECK(parseChatLine("@room-id=123 :tmi.twitch.tv ROOMSTATE #example_streamer").joinedChannel == "#example_streamer");
		CHECK(parseChatLine(":viewer!viewer@viewer.tmi.twitch.tv PRIVMSG #example_streamer :ROOMSTATE #example_streamer").joinedChannel.isEmpty());
		CHECK(encodeChatMessage("example_streamer", "Hello") == "PRIVMSG #example_streamer :Hello\r\n");
		CHECK(encodeChatMessage("example_streamer", "line\nINJECT").isEmpty());
		ChatDock chat;
		auto composer = chat.findChild<QLineEdit *>("twitchChatComposer");
		CHECK(composer && !composer->isEnabled());
		std::cout << "PASS IRC parsing and safe outgoing message\n";

		Fixture fixture;
		StreamInfoDock dock(fixture.api());
		dock.setCredentials({"testclient", "test-access", "test-refresh", "example_streamer", "12345", 0});
		auto title = dock.findChild<QLineEdit *>("twitchTitle");
		auto category = dock.findChild<QLineEdit *>("twitchCategory");
		auto results = dock.findChild<QListWidget *>("twitchCategoryResults");
		auto save = dock.findChild<QPushButton *>("twitchSave");
		CHECK(title && category && results && save);
		CHECK(waitUntil([&] { return title->text() == "Old title" && category->text() == "Old Game"; }));
		CHECK(fixture.requests.first().headers.value("authorization") == "Bearer test-access");
		CHECK(fixture.requests.first().headers.value("client-id") == "testclient");
		category->setText("Old G");
		CHECK(!save->isEnabled());
		category->setText("Old Game");
		title->setText("New title");
		CHECK(save->isEnabled());
		save->click();
		CHECK(waitUntil([&] {
			for (const auto &r : fixture.requests) if (r.method == "PATCH") return true;
			return false;
		}));
		Request titlePatch;
		for (const auto &r : fixture.requests) if (r.method == "PATCH") titlePatch = r;
		const auto titleJson = QJsonDocument::fromJson(titlePatch.body).object();
		CHECK(titleJson["title"].toString() == "New title" && !titleJson.contains("game_id"));
		CHECK(QUrlQuery(QString::fromUtf8(titlePatch.path.mid(titlePatch.path.indexOf('?') + 1))).queryItemValue("broadcaster_id") == "12345");

		category->setText("Fort");
		CHECK(waitUntil([&] { return results->count() == 1; }));
		CHECK(!save->isEnabled());
		emit results->itemClicked(results->item(0));
		CHECK(category->text() == "Fortnite" && save->isEnabled());
		save->click();
		CHECK(waitUntil([&] {
			int patches = 0;
			for (const auto &r : fixture.requests) if (r.method == "PATCH") ++patches;
			return patches == 2;
		}));
		Request categoryPatch;
		for (const auto &r : fixture.requests) if (r.method == "PATCH") categoryPatch = r;
		const auto categoryJson = QJsonDocument::fromJson(categoryPatch.body).object();
		CHECK(categoryJson["game_id"].toString() == "456" && !categoryJson.contains("title"));
		dock.setCredentials({});
		CHECK(title->text().isEmpty() && category->text().isEmpty() && !save->isEnabled());
		std::cout << "PASS channel read, category search and precise updates\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
