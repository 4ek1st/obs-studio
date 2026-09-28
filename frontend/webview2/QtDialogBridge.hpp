#pragma once

#include <QJsonObject>
#include <QObject>
#include <memory>

class QWidget;

namespace OBSWeb {
struct ExternalDropData;
// Adapts live Qt widgets in a dialog or floating dock content. Native ownership is retained.
class QtDialogBridge : public QObject {
public:
	explicit QtDialogBridge(QWidget *dialog, QObject *parent = nullptr);
	~QtDialogBridge() override;
	QJsonObject snapshot();
	bool execute(const QString &command, const QJsonObject &args, QString &error);
	// Native-only: file paths in this data have already been obtained from WebView file objects.
	bool drop(const ExternalDropData &data, QString &error);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace OBSWeb

void InstallWebView2Dialogs(QObject *owner, const QString &assets, const QString &profile);
