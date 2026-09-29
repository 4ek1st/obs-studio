#include "TwitchAuth.hpp"

#include <dialogs/OAuthLogin.hpp>
#include <docks/BrowserDock.hpp>
#include <utility/RemoteTextThread.hpp>
#include <utility/obf.h>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>
#include <ui-config.h>

#include <QTimer>
#include <QUuid>
#ifdef TWITCH_DEVICE_AUTH
#include "TwitchDeviceLogin.hpp"
#include "TwitchNativeDocks.hpp"
#include "TwitchTokenStore.hpp"
#include <QDesktopServices>
#endif

#include "moc_TwitchAuth.cpp"

using namespace json11;

/* ------------------------------------------------------------------------- */

#define TWITCH_AUTH_URL OAUTH_BASE_URL "v1/twitch/redirect"
#define TWITCH_TOKEN_URL OAUTH_BASE_URL "v1/twitch/token"

#define TWITCH_SCOPE_VERSION 1

#define TWITCH_CHAT_DOCK_NAME "twitchChat"
#define TWITCH_INFO_DOCK_NAME "twitchInfo"
#define TWITCH_STATS_DOCK_NAME "twitchStats"
#define TWITCH_FEED_DOCK_NAME "twitchFeed"

#ifdef TWITCH_DEVICE_AUTH
static Auth::Def twitchDef = {"Twitch", Auth::Type::OAuth_StreamKey, true, false};

static twitch::SessionStore deviceSessionStore()
{
	BPtr<char> path = GetAppConfigPathPtr("obs-studio/twitch-device-sessions");
	return twitch::SessionStore(path ? QString::fromUtf8(path.Get()) : QString{});
}
#else
static Auth::Def twitchDef = {"Twitch", Auth::Type::OAuth_StreamKey};
#endif

/* ------------------------------------------------------------------------- */

TwitchAuth::TwitchAuth(const Def &d) : OAuthStreamKey(d)
{
#ifdef TWITCH_DEVICE_AUTH
	connect(&deviceFlow, &twitch::DeviceFlow::credentialsChanged, this, [this] {
		const auto &credentials = deviceFlow.credentials();
		name = credentials.login.toStdString();
		token = credentials.accessToken.toStdString();
		refresh_token = credentials.refreshToken.toStdString();
		if (auto chat = dynamic_cast<twitch::ChatDock *>(OBSBasic::Get()->findChild<QDockWidget *>(TWITCH_CHAT_DOCK_NAME)))
			chat->setCredentials(credentials);
		if (auto info = dynamic_cast<twitch::StreamInfoDock *>(OBSBasic::Get()->findChild<QDockWidget *>(TWITCH_INFO_DOCK_NAME)))
			info->setCredentials(credentials);
		if (OBSBasic::Get()->GetAuth() == this) Auth::Save();
	});
	connect(&deviceFlow, &twitch::DeviceFlow::authenticated, this, [this] {
		needsReconnect = false;
		keyInvalidated = false;
		deviceVerified = true;
		key_ = deviceFlow.streamKey().toStdString();
		if (OBSBasic::Get()->GetAuth() == this) {
			OAuthStreamKey::OnStreamConfig();
			OBSBasic::Get()->SaveService();
			LoadUI();
		}
		emit AccountStateChanged();
	});
	connect(&deviceFlow, &twitch::DeviceFlow::failed, this, [this](const QString &message, bool reauthorize) {
		blog(LOG_WARNING, "Twitch device authorization: %s", QT_TO_UTF8(message));
		if (reauthorize) RequireReconnect(message, true);
	});
	connect(&deviceFlow, &twitch::DeviceFlow::permissionsRequired, this,
		[this](const QString &) { RequireReconnect(QTStr("TwitchAuth.Device.PermissionsRequired"), false); });
#endif
	if (!cef) {
		return;
	}

	cef->add_popup_whitelist_url("https://twitch.tv/popout/frankerfacez/chat?ffz-settings", this);

	/* enables javascript-based popups.  basically bttv popups */
	cef->add_popup_whitelist_url("about:blank#blocked", this);

	uiLoadTimer.setSingleShot(true);
	uiLoadTimer.setInterval(500);
	connect(&uiLoadTimer, &QTimer::timeout, this, &TwitchAuth::TryLoadSecondaryUIPanes);
}

