#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <optional>

namespace OBSWeb {
// .local causes a DNS/mDNS resolution delay in WebView2 virtual-host navigation.
// .example is reserved for this local-only mapping and cannot be a public site.
inline constexpr auto LocalUiHost = "obs-ui.example";
struct Request {
	QString id;
	QString command;
	QJsonObject args;
};

bool IsLocalUi(const QUrl &url);
std::optional<Request> ParseRequest(const QString &source, const QByteArray &json, QString &error);
} // namespace OBSWeb
