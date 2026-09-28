#pragma once
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QUrl>
#include <optional>

class QWidget;
namespace OBSWeb {
struct ExternalDropData {
	QList<QUrl> urls;
	QString text;
};
bool IsExternalDropOrigin(const QString &source, const QString &document = QStringLiteral("index.html"));
// trustedFilePaths must originate from ICoreWebView2File, never JSON metadata.
std::optional<ExternalDropData> ParseExternalDrop(const QJsonObject &args,
						const QStringList &trustedFilePaths, QString &error);
bool DispatchExternalDrop(QWidget *target, const ExternalDropData &drop);
}
Q_DECLARE_METATYPE(OBSWeb::ExternalDropData)