TwitchAuth::~TwitchAuth()
{
	if (!uiLoaded) {
		return;
	}

	OBSBasic *main = OBSBasic::Get();

	main->RemoveDockWidget(TWITCH_CHAT_DOCK_NAME);
	main->RemoveDockWidget(TWITCH_INFO_DOCK_NAME);
	main->RemoveDockWidget(TWITCH_STATS_DOCK_NAME);
	main->RemoveDockWidget(TWITCH_FEED_DOCK_NAME);
}

bool TwitchAuth::MakeApiRequest(const char *path, Json &json_out)
{
	std::string client_id = TWITCH_CLIENTID;
	deobfuscate_str(&client_id[0], TWITCH_HASH);

	std::string url = "https://api.twitch.tv/helix/";
	url += std::string(path);

	std::vector<std::string> headers;
	headers.push_back(std::string("Client-ID: ") + client_id);
	headers.push_back(std::string("Authorization: Bearer ") + token);

	std::string output;
	std::string error;
	long error_code = 0;

	bool success = false;

	auto func = [&]() {
		success = GetRemoteFile(url.c_str(), output, error, &error_code, "application/json", "", nullptr,
					headers, nullptr, 5);
	};

	ExecThreadedWithoutBlocking(func, QTStr("Auth.LoadingChannel.Title"),
				    QTStr("Auth.LoadingChannel.Text").arg(service()));
	if (error_code == 403) {
		OBSMessageBox::warning(OBSBasic::Get(), Str("TwitchAuth.TwoFactorFail.Title"),
				       Str("TwitchAuth.TwoFactorFail.Text"), true);
		blog(LOG_WARNING, "%s: %s. API response: %s", __FUNCTION__,
		     "Got 403 from Twitch, user probably does not "
		     "have two-factor authentication enabled on "
		     "their account",
		     output.empty() ? "<none>" : output.c_str());
		return false;
	}

	if (!success || output.empty()) {
		throw ErrorInfo("Failed to get text from remote", error);
	}

	json_out = Json::parse(output, error);
	if (!error.empty()) {
		throw ErrorInfo("Failed to parse json", error);
	}

	error = json_out["error"].string_value();
	if (!error.empty()) {
		throw ErrorInfo(error, json_out["message"].string_value());
	}

	return true;
}

bool TwitchAuth::GetChannelInfo()
try {
#ifdef TWITCH_DEVICE_AUTH
	return !key_.empty();
#else
	std::string client_id = TWITCH_CLIENTID;
	deobfuscate_str(&client_id[0], TWITCH_HASH);

	if (!GetToken(TWITCH_TOKEN_URL, client_id, TWITCH_SCOPE_VERSION)) {
		return false;
	}
	if (token.empty()) {
		return false;
	}
	if (!key_.empty()) {
		return true;
	}

	Json json;
	bool success = MakeApiRequest("users", json);

	if (!success) {
		return false;
	}

	name = json["data"][0]["login"].string_value();

	std::string path = "streams/key?broadcaster_id=" + json["data"][0]["id"].string_value();
	success = MakeApiRequest(path.c_str(), json);
	if (!success) {
		return false;
	}

	key_ = json["data"][0]["stream_key"].string_value();

	return true;
#endif
} catch (ErrorInfo info) {
	QString title = QTStr("Auth.ChannelFailure.Title");
	QString text = QTStr("Auth.ChannelFailure.Text").arg(service(), info.message.c_str(), info.error.c_str());

	QMessageBox::warning(OBSBasic::Get(), title, text);

	blog(LOG_WARNING, "%s: %s: %s", __FUNCTION__, info.message.c_str(), info.error.c_str());
	return false;
}

void TwitchAuth::SaveInternal()
{
#ifdef TWITCH_DEVICE_AUTH
	if (needsReconnect) return;
#endif
	OBSBasic *main = OBSBasic::Get();
	config_set_string(main->Config(), service(), "Name", name.c_str());
	config_set_string(main->Config(), service(), "UUID", uuid.c_str());

	if (uiLoaded) {
		config_set_string(main->Config(), service(), "DockState", main->saveState().toBase64().constData());
	}
#ifdef TWITCH_DEVICE_AUTH
	const auto &credentials = deviceFlow.credentials();
	if (credentials.refreshToken.isEmpty()) return;
	if (deviceSessionId.isEmpty()) deviceSessionId = twitch::SessionStore::newId();
	if (!deviceSessionStore().write(deviceSessionId, credentials)) {
		blog(LOG_ERROR, "Could not save Twitch device credentials protected with Windows DPAPI");
		return;
	}
	config_set_string(main->Config(), service(), "DeviceSessionId", QT_TO_UTF8(deviceSessionId));
	config_set_string(main->Config(), service(), "PublicClientId", QT_TO_UTF8(credentials.clientId));
	// This login is never written through OAuth's plain-text token storage.
	config_remove_value(main->Config(), service(), "DeviceSession");
	config_remove_value(main->Config(), service(), "Token");
	config_remove_value(main->Config(), service(), "RefreshToken");
#else
	OAuthStreamKey::SaveInternal();
#endif
}

