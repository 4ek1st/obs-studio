#include "WebView2Widget.hpp"
#include "BridgeProtocol.hpp"
#include "WebView2Drop.hpp"
#include <QApplication>
#include <QSaveFile>

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
	QString document;
	bool comInitialized = false;
	bool loaded = false;
	bool boundsUpdatePending = false;
	ComPtr<ICoreWebView2Environment> environment;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> webview;
};

WebView2Widget::WebView2Widget(QWidget *parent, QString assetsPath, QString profilePath, QString document)
	: QWidget(parent), impl(std::make_unique<Impl>())
{
	impl->assets = QDir(assetsPath).absolutePath();
	impl->profile = QDir(profilePath).absolutePath();
	impl->document = std::move(document);
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
	if (QFileInfo(impl->document).fileName() != impl->document || !impl->document.endsWith(QStringLiteral(".html")) ||
	    !QFileInfo::exists(impl->assets + QLatin1Char('/') + impl->document) ||
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
								ComPtr<ICoreWebView2Settings4> autofill;
								if (SUCCEEDED(settings.As(&autofill))) {
									autofill->put_IsPasswordAutosaveEnabled(FALSE);
									autofill->put_IsGeneralAutofillEnabled(FALSE);
								}
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
										const QString sourceUrl = source ? QString::fromWCharArray(source) : QString();
										const auto request = OBSWeb::ParseRequest(
											sourceUrl,
											json ? QString::fromWCharArray(json).toUtf8() : QByteArray(), error);
										CoTaskMemFree(source);
										CoTaskMemFree(json);
										if (request) {
											if (request->command == QStringLiteral("external.drop")) {
												std::optional<OBSWeb::ExternalDropData> drop;
												if (guard->impl->document == QStringLiteral("index.html") && OBSWeb::IsExternalDropOrigin(sourceUrl))
													drop = OBSWeb::ReadExternalDrop(args, request->args, error);
												else error = QStringLiteral("External drops are only accepted by the workspace.");
												const QString id = request->id;
												QTimer::singleShot(0, guard.data(), [guard, drop, error, id] {
													if (!guard || !guard->impl->loaded) return;
													if (drop) emit guard->externalDrop(id, *drop);
													else guard->postMessage({{"version", 1}, {"id", id}, {"ok", false},
														{"error", QJsonObject{{"code", "InvalidDrop"}, {"message", error}}}});
												});
												return S_OK;
											}
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
											if (guard)
												guard->queueBoundsUpdate();
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
							guard->queueBoundsUpdate();
							controller->put_IsVisible(guard->isVisible());
							const auto url = (QStringLiteral("https://obs-ui.local/") + state.document).toStdWString();
							const HRESULT navigate = state.webview->Navigate(url.c_str());
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

void WebView2Widget::capturePreview(QString path, std::function<void(bool)> done)
{
	// Complete outside the COM callback: the caller may close/delete this widget.
	auto completion = std::make_shared<std::function<void(bool)>>(std::move(done));
	auto destroyedConnection = std::make_shared<QMetaObject::Connection>();
	auto finish = [completion, destroyedConnection](bool ok) {
		QObject::disconnect(*destroyedConnection);
		auto callback = std::move(*completion);
		*completion = {};
		if (callback)
			QTimer::singleShot(0, qApp, [callback = std::move(callback), ok] { callback(ok); });
	};
	*destroyedConnection = connect(this, &QObject::destroyed, qApp, [finish] { finish(false); });
	if (!impl->loaded || !impl->webview || path.isEmpty()) {
		finish(false);
		return;
	}
	if (qEnvironmentVariableIsSet("OBS_WEBVIEW2_TRACE_GEOMETRY")) {
		RECT client{}, controllerBounds{};
		GetClientRect(reinterpret_cast<HWND>(winId()), &client);
		if (impl->controller) impl->controller->get_Bounds(&controllerBounds);
		auto rectJson = [](const QRect &rect) {
			return QJsonObject{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
		};
		const QJsonObject geometry{{"document", impl->document}, {"widget", rectJson(this->geometry())},
			{"parent", rectJson(parentWidget() ? parentWidget()->rect() : QRect())}, {"dpr", devicePixelRatioF()},
			{"visible", isVisible()}, {"hidden", isHidden()}, {"mask", rectJson(mask().boundingRect())},
			{"hwnd", rectJson(QRect(client.left, client.top, client.right-client.left, client.bottom-client.top))},
			{"controller", rectJson(QRect(controllerBounds.left, controllerBounds.top,
				controllerBounds.right-controllerBounds.left, controllerBounds.bottom-controllerBounds.top))}};
		QSaveFile trace(path + QStringLiteral(".geometry.json"));
		if (trace.open(QIODevice::WriteOnly)) {
			trace.write(QJsonDocument(geometry).toJson());
			trace.commit();
		}
	}
	ComPtr<IStream> stream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
		finish(false);
		return;
	}
	QPointer<WebView2Widget> guard(this);
	const HRESULT started = impl->webview->CapturePreview(
		COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
		Callback<ICoreWebView2CapturePreviewCompletedHandler>(
			[guard, stream, path = std::move(path), finish](HRESULT result) -> HRESULT {
				if (!guard || FAILED(result)) {
					finish(false);
					return S_OK;
				}
				STATSTG stat{};
				LARGE_INTEGER start{};
				constexpr ULONGLONG maximumBytes = 64ULL * 1024 * 1024;
				if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart ||
				    stat.cbSize.QuadPart > maximumBytes || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
					finish(false);
					return S_OK;
				}
				QByteArray png(static_cast<qsizetype>(stat.cbSize.QuadPart), Qt::Uninitialized);
				ULONG read = 0;
				if (FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size()) {
					finish(false);
					return S_OK;
				}
				QSaveFile file(path);
				const bool saved = file.open(QIODevice::WriteOnly) && file.write(png) == png.size() && file.commit();
				finish(saved);
				return S_OK;
			}).Get());
	if (FAILED(started))
		finish(false);
}

void WebView2Widget::updateBounds()
{
	if (!impl->controller)
		return;
	RECT bounds{};
	if (!GetClientRect(reinterpret_cast<HWND>(winId()), &bounds))
		return;
	impl->controller->put_Bounds(bounds);
	impl->controller->NotifyParentWindowPositionChanged();
}

void WebView2Widget::queueBoundsUpdate()
{
	if (impl->boundsUpdatePending)
		return;
	impl->boundsUpdatePending = true;
	// A hidden native child can still have its initial 100x30 HWND during
	// Qt show/resize callbacks. Read physical bounds again after Qt applies them.
	QTimer::singleShot(0, this, [this] {
		impl->boundsUpdatePending = false;
		updateBounds();
	});
}

void WebView2Widget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	updateBounds();
	queueBoundsUpdate();
}

void WebView2Widget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	updateBounds();
	queueBoundsUpdate();
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
	if (impl && impl->controller && (event->type() == QEvent::Move || event->type() == QEvent::ScreenChangeInternal ||
				       event->type() == QEvent::DevicePixelRatioChange)) {
		updateBounds();
		queueBoundsUpdate();
	}
	return result;
}

#include "moc_WebView2Widget.cpp"
