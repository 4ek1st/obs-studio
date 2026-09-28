// Same safe dialog tour in the original Qt frontend and the WebView frontend.
// Compiled only for disposable portable integration builds; never in releases.
#include <QWizard>
#include <QTableView>
#include <QDockWidget>
#include <QMenu>
#include <QScreen>
#include <QScrollBar>
#include <windows.h>

namespace {
class FrontendParityAudit final : public QObject {
    struct Case { const char *action; const char *type; };
    const QList<Case> cases{{"actionRemux", "OBSRemux"}, {"actionImportSceneCollection", "OBSImporter"},
        {"autoConfigure", "AutoConfig"}, {"actionShowAbout", "OBSAbout"},
        {"actionRenameProfile", "NameDialog"}, {"actionRenameSceneCollection", "NameDialog"},
        {"actionAdvAudioProperties", "OBSBasicAdvAudio"}, {"action_Settings", "OBSBasicSettings"},
        {"actionAddSource", "OBSBasicSourceSelect"}, {"actionSourceProperties", "OBSBasicProperties"},
        {"actionEditTransform", "OBSBasicTransform"}, {"@sourceFiltersButton", "OBSBasicFilters"},
        {"@virtualCamConfigButton", "OBSBasicVCamConfig"}, {"actionViewCurrentLog", "OBSLogViewer"},
        {"actionOpenPluginManager", "OBS::PluginManagerWindow"}, {"@browserDocks", "OBSExtraBrowsers"},
        {"#Scripts", "ScriptsTool"}, {"#SceneSwitcher", "SceneSwitcher"}, {"#OutputTimer", "OutputTimer"},
        {"@statsDock", "OBSBasicStats"}};
    QPointer<OBSBasic> main;
    bool web;
    QString directory;
    int caseIndex = -1, page = 0;
    QJsonArray checks, dialogs, actions, skippedCases;
    QPointer<QWidget> current;
    QTimer poll;
    QElapsedTimer wait;
    bool pending = false;
    bool finished = false;
    bool toolWorkflowsStarted = false;
    bool hadPrompt = false, hadSearch = false, oldPrompt = false, oldSearch = false;
    bool statsWasVisible = false, statsWasFloating = true;
    qint64 firstDetectedMs = -1, surfaceReadyMs = -1;
    QPointer<QWidget> raisedWindow;
    bool raisedWasTopmost = false;
    HWND raisedHandle = nullptr;
    bool captureWindowChanged = false;
    bool lastRaiseSucceeded = false;
    DWORD lastRaiseError = ERROR_SUCCESS;
    bool topmostBeforeActivation = false, topmostAfterActivation = false, topmostAfterRaise = false;
    QElapsedTimer composedWait;
    int composedAttempts = 0;
    quint64 captureGeneration = 0;
    OBSSceneAutoRelease fixtureScene;
    OBSSourceAutoRelease fixtureSource;

