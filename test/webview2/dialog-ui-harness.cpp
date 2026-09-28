// A local-only integration fixture: the real Qt bridge is exercised by browser UI input.
#include "QtDialogBridge.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	if (argc != 2) return 2;
	const QDir assets(QString::fromLocal8Bit(argv[1]));
	QDialog dialog;
	dialog.setWindowTitle("OBS dialog bridge interaction fixture");
	dialog.resize(640, 440);
	auto *layout = new QFormLayout(&dialog);
	auto *name = new QLineEdit("Scene fixture", &dialog); name->setObjectName("sceneName"); layout->addRow("Name", name);
	auto *combo = new QComboBox(&dialog); combo->setObjectName("sourceKind"); combo->addItems({"Image", "Color", "Video"}); layout->addRow("Source", combo);
	auto *spin = new QSpinBox(&dialog); spin->setObjectName("width"); spin->setRange(1, 4096); spin->setValue(640); layout->addRow("Width", spin);
	auto *check = new QCheckBox("Use custom settings", &dialog); check->setObjectName("custom"); layout->addRow(check);
	auto *list = new QListWidget(&dialog); list->setObjectName("sources"); list->addItems({"First source", "Second source", "Third source"});
	for (int i = 0; i < list->count(); ++i) list->item(i)->setFlags(list->item(i)->flags() | Qt::ItemIsEditable);
	layout->addRow(list);
	auto *feedback = new QLabel("Waiting for edits", &dialog); feedback->setObjectName("feedback"); layout->addRow(feedback);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addRow(buttons);
	QObject::connect(name, &QLineEdit::editingFinished, [&] { feedback->setText("Applied: " + name->text()); });
	QObject::connect(combo, &QComboBox::activated, [&](int index) { feedback->setText("Activated choice " + QString::number(index)); });
	QObject::connect(check, &QCheckBox::clicked, [&](bool value) { feedback->setText(value ? "Custom enabled" : "Custom disabled"); });
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.show();
	OBSWeb::QtDialogBridge bridge(&dialog);
	QTcpServer server;
	QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
		auto *socket = server.nextPendingConnection();
		auto buffer = std::make_shared<QByteArray>();
		QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
			buffer->append(socket->readAll());
			const int headerEnd = buffer->indexOf("\r\n\r\n");
			if (headerEnd < 0) return;
			int length = 0;
			for (const auto &line : buffer->left(headerEnd).split('\n'))
				if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
			if (buffer->size() < headerEnd + 4 + length) return;
			const auto route = buffer->left(buffer->indexOf('\n')).split(' ').value(1);
			QByteArray body, mime = "application/json";
			if (route == "/command") {
				const auto request = QJsonDocument::fromJson(buffer->mid(headerEnd + 4, length)).object();
				QString error;
				const bool ok = request.value("command") == "dialog.state" || bridge.execute(request.value("command").toString(), request.value("args").toObject(), error);
				body = QJsonDocument(QJsonObject{{"version", 1}, {"id", request.value("id")}, {"ok", ok},
					{"result", QJsonObject{}}, {"error", QJsonObject{{"message", error}}}}).toJson(QJsonDocument::Compact);
			} else if (route == "/state") {
				body = QJsonDocument(bridge.snapshot()).toJson(QJsonDocument::Compact);
			} else if (route == "/quit") {
				body = "{}"; QTimer::singleShot(0, &app, &QApplication::quit);
			} else {
				const auto filename = route == "/" ? QStringLiteral("dialog.html") : QString::fromUtf8(route.mid(1));
				if (filename.contains('/') || filename.contains('\\') || filename.contains("..")) { socket->disconnectFromHost(); return; }
				QFile file(assets.filePath(filename));
				if (!file.open(QIODevice::ReadOnly)) { socket->disconnectFromHost(); return; }
				body = file.readAll();
				mime = filename.endsWith(".css") ? "text/css" : filename.endsWith(".html") ? "text/html" : "text/javascript";
				if (filename == "dialog.html") {
					body.replace("script-src 'self'", "script-src 'self' 'unsafe-inline'");
					body.replace("connect-src 'none'", "connect-src 'self'");
					body.replace("<script type=\"module\"", R"(<script>
const listeners = new Set();
const deliver = data => listeners.forEach(listener => listener({data}));
window.chrome = {webview: {
addEventListener(type, fn) { if (type === 'message') listeners.add(fn); },
removeEventListener(type, fn) { listeners.delete(fn); },
async postMessage(message) {
 const response = await fetch('/command', {method:'POST',body:JSON.stringify(message)});
 deliver(await response.json());
 deliver({version:1,event:'dialog.state',data:await (await fetch('/state')).json()});
}
}};
setInterval(async () => deliver({version:1,event:'dialog.state',data:await (await fetch('/state')).json()}), 120);
</script><script type="module")");
				}
			}
			socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: " + mime +
				"\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
			socket->disconnectFromHost();
		});
		QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
	});
	if (!server.listen(QHostAddress::LocalHost, 47983)) return 3;
	std::cout << "http://127.0.0.1:47983/\n" << std::flush;
	QTimer::singleShot(900000, &app, &QApplication::quit);
	return app.exec();
}
