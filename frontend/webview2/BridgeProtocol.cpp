#include "BridgeProtocol.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

namespace OBSWeb {
bool IsLocalUi(const QUrl &url)
{
	return url.isValid() && url.scheme() == QStringLiteral("https") &&
	       url.host() == QLatin1String(LocalUiHost) && url.port(443) == 443 &&
	       url.userInfo().isEmpty();
}

std::optional<Request> ParseRequest(const QString &source, const QByteArray &json, QString &error)
{
	error.clear();
	auto reject = [&](const char *reason) -> std::optional<Request> {
		error = QString::fromLatin1(reason);
		return std::nullopt;
	};
	if (!IsLocalUi(QUrl(source)))
		return reject("InvalidOrigin");
	if (json.size() > 65536)
		return reject("MessageTooLarge");
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(json, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return reject("InvalidJson");
	const auto object = document.object();
	if (!object.value(QStringLiteral("version")).isDouble() ||
	    object.value(QStringLiteral("version")).toDouble() != 1.0)
		return reject("InvalidVersion");
	const auto id = object.value(QStringLiteral("id"));
	if (!id.isString() || id.toString().isEmpty() || id.toString().size() > 64)
		return reject("InvalidId");
	const auto command = object.value(QStringLiteral("command"));
	static const QRegularExpression commandPattern(QStringLiteral("^[a-z][a-zA-Z0-9.]{0,63}$"));
	if (!command.isString() || !commandPattern.match(command.toString()).hasMatch())
		return reject("InvalidCommand");
	const auto args = object.value(QStringLiteral("args"));
	if (!args.isObject())
		return reject("InvalidArgs");
	return Request{id.toString(), command.toString(), args.toObject()};
}
} // namespace OBSWeb