static inline std::string get_config_str(OBSBasic *main, const char *section, const char *name)
{
	const char *val = config_get_string(main->Config(), section, name);
	return val ? val : "";
}

bool TwitchAuth::LoadInternal()
{
#ifndef TWITCH_DEVICE_AUTH
	if (!cef) {
		return false;
	}
#endif

	OBSBasic *main = OBSBasic::Get();
	name = get_config_str(main, service(), "Name");
	uuid = get_config_str(main, service(), "UUID");

	firstLoad = false;
#ifdef TWITCH_DEVICE_AUTH
	deviceSessionId = QString::fromStdString(get_config_str(main, service(), "DeviceSessionId"));
	auto credentials = deviceSessionStore().read(deviceSessionId);
	if (!credentials) {
		// A legacy/imported profile is not evidence of server-side revocation.
		RequireReconnect(QTStr("TwitchAuth.Device.LocalSessionMissing"), false);
		return false;
	}
	deviceFlow.restore(*credentials);
	return true;
#else
	return OAuthStreamKey::LoadInternal();
#endif
}

static const char *ffz_script = "\
var ffz = document.createElement('script');\
ffz.setAttribute('src','https://cdn.frankerfacez.com/script/script.min.js');\
document.head.appendChild(ffz);";

static const char *bttv_script = "\
localStorage.setItem('bttv_clickTwitchEmotes', true);\
localStorage.setItem('bttv_darkenedMode', true);\
localStorage.setItem('bttv_bttvGIFEmotes', true);\
var bttv = document.createElement('script');\
bttv.setAttribute('src','https://cdn.betterttv.net/betterttv.js');\
document.head.appendChild(bttv);";

static const char *referrer_script1 = "\
Object.defineProperty(document, 'referrer', {get : function() { return '";
static const char *referrer_script2 = "'; }});";

