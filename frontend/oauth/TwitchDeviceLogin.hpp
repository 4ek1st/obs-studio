#pragma once

#include <QDialog>
#include <QUrl>
#include <functional>

namespace twitch {
class DeviceFlow;

class DeviceLogin : public QDialog {
	Q_OBJECT
public:
	using Translate = std::function<QString(const char *)>;
	DeviceLogin(DeviceFlow &flow, const QString &clientId, QWidget *parent = nullptr, Translate translate = {});
signals:
	void openBrowser(const QUrl &url);
};
} // namespace twitch
