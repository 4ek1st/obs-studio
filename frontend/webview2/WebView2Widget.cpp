#include "WebView2Widget.hpp"
#include "BridgeProtocol.hpp"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <Windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct WebView2Widget::Impl {
	QString assets;
	QString profile;
	bool comInitialized = false;
	bool loaded = false;
	ComPtr<ICoreWebView2Environment> environment;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> webview;
};

WebView2Widget::WebView2Widget(QWidget *parent, QString assetsPath, QString profilePath)
	: QWidget(parent), impl(std::make_unique<Impl>())
{
	impl->assets = QDir(assetsPath).absolutePath();
	impl->profile = QDir(profilePath).absolutePath();
	setAttribute(Qt::WA_NativeWindow);
	setFocusPolicy(Qt::StrongFocus);
	QTimer::singleShot(0, this, [this] { initialize(); });
}

WebView2Widget::~WebView2Widget()
{
	impl->loaded = false;
	if (impl->controller)
		impl->controller->Close();
	impl->webview.Reset();
	impl->controller.Reset();
	impl->environment.Reset();
	if (impl->comInitialized)
		CoUninitialize();
}

void WebView2Widget::reportFailure(const QString &stage, long result)
{
	impl->loaded = false;
	const auto detail = stage + QStringLiteral(" (0x%1)").arg(static_cast<unsigned long>(result), 8, 16, QLatin1Char('0'));
	// Never enter a nested Qt event loop from a WebView2 callback.
	QTimer::singleShot(0, this, [this, detail] { emit failed(detail); });
}