void TwitchAuth::LoadUI()
{
#ifdef TWITCH_DEVICE_AUTH
	if (OBSBasic::Get()->GetAuth() == this && !deviceFlow.credentials().refreshToken.isEmpty()) Auth::Save();
	if (!deviceVerified || uiLoaded || deviceFlow.credentials().accessToken.isEmpty() ||
	    deviceFlow.credentials().login.isEmpty()) return;
	OBSBasic *main = OBSBasic::Get();
	const bool existingNativeLayout = config_get_bool(main->Config(), service(), "NativeDocksInitialized");
	auto translate = [](const char *key) { return QTStr(key); };
	auto chat = new twitch::ChatDock(main, translate);
	chat->setObjectName(TWITCH_CHAT_DOCK_NAME);
	chat->resize(320, 600);
	main->AddDockWidget(chat, Qt::RightDockWidgetArea);
	auto info = new twitch::StreamInfoDock(QUrl("https://api.twitch.tv/helix/"), main, translate);
	info->setObjectName(TWITCH_INFO_DOCK_NAME);
	info->resize(360, 440);
	main->AddDockWidget(info, Qt::RightDockWidgetArea);
	chat->setCredentials(deviceFlow.credentials());
	info->setCredentials(deviceFlow.credentials());
	if (existingNativeLayout) {
		const char *dockStateStr = config_get_string(main->Config(), service(), "DockState");
		main->restoreState(QByteArray::fromBase64(QByteArray(dockStateStr ? dockStateStr : "")));
	} else {
		const QPoint origin = main->pos();
		const QSize size = main->frameSize();
		chat->setFloating(true);
		info->setFloating(true);
		chat->move(origin.x() + size.width() - chat->width() - 40, origin.y() + 50);
		info->move(origin.x() + 40, origin.y() + 50);
		chat->show();
		info->show();
		config_set_bool(main->Config(), service(), "NativeDocksInitialized", true);
	}
	uiLoaded = true;
	Auth::Save();
	return;
#else
	if (!cef) {
		return;
	}
	if (uiLoaded) {
		return;
	}
	if (!GetChannelInfo()) {
		return;
	}

	OBSBasic::InitBrowserPanelSafeBlock();
	OBSBasic *main = OBSBasic::Get();

	QCefWidget *browser;
	std::string url;
	std::string script;

	/* Twitch panels require a UUID, it does not actually need to be unique,
	 * and is generated client-side.
	 * It is only for preferences stored in the browser's local store. */
	if (uuid.empty()) {
		QString qtUuid = QUuid::createUuid().toString();
		qtUuid.replace(QRegularExpression("[{}-]"), "");
		uuid = qtUuid.toStdString();
	}

	std::string moderation_tools_url;
	moderation_tools_url = "https://www.twitch.tv/";
	moderation_tools_url += name;
	moderation_tools_url += "/dashboard/settings/moderation?no-reload=true";

	/* ----------------------------------- */

	url = "https://www.twitch.tv/popout/";
	url += name;
	url += "/chat";

	QSize size = main->frameSize();
	QPoint pos = main->pos();

	BrowserDock *chat = new BrowserDock(QTStr("Auth.Chat"));
	chat->setObjectName(TWITCH_CHAT_DOCK_NAME);
	chat->resize(300, 600);
	chat->setMinimumSize(200, 300);
	chat->setWindowTitle(QTStr("Auth.Chat"));
	chat->setAllowedAreas(Qt::AllDockWidgetAreas);

	browser = cef->create_widget(chat, url, panel_cookies);
	chat->SetWidget(browser);
	cef->add_force_popup_url(moderation_tools_url, chat);

	if (App()->IsThemeDark()) {
		script = "localStorage.setItem('twilight.theme', 1);";
	} else {
		script = "localStorage.setItem('twilight.theme', 0);";
	}

	const int twAddonChoice = config_get_int(main->Config(), service(), "AddonChoice");
	if (twAddonChoice) {
		if (twAddonChoice & 0x1) {
			script += bttv_script;
		}
		if (twAddonChoice & 0x2) {
			script += ffz_script;
		}
	}

	browser->setStartupScript(script);

	main->AddDockWidget(chat, Qt::RightDockWidgetArea);

	/* ----------------------------------- */

	chat->setFloating(true);
	chat->move(pos.x() + size.width() - chat->width() - 50, pos.y() + 50);

	if (firstLoad) {
		chat->setVisible(true);
	} else {
		const char *dockStateStr = config_get_string(main->Config(), service(), "DockState");
		QByteArray dockState = QByteArray::fromBase64(QByteArray(dockStateStr));
		main->restoreState(dockState);
	}

	TryLoadSecondaryUIPanes();

	uiLoaded = true;
#endif
}