    void check(bool ok, const QString &name) {
        checks.append(QJsonObject{{"name", name}, {"passed", ok}});
        blog(ok ? LOG_INFO : LOG_ERROR, "[Parity audit] %s: %s", ok ? "PASS" : "FAIL", name.toUtf8().constData());
    }
    void save(const QString &name, const QJsonValue &value) {
        QSaveFile file(QDir(directory).filePath(name));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(value.isArray() ? QJsonDocument(value.toArray()).toJson() : QJsonDocument(value.toObject()).toJson());
            file.commit();
        }
    }
    void finish() {
        if (finished) return;
        restoreCaptureWindow();
        if (!toolWorkflowsStarted) {
            toolWorkflowsStarted = true; ++captureGeneration; poll.stop();
            const QPointer<FrontendParityAudit> guard(this);
            RunToolDataWorkflowChecks(main, web, [guard](bool ok, const char *name) {
                if (guard) guard->check(ok, QString::fromUtf8(name));
            }, [guard] { if (guard) guard->finish(); });
            return;
        }
        finished = true; ++captureGeneration;
        poll.stop();
        bool passed = true;
        for (const auto &entry : checks) passed &= entry.toObject().value("passed").toBool();
        save("audit.json", QJsonObject{{"frontend", web ? "webview2" : "native-qt"}, {"passed", passed},
            {"checks", checks}, {"actions", actions}, {"dialogs", dialogs}, {"skippedCases", skippedCases},
            {"scope", "Safe dialog/category tour, original widget/action contracts, real disposable Remux and scene collection import; not every tool's end-to-end workflow"}});
        auto *config = obs_frontend_get_user_config();
        if (hadPrompt) config_set_bool(config, "General", "AutoSearchPrompt", oldPrompt); else config_remove_value(config, "General", "AutoSearchPrompt");
        if (hadSearch) config_set_bool(config, "General", "AutomaticCollectionSearch", oldSearch); else config_remove_value(config, "General", "AutomaticCollectionSearch");
        config_set_bool(config, "General", "ConfirmOnExit", false);
        // OBS tears libobs down before its QWidget children. Release our test
        // references while the library and the original scene list are alive.
        fixtureSource = nullptr; fixtureScene = nullptr;
        QTimer::singleShot(100, main, &QWidget::close);
        deleteLater();
    }
    void skip(const char *action, const QString &reason) {
        skippedCases.append(QJsonObject{{"case", QString::fromLatin1(action)}, {"reason", reason}});
        blog(LOG_INFO, "[Parity audit] SKIP %s: %s", action, reason.toUtf8().constData());
        QTimer::singleShot(0, this, [this] { next(); });
    }
    QAction *menuAction(const char *menuName, const QString &title) {
        auto *menu = main->findChild<QMenu *>(QString::fromLatin1(menuName));
        if (!menu) return nullptr;
        for (auto *action : menu->actions()) if (action->text() == title && action->isVisible()) return action;
        return nullptr;
    }
    void next() {
        if (finished) return;
        restoreCaptureWindow();
        current = nullptr; page = 0; pending = false; firstDetectedMs = -1; surfaceReadyMs = -1;
        if (++caseIndex >= cases.size()) { finish(); return; }
        const auto entry = cases.at(caseIndex);
        if (entry.action[0] == '#') {
            auto *module = obs_get_module("frontend-tools");
            auto *action = module ? menuAction("menuTools", QString::fromUtf8(obs_module_get_locale_text(module, entry.action + 1))) : nullptr;
            if (!action) { skip(entry.action, module ? "Loaded frontend-tools module did not expose this QAction in menuTools" : "frontend-tools module is not loaded in this build"); return; }
            check(action->isEnabled(), QStringLiteral("Original frontend-tools action enabled: ") + QLatin1String(entry.action + 1));
            if (!action->isEnabled()) { QTimer::singleShot(0, this, [this] { next(); }); return; }
            wait.restart(); poll.start(); QTimer::singleShot(0, action, &QAction::trigger); return;
        }
        if (entry.action[0] == '@') {
            wait.restart(); poll.start();
            if (QByteArray(entry.action) == "@browserDocks") {
                auto *action = menuAction("menuDocks", QTStr("Basic.MainMenu.Docks.CustomBrowserDocks"));
                if (!action) { poll.stop(); skip(entry.action, "obs-browser did not expose Custom Browser Docks in menuDocks"); return; }
                check(action->isEnabled(), "Native custom browser dock action is enabled");
                QTimer::singleShot(0, action, &QAction::trigger);
            } else if (QByteArray(entry.action) == "@statsDock") {
                auto *dock = main->findChild<QDockWidget *>("statsDock");
                if (!dock || !dock->widget()) { poll.stop(); skip(entry.action, "The original stats dock is unavailable"); return; }
                statsWasVisible = dock->isVisible(); statsWasFloating = dock->isFloating();
                dock->setFloating(true);
                if (!dock->isVisible()) dock->toggleViewAction()->trigger();
            } else if (auto *button = main->findChild<QAbstractButton *>(QString::fromLatin1(entry.action + 1))) {
                check(button->isEnabled(), "Native auxiliary button is enabled");
                QTimer::singleShot(0, button, &QAbstractButton::click);
            } else { poll.stop(); check(false, "Auxiliary button exists"); QTimer::singleShot(0, this, [this] { next(); }); }
            return;
        }
        auto *action = main->findChild<QAction *>(QLatin1String(entry.action));
        check(action && action->isEnabled(), QStringLiteral("Action available: ") + QLatin1String(entry.action));
        if (!action || !action->isEnabled()) { QTimer::singleShot(0, this, [this] { next(); }); return; }
        wait.restart(); poll.start();
        QTimer::singleShot(0, action, &QAction::trigger);
    }
    QJsonObject node(OBSWeb::QtDialogBridge &bridge, const QString &name) {
        for (const auto &value : bridge.snapshot().value("nodes").toArray())
            if (value.toObject().value("name") == name) return value.toObject();
        return {};
    }
    void closeCurrent() {
        restoreCaptureWindow();
        if (current) {
            if (QByteArray(cases.at(caseIndex).action) == "@statsDock") {
                auto *dock = main->findChild<QDockWidget *>("statsDock");
                if (dock && dock->isVisible()) dock->toggleViewAction()->trigger();
            } else if (web) {
                OBSWeb::QtDialogBridge bridge(current); QString error;
                check(bridge.execute("dialog.key", {{"key", "Escape"}}, error), "Dialog native Escape route");
            } else if (auto *dialog = qobject_cast<QDialog *>(current.data())) dialog->reject();
        }
        QTimer::singleShot(150, this, [this] {
            check(!current || !current->isVisible(), "Dialog closes without saving test changes");
            if (current && current->isVisible()) { finish(); return; }
            if (QByteArray(cases.at(caseIndex).action) == "@statsDock") {
                if (auto *dock = main->findChild<QDockWidget *>("statsDock")) {
                    dock->setFloating(statsWasFloating); dock->setVisible(statsWasVisible);
                }
            }
            next();
        });
    }
    void advance() {
        if (finished) return;
        restoreCaptureWindow();
        pending = false;
        if (!current) { check(false, "Dialog survived capture"); next(); return; }
        if (current->inherits("OBSBasicSettings")) {
            auto *list = current->findChild<QListWidget *>("listWidget");
            if (list && ++page < list->count()) {
                if (web) {
                    OBSWeb::QtDialogBridge bridge(current); QString error;
                    const auto listNode = node(bridge, "listWidget"); QJsonObject item;
                    for (const auto &value : listNode.value("items").toArray())
                        if (value.toObject().value("row").toInt(-1) == page) item = value.toObject();
                    check(!item.isEmpty() && bridge.execute("dialog.item", {{"id", listNode.value("id")},
                        {"item", item.value("id")}, {"action", "select"}}, error), "Settings category uses production bridge");
                } else list->setCurrentRow(page);
                scheduleCapture(400); return;
            }
        } else if (auto *wizard = qobject_cast<QWizard *>(current.data()); wizard && page < 2) {
            // Start -> Video -> Start, then Cancel. Never enter encoder tests or stream.
            auto *button = wizard->button(page++ == 0 ? QWizard::NextButton : QWizard::BackButton);
            const int before = wizard->currentId();
            if (web) {
                const auto originalName = button->objectName(); button->setObjectName("parityWizardButton");
                OBSWeb::QtDialogBridge bridge(current); QString error;
                const auto entry = node(bridge, "parityWizardButton");
                check(bridge.execute("dialog.click", {{"id", entry.value("id")}}, error), "Wizard navigation uses production bridge");
                button->setObjectName(originalName);
            } else button->click();
            check(wizard->currentId() != before, "Wizard moves between Start and Video pages");
            scheduleCapture(300); return;
        }
        closeCurrent();
    }
    static HWND ownCaptureHandle(QWidget *window) {
        if (!window || !window->isWindow()) return nullptr;
        // Never create/recreate an HWND while checking or restoring it. A
        // deferred Qt/WebView mount may have replaced the one raised earlier.
        const auto handle = reinterpret_cast<HWND>(window->internalWinId());
        DWORD process = 0;
        if (!handle || !IsWindow(handle) || !GetWindowThreadProcessId(handle, &process) ||
            process != GetCurrentProcessId() || QWidget::find(reinterpret_cast<WId>(handle)) != window)
            return nullptr;
        return handle;
    }
    static QJsonObject captureWindowInfo(HWND handle) {
        const auto id = [](HWND value) { return QString::number(reinterpret_cast<quintptr>(value), 16); };
        DWORD process = 0;
        GetWindowThreadProcessId(handle, &process);
        wchar_t className[256]{};
        const auto length = GetClassNameW(handle, className, int(std::size(className)));
        const auto root = GetAncestor(handle, GA_ROOT);
        RECT region{};
        const auto regionType = GetWindowRgnBox(handle, &region);
        const auto style = GetWindowLongPtr(handle, GWL_STYLE);
        const auto exStyle = GetWindowLongPtr(handle, GWL_EXSTYLE);
        QJsonObject result{{"hwnd", id(handle)}, {"pid", int(process)}, {"rootHwnd", id(root)},
            {"parentHwnd", id(GetParent(handle))}, {"windowClass", QString::fromWCharArray(className, length)},
            {"winVisible", bool(IsWindowVisible(handle))}, {"winEnabled", bool(IsWindowEnabled(handle))},
            {"iconic", bool(IsIconic(handle))}, {"style", QString::number(quintptr(style), 16)},
            {"exStyle", QString::number(quintptr(exStyle), 16)}, {"topmost", bool(exStyle & WS_EX_TOPMOST)},
            {"transparent", bool(exStyle & WS_EX_TRANSPARENT)}, {"layered", bool(exStyle & WS_EX_LAYERED)},
            {"aboveHwnd", id(GetWindow(handle, GW_HWNDPREV))}, {"belowHwnd", id(GetWindow(handle, GW_HWNDNEXT))},
            {"regionType", regionType}, {"regionBounds", QJsonObject{{"x", int(region.left)}, {"y", int(region.top)},
                {"width", int(region.right - region.left)}, {"height", int(region.bottom - region.top)}}}};
        if (process == GetCurrentProcessId()) {
            if (auto *widget = QWidget::find(reinterpret_cast<WId>(handle))) {
                result.insert("qtClass", widget->metaObject()->className());
                result.insert("qtObject", widget->objectName());
                result.insert("qtVisible", widget->isVisible());
                result.insert("qtEnabled", widget->isEnabled());
            }
            if (auto *widget = QWidget::find(reinterpret_cast<WId>(root))) {
                result.insert("rootQtClass", widget->metaObject()->className());
                result.insert("rootQtObject", widget->objectName());
            }
        }
        return result;
    }
    void restoreCaptureWindow() {
        if (const auto handle = ownCaptureHandle(raisedWindow))
            SetWindowPos(handle, raisedWasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        raisedWindow = nullptr; raisedHandle = nullptr;
    }
    void raiseCaptureWindow() {
        auto *window = current ? current->window() : nullptr;
        auto handle = ownCaptureHandle(window);
        if (!handle) return;
        if (raisedWindow != window) {
            restoreCaptureWindow();
            raisedWindow = window;
            raisedWasTopmost = bool(GetWindowLongPtr(handle, GWL_EXSTYLE) & WS_EX_TOPMOST);
        } else if (raisedHandle && raisedHandle != handle) {
            captureWindowChanged = true;
        }
        raisedHandle = handle;
        topmostBeforeActivation = bool(GetWindowLongPtr(handle, GWL_EXSTYLE) & WS_EX_TOPMOST);
        // Exercise the ordinary user-facing foreground path for this QA window.
        // Both original Qt and WebView tours can otherwise remain behind OBS.
        const QPointer<QWidget> guard(window);
        window->raise();
        if (!guard) return;
        guard->activateWindow();
        if (!guard || !(handle = ownCaptureHandle(guard))) return;
        if (raisedHandle != handle) captureWindowChanged = true;
        raisedHandle = handle;
        topmostAfterActivation = bool(GetWindowLongPtr(handle, GWL_EXSTYLE) & WS_EX_TOPMOST);
        SetLastError(ERROR_SUCCESS);
        lastRaiseSucceeded = SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        lastRaiseError = GetLastError();
        topmostAfterRaise = bool(GetWindowLongPtr(handle, GWL_EXSTYLE) & WS_EX_TOPMOST);
    }
    void scheduleCapture(int delay) {
        restoreCaptureWindow();
        const auto generation = ++captureGeneration;
        pending = true; composedWait.invalidate(); composedAttempts = 0; captureWindowChanged = false;
        lastRaiseSucceeded = false; lastRaiseError = ERROR_SUCCESS;
        topmostBeforeActivation = false; topmostAfterActivation = false; topmostAfterRaise = false;
        raiseCaptureWindow();
        QTimer::singleShot(delay, this, [this, generation] {
            if (!finished && pending && generation == captureGeneration) capture();
        });
    }
    void capture() {
        if (finished) return;
        if (!current) { check(false, "Requested dialog exists"); next(); return; }
        const auto generation = captureGeneration;
        if (!composedWait.isValid()) {
            composedWait.start();
            QTimer::singleShot(15000, this, [this, generation] {
                if (finished || generation != captureGeneration || !pending) return;
                check(false, "Dialog capture finished within its deadline");
                pending = false; closeCurrent();
            });
        }
        ++composedAttempts;
        const QString key = QString::fromLatin1(cases.at(caseIndex).action) + "-" + QString::number(page);
        // Capture only our visible QA window. Chromium CapturePreview below
        // deliberately excludes the native GPU and custom-control islands.
        bool composedSaved = false;
        const auto window = current->window();
        const auto handle = ownCaptureHandle(window);
        const auto origin = current->mapToGlobal(QPoint());
        bool ownsPixels = handle && current->isVisible() && current->screen() &&
            current->screen()->geometry().contains(QRect(origin, current->size()));
        QString composedFailure = !handle ? "No current owned HWND" : !current->isVisible() ? "Dialog is hidden" :
            !ownsPixels ? "Dialog is not fully inside its screen" : QString();
        const auto offset = current->mapTo(window, QPoint());
        const auto dpr = window->devicePixelRatioF();
        const auto rectangle = [](const QRect &rect) { return QJsonObject{{"x", rect.x()}, {"y", rect.y()},
            {"width", rect.width()}, {"height", rect.height()}}; };
        RECT client{}, outer{}; POINT clientOrigin{};
        const bool haveClient = handle && GetClientRect(handle, &client);
        const bool haveOuter = handle && GetWindowRect(handle, &outer);
        const bool haveOrigin = handle && ClientToScreen(handle, &clientOrigin);
        auto composedGeometry = captureWindowInfo(handle);
        composedGeometry.insert("clientRectValid", haveClient);
        composedGeometry.insert("clientRect", rectangle(QRect(client.left, client.top, client.right - client.left, client.bottom - client.top)));
        composedGeometry.insert("windowRectValid", haveOuter);
        composedGeometry.insert("windowRect", rectangle(QRect(outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top)));
        composedGeometry.insert("clientOriginValid", haveOrigin);
        composedGeometry.insert("clientOriginPhysical", QJsonObject{{"x", int(clientOrigin.x)}, {"y", int(clientOrigin.y)}});
        composedGeometry.insert("qtCaptureRect", rectangle(QRect(origin, current->size())));
        composedGeometry.insert("qtWindowRect", rectangle(QRect(window->mapToGlobal(QPoint()), window->size())));
        composedGeometry.insert("dpr", dpr);
        composedGeometry.insert("lastRaiseSucceeded", lastRaiseSucceeded);
        composedGeometry.insert("lastRaiseError", int(lastRaiseError));
        composedGeometry.insert("originalTopmost", raisedWasTopmost);
        composedGeometry.insert("topmostBeforeActivation", topmostBeforeActivation);
        composedGeometry.insert("topmostAfterActivation", topmostAfterActivation);
        composedGeometry.insert("topmostAfterRaise", topmostAfterRaise);
        const auto activeWidget = [](QWidget *widget) {
            return widget ? QJsonObject{{"class", widget->metaObject()->className()}, {"object", widget->objectName()},
                {"visible", widget->isVisible()}, {"enabled", widget->isEnabled()}} : QJsonObject();
        };
        composedGeometry.insert("activeModalWidget", activeWidget(QApplication::activeModalWidget()));
        composedGeometry.insert("activePopupWidget", activeWidget(QApplication::activePopupWidget()));
        QJsonArray composedFailedPoints;
        if (ownsPixels) for (QPoint point : {QPoint(8, 8), QPoint(current->width() - 8, 8),
                QPoint(8, current->height() - 8), QPoint(current->width() - 8, current->height() - 8),
                current->rect().center()}) {
            point += offset;
            POINT physical{LONG(qRound(point.x() * dpr)), LONG(qRound(point.y() * dpr))};
            if (!ClientToScreen(handle, &physical)) {
                ownsPixels = false; composedFailure = "Current HWND coordinate mapping failed"; break;
            }
            const auto top = WindowFromPoint(physical);
            const bool owned = top == handle || IsChild(handle, top);
            ownsPixels &= owned;
            if (!owned) {
                auto hit = captureWindowInfo(top);
                hit.insert("physical", QJsonObject{{"x", int(physical.x)}, {"y", int(physical.y)}});
                hit.insert("qtWindowPoint", QJsonObject{{"x", point.x()}, {"y", point.y()}});
                composedFailedPoints.append(hit);
            }
        }
        if (!ownsPixels) {
            if (composedFailure.isEmpty()) composedFailure = "A sampled pixel is outside the owned HWND tree";
            const auto remaining = 2000 - composedWait.elapsed();
            if (remaining > 0) {
                // Retry only this QA window. Do not weaken the screen/ancestry
                // guard or capture pixels from a window covering the dialog.
                raiseCaptureWindow();
                QTimer::singleShot(int(qMin<qint64>(100, remaining)), this, [this, generation] {
                    if (!finished && pending && generation == captureGeneration) capture();
                });
                return;
            }
        }
        if (ownsPixels) {
            const auto screenOrigin = origin - current->screen()->geometry().topLeft();
            composedSaved = current->screen()->grabWindow(0, screenOrigin.x(), screenOrigin.y(), current->width(), current->height())
                .save(QDir(directory).filePath(key + "-composed.png"));
        }
        if (ownsPixels && !composedSaved) composedFailure = "Owned screen capture could not be saved";
        OBSWeb::QtDialogBridge bridge(current);
        const auto snapshot = bridge.snapshot();
        auto *surface = current->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
        QJsonObject types;
        QJsonArray controls;
        for (const auto &value : snapshot.value("nodes").toArray()) {
            const auto n = value.toObject(); const auto type = n.value("type").toString();
            types.insert(type, types.value(type).toInt() + 1);
            controls.append(QJsonObject{{"name", n.value("name")}, {"class", n.value("class")},
                {"type", type}, {"enabled", n.value("enabled")}, {"text", n.value("text")}});
        }
        dialogs.append(QJsonObject{{"case", key}, {"class", current->metaObject()->className()},
            {"nodes", controls}, {"types", types}, {"firstDetectedMs", firstDetectedMs},
            {"surfaceReadyMs", web ? QJsonValue(surfaceReadyMs) : QJsonValue(QJsonValue::Null)},
            {"timingPollIntervalMs", poll.interval()}, {"composedCapture", composedSaved},
            {"composedAttempts", composedAttempts}, {"composedWaitMs", composedWait.elapsed()},
            {"composedWindowChanged", captureWindowChanged}, {"composedFailure", composedFailure},
            {"composedGeometry", composedGeometry}, {"composedFailedPoints", composedFailedPoints}});
        save(key + ".json", snapshot);
        check(!controls.isEmpty(), "Visible control inventory: " + key);
        check(web ? surface != nullptr : surface == nullptr, "Expected frontend owns dialog: " + key);
        if (web && surface) {
            const QPointer<FrontendParityAudit> guard(this);
            surface->capturePreview(QDir(directory).filePath(key + ".png"), [guard, key, generation](bool ok) {
                if (!guard || guard->finished || guard->captureGeneration != generation || !guard->pending) return;
                guard->check(ok, "Actual Chromium capture: " + key); guard->advance();
            });
        } else {
            check(current->grab().save(QDir(directory).filePath(key + ".png")), "Actual Qt capture: " + key);
            advance();
        }
    }
public:
    FrontendParityAudit(OBSBasic *window, bool isWeb) : QObject(window), main(window), web(isWeb),
        directory(qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS")) {
        poll.setInterval(100);
        connect(&poll, &QTimer::timeout, this, [this] {
            if (pending) return;
            if (QByteArray(cases.at(caseIndex).action) == "@statsDock") {
                auto *dock = main->findChild<QDockWidget *>("statsDock");
                if (dock && dock->isVisible()) current = dock->widget();
            }
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible() &&
                    dialog->inherits(cases.at(caseIndex).type)) { current = dialog; break; }
            if (current) {
                if (firstDetectedMs < 0) firstDetectedMs = wait.elapsed();
                auto *surface = current->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
                if (!web || (surface && surface->isVisible() && surface->property("webview2Presented").toBool() &&
                             !current->property("webview2Opening").toBool())) {
                    if (web) surfaceReadyMs = wait.elapsed();
                    poll.stop(); pending = true;
                    // Measured milestones above exclude this visual stabilization delay.
                    scheduleCapture(250);
                } else if (wait.elapsed() > 15000) {
                    poll.stop(); check(false, "Dialog's WebView surface becomes ready within its deadline"); closeCurrent();
                }
            } else if (wait.elapsed() > 15000) {
                poll.stop(); check(false, "Timed out opening " + QString::fromLatin1(cases.at(caseIndex).type));
                for (auto *widget : QApplication::topLevelWidgets())
                    if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible()) dialog->reject();
                QTimer::singleShot(200, this, [this] { next(); });
            }
        });
    }
    ~FrontendParityAudit() override { restoreCaptureWindow(); }
    void run() {
        QDir().mkpath(directory);
        auto *config = obs_frontend_get_user_config();
        hadPrompt = config_has_user_value(config, "General", "AutoSearchPrompt");
        hadSearch = config_has_user_value(config, "General", "AutomaticCollectionSearch");
        oldPrompt = config_get_bool(config, "General", "AutoSearchPrompt");
        oldSearch = config_get_bool(config, "General", "AutomaticCollectionSearch");
        // The tour uses an explicit fixture and does not search unrelated applications.
        config_set_bool(config, "General", "AutoSearchPrompt", true);
        config_set_bool(config, "General", "AutomaticCollectionSearch", false);
        check(!obs_frontend_recording_active() && !obs_frontend_streaming_active() && !obs_frontend_replay_buffer_active(),
            "Audit has no active outputs");
        fixtureScene = obs_scene_create("Parity audit scene");
        fixtureSource = obs_source_create("color_source_v3", "Parity audit color", nullptr, nullptr);
        if (fixtureScene && fixtureSource) {
            const auto item = obs_scene_add(fixtureScene, fixtureSource);
            obs_frontend_set_current_scene(obs_scene_get_source(fixtureScene));
            if (auto *tree = main->findChild<SourceTree *>("sources")) tree->SelectItem(item, true);
        }
        for (auto *action : main->findChildren<QAction *>())
            if (!action->objectName().isEmpty() && !action->property("webview2Entry").toBool())
                actions.append(QJsonObject{{"name", action->objectName()}, {"text", action->text()},
                    {"visible", action->isVisible()}, {"enabled", action->isEnabled()}, {"checked", action->isChecked()}});
        if (!web) {
            const auto rectangle = [](const QRect &rect) { return QJsonObject{{"x", rect.x()}, {"y", rect.y()},
                {"width", rect.width()}, {"height", rect.height()}}; };
            QJsonObject evidence{{"frontend", "native-qt"}, {"mainRect", rectangle(main->rect())},
                {"centralRect", rectangle(main->centralWidget()->geometry())}};
            if (auto *preview = main->findChild<OBSBasicPreview *>("preview"))
                evidence.insert("preview", QJsonObject{{"rect", rectangle(preview->geometry())},
                    {"visible", preview->isVisible()}, {"fixedScaling", preview->IsFixedScaling()},
                    {"scale", preview->GetScalingAmount()}});
            for (const auto *name : {"previewXScrollBar", "previewYScrollBar"})
                if (auto *bar = main->findChild<QScrollBar *>(QString::fromLatin1(name)))
                    evidence.insert(QLatin1String(name), QJsonObject{{"visible", bar->isVisible()}, {"hidden", bar->isHidden()},
                        {"rect", rectangle(bar->geometry())}, {"minimum", bar->minimum()}, {"maximum", bar->maximum()},
                        {"pageStep", bar->pageStep()}});
            save("main-preview.json", evidence);
            check(main->grab().save(QDir(directory).filePath("main-native.png")), "Original Qt main window captured for preview geometry baseline");
        }
        next();
    }
};
static void RunFrontendParityAudit(OBSBasic *window, bool web) {
    (new FrontendParityAudit(window, web))->run();
}
} // namespace
