#pragma once

#include "OAuth.hpp"

#include <json11.hpp>

#include <QTimer>
#ifdef TWITCH_DEVICE_AUTH
#include "TwitchDeviceFlow.hpp"
#endif

class TwitchAuth : public OAuthStreamKey {
	Q_OBJECT

	bool uiLoaded = false;

	std::string name;
	std::string uuid;
#ifdef TWITCH_DEVICE_AUTH
	twitch::DeviceFlow deviceFlow;
	QString deviceSessionId;
	bool needsReconnect = false;
	bool keyInvalidated = false;
	bool DeviceLogin(QWidget *parent);
	void RequireReconnect(const QString &message, bool invalidateSession);
#endif

	virtual bool RetryLogin() override;

	virtual void SaveInternal() override;
	virtual bool LoadInternal() override;

	bool MakeApiRequest(const char *path, json11::Json &json_out);
	bool GetChannelInfo();

	virtual void LoadUI() override;

public:
	TwitchAuth(const Def &d);
	~TwitchAuth();

	static std::shared_ptr<Auth> Login(QWidget *parent, const std::string &service_name);
#ifdef TWITCH_DEVICE_AUTH
	static void ForgetDeviceSession();
	QString AccountName() const { return QString::fromStdString(name); }
	bool NeedsReconnect() const { return needsReconnect; }
	bool KeyInvalidated() const { return keyInvalidated; }

signals:
	void AccountStateChanged();

public:
#endif

	QTimer uiLoadTimer;

public slots:
	void TryLoadSecondaryUIPanes();
	void LoadSecondaryUIPanes();
};