void TwitchAuth::LoadSecondaryUIPanes()
{
	OBSBasic *main = OBSBasic::Get();

	QCefWidget *browser;
	std::string url;
	std::string script;

	QSize size = main->frameSize();
	QPoint pos = main->pos();

	if (App()->IsThemeDark()) {
		script = "localStorage.setItem('twilight.theme', 1);";
	} else {
		script = "localStorage.setItem('twilight.theme', 0);";
	}
	script += referrer_script1;
	script += "https://www.twitch.tv/";
	script += name;
	script += "/dashboard/live";
	script += referrer_script2;

	const int twAddonChoice = config_get_int(main->Config(), service(), "AddonChoice");
	if (twAddonChoice) {
		if (twAddonChoice & 0x1) {
			script += bttv_script;
		}
		if (twAddonChoice & 0x2) {
			script += ffz_script;
		}
	}

	/* ----------------------------------- */

	url = "https://dashboard.twitch.tv/popout/u/";
	url += name;
	url += "/stream-manager/edit-stream-info";

	BrowserDock *info = new BrowserDock(QTStr("Auth.StreamInfo"));
	info->setObjectName(TWITCH_INFO_DOCK_NAME);
	info->resize(300, 650);
	info->setMinimumSize(200, 300);
	info->setWindowTitle(QTStr("Auth.StreamInfo"));
	info->setAllowedAreas(Qt::AllDockWidgetAreas);

	browser = cef->create_widget(info, url, panel_cookies);
	info->SetWidget(browser);
	browser->setStartupScript(script);

	main->AddDockWidget(info, Qt::RightDockWidgetArea);

	/* ----------------------------------- */

	url = "https://www.twitch.tv/popout/";
	url += name;
	url += "/dashboard/live/stats";

	BrowserDock *stats = new BrowserDock(QTStr("TwitchAuth.Stats"));
	stats->setObjectName(TWITCH_STATS_DOCK_NAME);
	stats->resize(200, 250);
	stats->setMinimumSize(200, 150);
	stats->setWindowTitle(QTStr("TwitchAuth.Stats"));
	stats->setAllowedAreas(Qt::AllDockWidgetAreas);

	browser = cef->create_widget(stats, url, panel_cookies);
	stats->SetWidget(browser);
	browser->setStartupScript(script);

	main->AddDockWidget(stats, Qt::RightDockWidgetArea);

	/* ----------------------------------- */

	url = "https://dashboard.twitch.tv/popout/u/";
	url += name;
	url += "/stream-manager/activity-feed";
	url += "?uuid=" + uuid;

	BrowserDock *feed = new BrowserDock(QTStr("TwitchAuth.Feed"));
	feed->setObjectName(TWITCH_FEED_DOCK_NAME);
	feed->resize(300, 650);
	feed->setMinimumSize(200, 300);
	feed->setWindowTitle(QTStr("TwitchAuth.Feed"));
	feed->setAllowedAreas(Qt::AllDockWidgetAreas);

	browser = cef->create_widget(feed, url, panel_cookies);
	feed->SetWidget(browser);
	browser->setStartupScript(script);

	main->AddDockWidget(feed, Qt::RightDockWidgetArea);

	/* ----------------------------------- */

	info->setFloating(true);
	stats->setFloating(true);
	feed->setFloating(true);

	QSize statSize = stats->frameSize();

	info->move(pos.x() + 50, pos.y() + 50);
	stats->move(pos.x() + size.width() / 2 - statSize.width() / 2,
		    pos.y() + size.height() / 2 - statSize.height() / 2);
	feed->move(pos.x() + 100, pos.y() + 100);

	if (firstLoad) {
		info->setVisible(true);
		stats->setVisible(false);
		feed->setVisible(false);
	} else {
		uint32_t lastVersion = config_get_int(App()->GetAppConfig(), "General", "LastVersion");

		if (lastVersion <= MAKE_SEMANTIC_VERSION(23, 0, 2)) {
			feed->setVisible(false);
		}

		const char *dockStateStr = config_get_string(main->Config(), service(), "DockState");
		QByteArray dockState = QByteArray::fromBase64(QByteArray(dockStateStr));

		if (main->isVisible() || !main->isMaximized()) {
			main->restoreState(dockState);
		}
	}
}

/* Twitch.tv has an OAuth for itself.  If we try to load multiple panel pages
 * at once before it's OAuth'ed itself, they will all try to perform the auth
 * process at the same time, get their own request codes, and only the last
 * code will be valid -- so one or more panels are guaranteed to fail.
 *
 * To solve this, we want to load just one panel first (the chat), and then all
 * subsequent panels should only be loaded once we know that Twitch has auth'ed
 * itself (if the cookie "auth-token" exists for twitch.tv).
 *
 * This is annoying to deal with. */
void TwitchAuth::TryLoadSecondaryUIPanes()
{
	QPointer<TwitchAuth> this_ = this;

	auto cb = [this_](bool found) {
		if (!this_) {
			return;
		}

		if (!found) {
			QMetaObject::invokeMethod(this_, [this_]() { this_->uiLoadTimer.start(); });
		} else {
			QMetaObject::invokeMethod(this_, &TwitchAuth::LoadSecondaryUIPanes);
		}
	};

	panel_cookies->CheckForCookie("https://www.twitch.tv", "auth-token", cb);
}

bool TwitchAuth::RetryLogin()
{
#ifdef TWITCH_DEVICE_AUTH
	return DeviceLogin(OBSBasic::Get());
#else
	OAuthLogin login(OBSBasic::Get(), TWITCH_AUTH_URL, false);
	if (login.exec() == QDialog::Rejected) {
		return false;
	}

	std::shared_ptr<TwitchAuth> auth = std::make_shared<TwitchAuth>(twitchDef);
	std::string client_id = TWITCH_CLIENTID;
	deobfuscate_str(&client_id[0], TWITCH_HASH);

	return GetToken(TWITCH_TOKEN_URL, client_id, TWITCH_SCOPE_VERSION, QT_TO_UTF8(login.GetCode()), true);
#endif
}

