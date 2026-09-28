#include "BridgeProtocol.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	int failures = 0;
	int checks = 0;
	auto check = [&](bool pass, const char *label) {
		++checks;
		if (!pass) {
			std::cerr << "FAIL: " << label << '\n';
			++failures;
		}
	};
	const QString local = QStringLiteral("https://obs-ui.local/index.html");
	const QByteArray valid = R"({"version":1,"id":"17","command":"state.get","args":{}})";
	QString error;
	const auto accepted = OBSWeb::ParseRequest(local, valid, error);
	check(accepted.has_value(), "a local version 1 request is accepted");
	if (accepted) {
		check(accepted->id == "17", "request ID survives parsing");
		check(accepted->command == "state.get", "command survives parsing");
	}
	check(OBSWeb::IsLocalUi(QUrl(local)), "local document origin is accepted");
	for (const auto *url : {"https://example.com/", "https://obs-ui.local.evil/index.html",
				"http://obs-ui.local/", "file:///index.html", "https://user@obs-ui.local/",
				"https://obs-ui.local:8443/", "data:text/html,hello"}) {
		check(!OBSWeb::ParseRequest(QString::fromLatin1(url), valid, error), "untrusted origin is rejected");
	}
	for (const auto *body : {"{", "[]", "{}",
				 R"({"version":2,"id":"1","command":"state.get","args":{}})",
				 R"({"version":1,"id":1,"command":"state.get","args":{}})",
				 R"({"version":1,"id":"","command":"state.get","args":{}})",
				 R"({"version":1,"id":"1","command":"","args":{}})",
				 R"({"version":1,"id":"1","command":"state.get","args":[]})",
				 R"({"version":1,"id":"1","command":"state.get"})"}) {
		check(!OBSWeb::ParseRequest(local, body, error), "malformed envelope is rejected");
	}
	QJsonObject object = QJsonDocument::fromJson(valid).object();
	object["id"] = QString(65, 'a');
	check(!OBSWeb::ParseRequest(local, QJsonDocument(object).toJson(), error), "oversized ID is rejected");
	object["id"] = "1";
	object["args"] = QJsonObject{{"text", QString(70000, 'a')}};
	check(!OBSWeb::ParseRequest(local, QJsonDocument(object).toJson(), error), "oversized message is rejected");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
