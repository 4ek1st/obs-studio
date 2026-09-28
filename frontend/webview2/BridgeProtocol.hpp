#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <optional>

namespace OBSWeb {
struct Request {
	QString id;
	QString command;
	QJsonObject args;
};

bool IsLocalUi(const QUrl &url);
std::optional<Request> ParseRequest(const QString &source, const QByteArray &json, QString &error);
} // namespace OBSWeb