std::shared_ptr<Auth> TwitchAuth::Login(QWidget *parent, const std::string &)
{
#ifdef TWITCH_DEVICE_AUTH
	auto auth = std::make_shared<TwitchAuth>(twitchDef);
	return auth->DeviceLogin(parent) ? auth : nullptr;
#else
	OAuthLogin login(parent, TWITCH_AUTH_URL, false);
	if (login.exec() == QDialog::Rejected) {
		return nullptr;
	}

	std::shared_ptr<TwitchAuth> auth = std::make_shared<TwitchAuth>(twitchDef);

	std::string client_id = TWITCH_CLIENTID;
	deobfuscate_str(&client_id[0], TWITCH_HASH);

	if (!auth->GetToken(TWITCH_TOKEN_URL, client_id, TWITCH_SCOPE_VERSION, QT_TO_UTF8(login.GetCode()))) {
		return nullptr;
	}

	if (auth->GetChannelInfo()) {
		return auth;
	}

	return nullptr;
#endif
}

#ifdef TWITCH_DEVICE_AUTH
void TwitchAuth::RequireReconnect(const QString &message, bool invalidateSession)
{
	needsReconnect = true;
	keyInvalidated = invalidateSession;
	deviceVerified = false;
	key_.clear();
	deviceFlow.cancel();
	if (invalidateSession && !deviceSessionId.isEmpty()) deviceSessionStore().remove(deviceSessionId);
	emit AccountStateChanged();
	auto main = OBSBasic::Get();
	if (main->GetAuth() != this) return;
	if (invalidateSession) {
		OBSDataAutoRelease settings = obs_service_get_settings(main->GetService());
		obs_data_set_string(settings, "key", "");
		obs_service_update(main->GetService(), settings);
		main->SaveService();
		config_remove_value(main->Config(), service(), "DeviceSessionId");
	}
	QPointer<TwitchAuth> self(this);
	QMetaObject::invokeMethod(main, [self, message] {
		if (!self || OBSBasic::Get()->GetAuth() != self) return;
		config_remove_value(OBSBasic::Get()->Config(), "Auth", "Type");
		Auth::Load();
		config_save_safe(OBSBasic::Get()->Config(), "tmp", nullptr);
		QMessageBox::warning(OBSBasic::Get(), QTStr("TwitchAuth.Device.Title"), message);
	}, Qt::QueuedConnection);
}

bool TwitchAuth::DeviceLogin(QWidget *parent)
{
	QString clientId = QString::fromStdString(get_config_str(OBSBasic::Get(), service(), "PublicClientId"));
	if (clientId.isEmpty()) clientId = QString::fromUtf8(TWITCH_PUBLIC_CLIENTID);
	twitch::DeviceLogin login(deviceFlow, clientId, parent, [](const char *key) { return QTStr(key); });
	connect(&login, &twitch::DeviceLogin::openBrowser, &login, [](const QUrl &url) { QDesktopServices::openUrl(url); });
	return login.exec() == QDialog::Accepted;
}

void TwitchAuth::ForgetDeviceSession()
{
	auto config = OBSBasic::Get()->Config();
	const char *id = config_get_string(config, "Twitch", "DeviceSessionId");
	if (id) deviceSessionStore().remove(QString::fromUtf8(id));
	if (auto auth = dynamic_cast<TwitchAuth *>(OBSBasic::Get()->GetAuth())) {
		auth->deviceFlow.cancel();
		auth->needsReconnect = true;
		auth->keyInvalidated = true;
		auth->key_.clear();
		emit auth->AccountStateChanged();
	}
	for (const char *key : {"DeviceSessionId", "DeviceSession", "Token", "RefreshToken", "ExpireTime", "ScopeVer", "Name"})
		config_remove_value(config, "Twitch", key);
	config_save_safe(config, "tmp", nullptr);
}
#endif

static std::shared_ptr<Auth> CreateTwitchAuth()
{
	return std::make_shared<TwitchAuth>(twitchDef);
}

static void DeleteCookies()
{
	if (panel_cookies) {
		panel_cookies->DeleteCookies("twitch.tv", std::string());
	}
}

void RegisterTwitchAuth()
{
#if !defined(__APPLE__) && !defined(_WIN32)
	if (QApplication::platformName().contains("wayland")) {
		return;
	}
#endif

	OAuth::RegisterOAuth(twitchDef, CreateTwitchAuth, TwitchAuth::Login, DeleteCookies);
}