void WebView2Widget::initialize()
{
	if (!QFileInfo::exists(impl->assets + QStringLiteral("/index.html")) ||
	    !QDir().mkpath(impl->profile)) {
		reportFailure(QStringLiteral("WebView2 local resources or profile unavailable"), E_FAIL);
		return;
	}
	const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(apartment)) {
		reportFailure(QStringLiteral("WebView2 requires an STA UI thread"), apartment);
		return;
	}
	impl->comInitialized = true;
	const QPointer<WebView2Widget> guard(this);
	const auto profile = impl->profile.toStdWString();
	const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
		nullptr, profile.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[guard](HRESULT hr, ICoreWebView2Environment *environment) -> HRESULT {
				if (!guard)
					return S_OK;
				if (FAILED(hr) || !environment) {
					guard->reportFailure(QStringLiteral("Cannot initialize WebView2 Runtime"), FAILED(hr) ? hr : E_FAIL);
					return S_OK;
				}
				guard->impl->environment = environment;
				const HRESULT created = environment->CreateCoreWebView2Controller(
					reinterpret_cast<HWND>(guard->winId()),
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[guard](HRESULT controllerResult, ICoreWebView2Controller *controller) -> HRESULT {
							if (!guard) {
								if (controller)
									controller->Close();
								return S_OK;
							}
							if (FAILED(controllerResult) || !controller) {
								guard->reportFailure(QStringLiteral("Cannot create WebView2 controller"),
										     FAILED(controllerResult) ? controllerResult : E_FAIL);
								return S_OK;
							}
							auto &state = *guard->impl;
							state.controller = controller;
							if (FAILED(controller->get_CoreWebView2(&state.webview))) {
								guard->reportFailure(QStringLiteral("Cannot access WebView2"), E_FAIL);
								return S_OK;
							}
							ComPtr<ICoreWebView2_3> localContent;
							if (FAILED(state.webview.As(&localContent))) {
								guard->reportFailure(QStringLiteral("Update WebView2 Runtime"), E_NOINTERFACE);
								return S_OK;
							}
							const auto directory = state.assets.toStdWString();
							const HRESULT mapping = localContent->SetVirtualHostNameToFolderMapping(
								L"obs-ui.local", directory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
							if (FAILED(mapping)) {
								guard->reportFailure(QStringLiteral("Cannot map WebView2 resources"), mapping);
								return S_OK;
							}
							ComPtr<ICoreWebView2Settings> settings;
							if (SUCCEEDED(state.webview->get_Settings(&settings))) {
								settings->put_AreDefaultContextMenusEnabled(FALSE);
								settings->put_AreDevToolsEnabled(FALSE);
								settings->put_IsStatusBarEnabled(FALSE);
								settings->put_IsZoomControlEnabled(FALSE);
								settings->put_AreHostObjectsAllowed(FALSE);
								ComPtr<ICoreWebView2Settings3> keyboardSettings;
								if (SUCCEEDED(settings.As(&keyboardSettings)))
									keyboardSettings->put_AreBrowserAcceleratorKeysEnabled(FALSE);
							}
							EventRegistrationToken token;
							state.webview->add_NavigationStarting(
								Callback<ICoreWebView2NavigationStartingEventHandler>(
									[](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
										LPWSTR uri = nullptr;
										args->get_Uri(&uri);
										const bool local = uri && OBSWeb::IsLocalUi(QUrl(QString::fromWCharArray(uri)));
										CoTaskMemFree(uri);
										if (!local)
											args->put_Cancel(TRUE);
										return S_OK;
									}).Get(), &token);
							state.webview->add_NewWindowRequested(
								Callback<ICoreWebView2NewWindowRequestedEventHandler>(
									[](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
										return args->put_Handled(TRUE);
									}).Get(), &token);
							state.webview->add_PermissionRequested(
								Callback<ICoreWebView2PermissionRequestedEventHandler>(
									[](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
										return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
									}).Get(), &token);
							ComPtr<ICoreWebView2_4> downloads;
							if (SUCCEEDED(state.webview.As(&downloads)))
								downloads->add_DownloadStarting(
									Callback<ICoreWebView2DownloadStartingEventHandler>(
										[](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
											return args->put_Cancel(TRUE);
										}).Get(), &token);
							state.webview->add_WebMessageReceived(
								Callback<ICoreWebView2WebMessageReceivedEventHandler>(
									[guard](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
										if (!guard)
											return S_OK;
										LPWSTR source = nullptr;
										LPWSTR json = nullptr;
										args->get_Source(&source);
										args->get_WebMessageAsJson(&json);
										QString error;
										const auto request = OBSWeb::ParseRequest(
											source ? QString::fromWCharArray(source) : QString(),
											json ? QString::fromWCharArray(json).toUtf8() : QByteArray(), error);
										CoTaskMemFree(source);
										CoTaskMemFree(json);
										if (request) {
											const QJsonObject message{{QStringLiteral("version"), 1},
														  {QStringLiteral("id"), request->id},
														  {QStringLiteral("command"), request->command},
														  {QStringLiteral("args"), request->args}};
											QTimer::singleShot(0, guard.data(), [guard, message] {
												if (guard && guard->impl->loaded)
													emit guard->messageReceived(message);
											});
										}
										return S_OK;
									}).Get(), &token);
							state.webview->add_NavigationCompleted(
								Callback<ICoreWebView2NavigationCompletedEventHandler>(
									[guard](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
										if (!guard)
											return S_OK;
										BOOL success = FALSE;
										args->get_IsSuccess(&success);
										if (!success) {
											COREWEBVIEW2_WEB_ERROR_STATUS status;
											args->get_WebErrorStatus(&status);
											if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
												guard->reportFailure(QStringLiteral("Cannot load local WebView2 interface"), E_FAIL);
											return S_OK;
										}
										guard->impl->loaded = true;
										QTimer::singleShot(0, guard.data(), [guard] {
											if (guard)
												emit guard->ready();
										});
										return S_OK;
									}).Get(), &token);
							state.webview->add_ProcessFailed(
								Callback<ICoreWebView2ProcessFailedEventHandler>(
									[guard](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
										if (guard)
											guard->reportFailure(QStringLiteral("WebView2 process stopped; reopen the interface"), E_FAIL);
										return S_OK;
									}).Get(), &token);
							guard->updateBounds();
							controller->put_IsVisible(guard->isVisible());
							const HRESULT navigate = state.webview->Navigate(L"https://obs-ui.local/index.html");
							if (FAILED(navigate))
								guard->reportFailure(QStringLiteral("Cannot navigate to local WebView2 interface"), navigate);
							return S_OK;
						}).Get());
				if (FAILED(created))
					guard->reportFailure(QStringLiteral("Cannot request WebView2 controller"), created);
				return S_OK;
			}).Get());
	if (FAILED(result))
		reportFailure(QStringLiteral("Microsoft Edge WebView2 Runtime is unavailable. Install or repair the Evergreen Runtime from microsoft.com/edge/webview2"), result);
}

void WebView2Widget::postMessage(const QJsonObject &message)
{
	if (!impl->loaded || !impl->webview)
		return;
	const auto json = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)).toStdWString();
	const HRESULT result = impl->webview->PostWebMessageAsJson(json.c_str());
	if (FAILED(result))
		reportFailure(QStringLiteral("Cannot deliver WebView2 message"), result);
}

void WebView2Widget::updateBounds()
{
	if (!impl->controller)
		return;
	RECT bounds{};
	GetClientRect(reinterpret_cast<HWND>(winId()), &bounds);
	impl->controller->put_Bounds(bounds);
	impl->controller->NotifyParentWindowPositionChanged();
}

void WebView2Widget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	updateBounds();
}

void WebView2Widget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	updateBounds();
	if (impl->controller)
		impl->controller->put_IsVisible(TRUE);
}

void WebView2Widget::hideEvent(QHideEvent *event)
{
	if (impl->controller)
		impl->controller->put_IsVisible(FALSE);
	QWidget::hideEvent(event);
}

void WebView2Widget::focusInEvent(QFocusEvent *event)
{
	QWidget::focusInEvent(event);
	if (impl->controller)
		impl->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

bool WebView2Widget::event(QEvent *event)
{
	const bool result = QWidget::event(event);
	if (impl && impl->controller && (event->type() == QEvent::Move || event->type() == QEvent::ScreenChangeInternal))
		updateBounds();
	return result;
}

#include "moc_WebView2Widget.cpp"
