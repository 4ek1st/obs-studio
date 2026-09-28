// Member of OBSWebView2; only compiled for the disposable portable test build.
void traceWorkspacePreview(const char *phase) const
{
    auto *display = preview->GetDisplay();
    auto *handle = preview->windowHandle();
    auto *parent = preview->parentWidget();
    auto *window = preview->window();
    const auto geometry = preview->geometry();
    blog(LOG_INFO, "[WebView2 preview trace] %s visible=%d hidden=%d display=%p enabled=%d exposed=%d parentVisible=%d windowVisible=%d mainMinimized=%d geometry=%d,%d,%d,%d",
        phase, preview->isVisible(), preview->isHidden(), static_cast<void *>(display),
        display && obs_display_enabled(display), handle && handle->isExposed(),
        parent && parent->isVisible(), window && window->isVisible(), main->isMinimized(),
        geometry.x(), geometry.y(), geometry.width(), geometry.height());
}

void runOverlayAndFloatingDockChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    const QRect video = preview->geometry().translated(-browser->pos());
    const QRect menu(video.topLeft() + QPoint(12, 12), QSize(70, 50));
    QJsonObject bounds{{"target", "preview"}, {"x", video.x()}, {"y", video.y()},
        {"width", video.width()}, {"height", video.height()}, {"viewportWidth", browser->width()},
        {"viewportHeight", browser->height()}, {"visible", true},
        {"overlays", QJsonArray{QJsonObject{{"x", menu.x()}, {"y", menu.y()},
            {"width", menu.width()}, {"height", menu.height()}}}}};
    auto *display = preview->GetDisplay();
    traceWorkspacePreview("before overlay bounds");
    execute({{"id", "overlay-live-test"}, {"command", "preview.bounds"}, {"args", bounds}});
    traceWorkspacePreview("after overlay bounds");
    check(lastTestReply.value("ok").toBool() && preview->isVisible() && display &&
          preview->GetDisplay() == display && obs_display_enabled(display),
          "HTML menu bounds preserve the visible enabled native GPU display and its identity");
    check(browser->mask().contains(menu.center()) && !browser->mask().contains(video.bottomRight() - QPoint(12, 12)),
          "Browser mask covers the menu overlap while leaving uncovered GPU video visible");
    bounds.insert("overlays", QJsonArray{});
    execute({{"id", "overlay-close-test"}, {"command", "preview.bounds"}, {"args", bounds}});
    check(!browser->mask().contains(menu.center()) && preview->GetDisplay() == display,
          "Closing the HTML menu restores its native video aperture without replacing the display");

    auto *dock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    check(dock && lock, "Native Controls dock and dock lock action are available");
    if (!dock || !lock) { done(); return; }
    const bool wasLocked = lock->isChecked();
    const bool wasFloating = dock->isFloating();
    const bool wasHidden = dock->isHidden();
    const QByteArray originalDockLayout = main->saveState();
    const QPointer<QWidget> nativeContent = dock->widget();
    check(property("webview2NativeDocking").toBool() && main->isVisible() && main->centralWidget() == this,
          "The original OBS QMainWindow owns the visible workspace and native docking layout");
    auto *scenesDock = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
    check(scenesDock && !scenesDock->isFloating() && webDockViews.value(scenesDock) &&
          webDockViews.value(scenesDock)->parentWidget() == scenesDock->widget(),
          "An attached core panel uses its own WebView inside the original native dock content");
    lock->setChecked(false);
    if (dock->isFloating()) execute({{"id", "dock-preparation-test"}, {"command", "dock.attach"},
        {"args", QJsonObject{{"name", "controlsDock"}}}});
    lock->setChecked(true);
    auto *toggle = dock->titleBarWidget() ? dock->titleBarWidget()->findChild<QToolButton *>(QStringLiteral("obsWebView2DockToggle")) : nullptr;
    check(toggle && !toggle->isEnabled(), "Native dock lock disables the custom Qt float and attach control");
    execute({{"id", "dock-lock-test"}, {"command", "dock.detach"}, {"args", QJsonObject{{"name", "controlsDock"}}}});
    check(!lastTestReply.value("ok").toBool() && !dock->isFloating(),
          "Locked workspace rejects a core panel detach without changing its native dock state");
    lock->setChecked(false);
    execute({{"id", "dock-detach-test"}, {"command", "dock.detach"},
        {"args", QJsonObject{{"name", "controlsDock"}, {"width", 360}, {"height", 420}}}});
    const QPointer<WebView2Widget> surface = webDockViews.value(dock);
    check(lastTestReply.value("ok").toBool() && dock->isFloating() && dock->isVisible() && surface &&
          dock->widget() == nativeContent, "Detaching Controls creates a WebView while preserving its native controller widget");
    check(dock->titleBarWidget() && dock->titleBarWidget()->objectName() == QStringLiteral("obsWebView2DockTitleBar") &&
          dock->windowFlags().testFlag(Qt::FramelessWindowHint),
          "A floating WebView dock has one native Qt drag title bar and no duplicate OS caption");
    auto completed = std::make_shared<bool>(false);
    auto finish = [this, dock = QPointer<QDockWidget>(dock), lock = QPointer<QAction>(lock),
                   wasLocked, wasFloating, wasHidden, originalDockLayout, completed, done] {
        if (*completed) return;
        *completed = true;
        if (lock) lock->setChecked(false);
        if (dock) {
            execute({{"id", "dock-cleanup-test"}, {"command", wasFloating ? "dock.detach" : "dock.attach"},
                {"args", QJsonObject{{"name", "controlsDock"}}}});
            if (wasHidden) dock->hide();
        }
        if (lock) lock->setChecked(wasLocked);
        main->restoreState(originalDockLayout);
        browser->postMessage({{"version", 1}, {"event", "viewport.invalidate"}, {"data", QJsonObject{}}});
        done();
    };
    if (!surface) { finish(); return; }
    QTimer::singleShot(15000, this, [check, completed, finish] {
        if (!*completed) { check(false, "Floating Controls WebView loads before the timeout"); finish(); }
    });
    auto started = std::make_shared<bool>(false);
    auto loaded = [this, surface, dock = QPointer<QDockWidget>(dock), nativeContent, check, completed, started, finish] {
        if (*completed || *started) return;
        *started = true;
        check(surface && dock && surface->isVisible(), "Floating Controls loads the real WebView document");
        QTimer::singleShot(300, this, [this, surface, dock, nativeContent, check, completed, finish] {
            if (*completed) return;
            if (!surface || !dock) { check(false, "Floating Controls survives document initialization"); finish(); return; }
            const auto children = dock->widget()->findChildren<WebView2Widget *>(QString(), Qt::FindDirectChildrenOnly);
            check(children.size() == 1 && children.first() == surface &&
                  surface->objectName() == QStringLiteral("obsWebView2DockSurface"),
                  "Core floating panel has exactly one workspace WebView and no generic dialog overlay");
            const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
            const auto png = artifacts.isEmpty() ? QDir::temp().filePath(QStringLiteral("obs-webview2-floating-%1.png").arg(QCoreApplication::applicationPid())) :
                QDir(artifacts).filePath(QStringLiteral("floating-controls.png"));
            surface->capturePreview(png, [this, surface, dock, nativeContent, check, completed, finish, png, artifacts](bool saved) {
                if (*completed) return;
                const QImage pixels(png);
                bool varied = false;
                if (!pixels.isNull()) {
                    const auto first = pixels.pixel(0, 0);
                    for (int y = 0; y < pixels.height() && !varied; y += 3)
                        for (int x = 0; x < pixels.width() && !varied; x += 3) varied |= pixels.pixel(x, y) != first;
                }
                check(saved && surface && !pixels.isNull() && pixels.width() > 100 && pixels.height() > 100 && varied &&
                      qAbs(pixels.width() - qRound(surface->width() * surface->devicePixelRatioF())) <= 2 &&
                      qAbs(pixels.height() - qRound(surface->height() * surface->devicePixelRatioF())) <= 2,
                      "Floating Controls renders a populated Chromium image at its real surface size");
                if (artifacts.isEmpty()) QFile::remove(png);
                if (!surface || !dock) { finish(); return; }
                const auto geometry = preview->geometry();
                surface->messageReceived({{"id", "floating-bounds-test"}, {"command", "preview.bounds"},
                    {"args", QJsonObject{{"visible", false}}}});
                check(preview->geometry() == geometry && preview->isVisible(),
                      "Floating document cannot move or hide the main native preview");
                const bool studio = obs_frontend_preview_program_mode_active();
                surface->messageReceived({{"id", "floating-control-test"}, {"command", "control.click"},
                    {"args", QJsonObject{{"id", "modeSwitch"}, {"context", snapshot().value("context")}}}});
                check(lastTestReply.value("ok").toBool() && obs_frontend_preview_program_mode_active() != studio,
                      "Floating Controls command route executes the original Studio Mode button");
                obs_frontend_set_preview_program_mode(studio);
                traceWorkspacePreview("floating controls before native restore");
                restoreSurfaces();
                check(surface->isHidden() && dock->widget() == nativeContent && !dock->titleBarWidget() &&
                      !dock->windowFlags().testFlag(Qt::FramelessWindowHint),
                      "Native fallback restores the original dock content and system caption");
                resumeFrontend();
                traceWorkspacePreview("floating controls after frontend resume");
                check(surface->isVisible() && dock->isFloating(), "Resuming WebView restores the same floating panel surface");
                // Deliver mouse gestures through the real Qt title bar. The
                // test never moves the user's cursor or sends system input.
                auto mouse = [](QWidget *target, QEvent::Type type, QPoint global, Qt::MouseButton button, Qt::MouseButtons buttons) {
                    const QPointF local = target->mapFromGlobal(global);
                    QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
                    QApplication::sendEvent(target, &event);
                };
                auto *titleBar = dock->titleBarWidget();
                if (titleBar) {
                    QPoint grip = titleBar->mapToGlobal(QPoint(20, titleBar->height() / 2));
                    mouse(titleBar, QEvent::MouseButtonDblClick, grip, Qt::LeftButton, Qt::LeftButton);
                    check(!dock->isFloating() && webDockViews.value(dock) == surface,
                          "Double clicking the Qt title bar redocks the same live WebView through the original QDockWidget handler");
                    grip = titleBar->mapToGlobal(QPoint(20, titleBar->height() / 2));
                    mouse(titleBar, QEvent::MouseButtonDblClick, grip, Qt::LeftButton, Qt::LeftButton);
                    check(dock->isFloating(), "Double clicking the same Qt title bar detaches its original dock again");
                    grip = titleBar->mapToGlobal(QPoint(20, titleBar->height() / 2));
                    const auto beforeMove = dock->pos();
                    mouse(titleBar, QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton);
                    mouse(titleBar, QEvent::MouseMove, grip + QPoint(80, 50), Qt::NoButton, Qt::LeftButton);
                    mouse(titleBar, QEvent::MouseMove, grip + QPoint(120, 70), Qt::NoButton, Qt::LeftButton);
                    check(dock->pos() != beforeMove, "Dragging the Qt title bar moves the actual native floating panel");
                    const QPoint target = main->mapToGlobal(QPoint(6, main->height() / 2));
                    mouse(titleBar, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
                    auto *indicator = main->findChild<QWidget *>(QStringLiteral("qt_rubberband"));
                    blog(LOG_INFO, "[WebView2 dock trace] before drop target=%d,%d indicator=%d dockFloating=%d mainVisible=%d mainSize=%dx%d dockOptions=%u",
                        target.x(), target.y(), indicator && indicator->isVisible(), dock->isFloating(), main->isVisible(),
                        main->width(), main->height(), unsigned(main->dockOptions()));
                    check(indicator && indicator->isVisible(),
                          "Native QMainWindow displays its accepted drop target while the floating WebView dock is dragged");
                    mouse(titleBar, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
                } else check(false, "Core WebView dock exposes its native Qt drag title bar");
                // Qt's zero-duration dock animation finishes through a deferred
                // QPropertyAnimation destruction callback. Let the event loop
                // perform the actual native plug before observing or moving it.
                QTimer::singleShot(100, this, [this, surface, dock, nativeContent, check, completed, finish] {
                if (*completed) return;
                if (!surface || !dock) { check(false, "Dock survives native drop completion"); finish(); return; }
                blog(LOG_INFO, "[WebView2 dock trace] after drop floating=%d nativeParent=%d sameSurface=%d",
                    dock->isFloating(), dock->parentWidget() == main, webDockViews.value(dock) == surface);
                check(!dock->isFloating() && dock->parentWidget() == main && webDockViews.value(dock) == surface,
                      "Dragging a floating title bar into the original QMainWindow drop area redocks its live WebView");
                dock->setFloating(true);
                surface->messageReceived({{"id", "floating-attach-test"}, {"command", "dock.attach"},
                    {"args", QJsonObject{{"name", "controlsDock"}}}});
                check(lastTestReply.value("ok").toBool() && !dock->isFloating() && !surface->isHidden() && dock->widget() == nativeContent,
                      "Redocking keeps the same live WebView and native controller content inside the main workspace");
                main->addDockWidget(Qt::LeftDockWidgetArea, dock);
                check(main->dockWidgetArea(dock) == Qt::LeftDockWidgetArea && dock->widget() == nativeContent,
                      "WebView dock can occupy the original QMainWindow left dock area");
                auto *scenes = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
                if (scenes) {
                    main->splitDockWidget(scenes, dock, Qt::Vertical);
                    check(main->dockWidgetArea(dock) == main->dockWidgetArea(scenes) && !dock->isFloating(),
                          "Original QMainWindow accepts a vertical split containing the WebView dock");
                    main->tabifyDockWidget(scenes, dock);
                    check(main->tabifiedDockWidgets(scenes).contains(dock),
                          "Original QMainWindow tabifies WebView docks using its native layout");
                    const auto tabbedLayout = main->saveState();
                    main->addDockWidget(Qt::RightDockWidgetArea, dock);
                    check(main->restoreState(tabbedLayout) && main->tabifiedDockWidgets(scenes).contains(dock),
                          "Native saveState and restoreState retain WebView dock tabs and placement");
                } else check(false, "Native Scenes dock exists for split and tab checks");
                const QPointer<QObject> resizeFilter = surface->findChild<QObject *>(QStringLiteral("obsWebView2DockResizeFilter"));
                check(resizeFilter, "Floating resize filter belongs to its browser surface");
                destroyWebDocks();
                check(surface.isNull() && resizeFilter.isNull() && webDockViews.isEmpty() && dock->widget() == nativeContent,
                      "Frontend disposal destroys floating browser controllers and filters while retaining native content");
                execute({{"id", "dock-recreate-test"}, {"command", "dock.detach"},
                    {"args", QJsonObject{{"name", "controlsDock"}}}});
                const auto recreated = dock->widget()->findChildren<WebView2Widget *>(QString(), Qt::FindDirectChildrenOnly);
                check(lastTestReply.value("ok").toBool() && recreated.size() == 1 && webDockViews.value(dock) == recreated.first(),
                      "Creating a panel after frontend disposal produces exactly one fresh browser host");
                finish();
                });
            });
        });
    };
    connect(surface, &WebView2Widget::ready, this, loaded);
    // A restored floating panel may have completed navigation before this test.
    if (surface->property("webview2Ready").toBool()) QTimer::singleShot(300, this, loaded);
}

void runWorkspaceChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    done = [this, check, complete = std::move(done)] { runOverlayAndFloatingDockChecks(check, complete); };
    struct WorkspaceFixture {
        OBSSceneAutoRelease scene{obs_scene_create("WebView Workspace Editing")};
        OBSSourceAutoRelease a{obs_source_create("color_source_v3", "Workspace red", nullptr, nullptr)};
        OBSSourceAutoRelease b{obs_source_create("color_source_v3", "Workspace green", nullptr, nullptr)};
        OBSSourceAutoRelease c{obs_source_create("color_source_v3", "Workspace blue", nullptr, nullptr)};
    };
    auto fixture = std::make_shared<WorkspaceFixture>();
    obs_scene_add(fixture->scene, fixture->a);
    obs_scene_add(fixture->scene, fixture->b);
    obs_scene_add(fixture->scene, fixture->c);
    obs_frontend_set_current_scene(obs_scene_get_source(fixture->scene));
    QTimer::singleShot(250, this, [this, fixture, check, done] {
        auto send = [this](const char *command, QJsonObject args) {
            args.insert("context", snapshot().value("context"));
            execute(QJsonObject{{"id", "workspace-test"}, {"command", command}, {"args", args}});
            return lastTestReply.value("ok").toBool();
        };
        auto rowFor = [this](obs_source_t *source) {
            for (const auto &value : snapshot().value("sources").toArray()) {
                auto row = value.toObject();
                if (row.value("uuid") == SourceId(source)) {
                    row.insert("scene", snapshot().value("currentScene"));
                    return row;
                }
            }
            return QJsonObject{};
        };
        auto a = rowFor(fixture->a), b = rowFor(fixture->b), c = rowFor(fixture->c);
        a.insert("value", false);
        check(send("source.visibility", a) && !obs_sceneitem_visible(obs_scene_find_source(fixture->scene, obs_source_get_name(fixture->a))),
              "Source eye control changes actual scene visibility");
        static_cast<OBSBasic *>(main)->undo_s.undo();
        check(obs_sceneitem_visible(obs_scene_find_source(fixture->scene, obs_source_get_name(fixture->a))),
              "Visibility edit participates in native Undo");
        a.insert("value", true);
        check(send("source.lock", a) && obs_sceneitem_locked(obs_scene_find_source(fixture->scene, obs_source_get_name(fixture->a))),
              "Source lock changes the actual editable item");
        a.insert("value", false); send("source.lock", a);
        const QString renamed = QStringLiteral("Workspace renamed green ") + SourceId(fixture->b).left(8);
        b.insert("name", renamed);
        check(send("source.rename", b) && QString::fromUtf8(obs_source_get_name(fixture->b)) == renamed,
              "Source rename uses native validation and source name");
        send("source.select", a);
        c.insert("additive", true); send("source.select", c);
        int selected = 0;
        for (const auto &row : snapshot().value("sources").toArray()) selected += row.toObject().value("selected").toBool();
        check(selected == 2, "WebView2 supports native multiple source selection");
        a.insert("target", b); a.insert("position", "before");
        check(send("source.move", a), "Multiple selected sources can move through the native reorder controller");
        const auto rows = snapshot().value("sources").toArray();
        int first = -1, last = -1, target = -1;
        for (int i = 0; i < rows.size(); ++i) {
            const auto uuid = rows[i].toObject().value("uuid").toString();
            if (uuid == SourceId(fixture->a)) first = i;
            if (uuid == SourceId(fixture->c)) last = i;
            if (uuid == SourceId(fixture->b)) target = i;
        }
        check(first >= 0 && last >= 0 && std::abs(first - last) == 1 && target > first && target > last,
              "Drag reorder preserves both selected sources as an adjacent block");
        check(send("native.command", QJsonObject{{"id", "source.group"}}), "Group selected sources command is accepted");
        QTimer::singleShot(150, this, [this, fixture, check, done] {
            auto *tree = main->findChild<SourceTree *>("sources");
            bool editing = false, grouped = false;
            for (int row = 0; row < tree->model()->rowCount(); ++row) {
                if (auto *widget = tree->GetItemWidget(row)) editing |= widget->IsEditing();
                grouped |= obs_sceneitem_is_group(tree->Get(row));
            }
            check(grouped && !editing, "Grouping commits its queued native name editor");
            auto *basic = static_cast<OBSBasic *>(main);
            bool undoWorked = false;
            basic->undo_s.add_action("WebView grouping undo balance", [&undoWorked](const std::string &) { undoWorked = true; },
                                    [](const std::string &) {}, "", "");
            basic->undo_s.undo();
            check(undoWorked, "Grouping leaves the native undo stack enabled");
            basic->undo_s.clear();
            auto *stats = main->findChild<QDockWidget *>("statsDock");
            routeDocks();
            if (stats) {
                const bool floating = stats->isFloating();
                const auto dockArea = main->dockWidgetArea(stats);
                if (stats->toggleViewAction()->isChecked()) stats->toggleViewAction()->trigger();
                stats->toggleViewAction()->trigger();
                check(stats->isVisible() && stats->isFloating() == floating && main->dockWidgetArea(stats) == dockArea,
                      "Additional native dock opens from WebView2 without changing its original dock area or floating state");
                stats->hide();
            } else check(false, "Stats dock is available");
            const auto home = previewPlacement.parent;
            const auto homeLayout = previewPlacement.layout;
            const auto row = previewPlacement.row, column = previewPlacement.column;
            traceWorkspacePreview("workspace before native restore");
            restoreSurfaces();
            int restoredRow = -1, restoredColumn = -1, rowSpan = 0, columnSpan = 0;
            auto *grid = qobject_cast<QGridLayout *>(homeLayout.data());
            if (grid && grid->indexOf(preview) >= 0) grid->getItemPosition(grid->indexOf(preview), &restoredRow, &restoredColumn, &rowSpan, &columnSpan);
            check(preview->parentWidget() == home && !preview->isHidden() && restoredRow == row && restoredColumn == column,
                  "Switching to the original UI restores preview visibility and exact nested grid position");
            const QRect nativeGeometry = preview->geometry();
            execute(QJsonObject{{"id", "restored-preview-test"}, {"command", "preview.bounds"},
                {"args", QJsonObject{{"x", 1}, {"y", 1}, {"width", 10}, {"height", 10},
                    {"viewportWidth", 100}, {"viewportHeight", 100}, {"visible", false}}}});
            check(preview->geometry() == nativeGeometry && !preview->isHidden(),
                  "Late WebView2 bounds cannot move or hide a restored native preview");
            resumeFrontend();
            traceWorkspacePreview("workspace after frontend resume");
            check(preview->parentWidget() == this && surfacesBorrowed, "Returning to WebView2 reuses the same native editing surface");
            obs_frontend_set_preview_program_mode(true);
            syncProgramSurface();
            publishState(true);
            check(programPreview && programPreview->parentWidget() == this && snapshot().value("addQuickTransition").isString(),
                  "Studio Mode exposes actual program display and quick-transition creation");
            const auto programHome = programPlacement.parent;
            const auto programLayout = programPlacement.layout;
            resumeFrontend();
            check(programPlacement.parent == programHome && programPlacement.layout == programLayout,
                  "Focusing an existing WebView2 window preserves the saved program display placement");
            QTimer::singleShot(300, this, [this, check, done] {
                traceWorkspacePreview("workspace delayed studio check");
                check(programPreview && programPreview->isVisible() && programPreview->width() > 100 && preview->width() > 100,
                      "Actual WebView2 Studio Mode lays out both native video displays");
                OBSSceneAutoRelease quickScene{obs_scene_create("WebView quick transition destination")};
                const auto destination = SourceId(obs_scene_get_source(quickScene));
                obs_frontend_set_current_preview_scene(obs_scene_get_source(quickScene));
                const auto quick = snapshot().value("quickTransitions").toArray();
                check(!quick.isEmpty(), "Native quick transition buttons are available");
                if (!quick.isEmpty()) execute(QJsonObject{{"id", "quick-transition-test"}, {"command", "control.click"},
                    {"args", QJsonObject{{"id", quick.first().toObject().value("id")}, {"context", snapshot().value("context")}}}});
                QTimer::singleShot(650, this, [this, check, done, destination] {
                check(SourceId(static_cast<OBSBasic *>(main)->GetProgramSource()) == destination && !QApplication::activePopupWidget(),
                      "Quick transition executes the native program change without opening its menu");
                obs_frontend_set_preview_program_mode(false);
                publishState(true);
                check(programPreview.isNull(), "Leaving Studio Mode releases its borrowed program display");
                const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
                if (!artifacts.isEmpty()) {
                    QDir().mkpath(artifacts);
                    const auto previousGeometry = main->geometry();
                    const auto ownWindow = reinterpret_cast<HWND>(main->winId());
                    const bool wasTopmost = (GetWindowLongPtrW(ownWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                    if (auto *screen = main->screen()) {
                        const auto available = screen->availableGeometry().adjusted(8, 32, -8, -8);
                        QRect visible(QPoint(), previousGeometry.size().boundedTo(available.size()));
                        visible.moveCenter(available.center());
                        main->setGeometry(visible);
                    }
                    main->raise();
                    SetWindowPos(ownWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    QTimer::singleShot(250, this, [this, artifacts, check, done, previousGeometry, wasTopmost] {
                        const auto ownWindow = reinterpret_cast<HWND>(main->winId());
                        RECT client{};
                        bool ownsPixels = main->isVisible() && main->centralWidget() == this &&
                            main->property("webview2NativeDocking").toBool() && GetClientRect(ownWindow, &client);
                        for (POINT point : {POINT{8, 8}, POINT{client.right - 8, 8},
                             POINT{8, client.bottom - 8}, POINT{client.right - 8, client.bottom - 8},
                             POINT{client.right / 2, client.bottom / 2}}) {
                            ClientToScreen(ownWindow, &point);
                            const auto topWindow = WindowFromPoint(point);
                            ownsPixels &= topWindow == ownWindow || IsChild(ownWindow, topWindow);
                        }
                        bool composedSaved = false;
                        if (ownsPixels && main->screen()) {
                            const auto origin = main->mapToGlobal(QPoint());
                            composedSaved = main->screen()->grabWindow(0, origin.x(), origin.y(), main->width(), main->height())
                                .save(QDir(artifacts).filePath("workspace-composed.png"));
                        }
                        check(ownsPixels && composedSaved,
                              "Owned QA main window composed pixels include the native dock layout and GPU preview");
                        browser->capturePreview(QDir(artifacts).filePath("workspace.png"),
                            [main = QPointer<QMainWindow>(main), previousGeometry, wasTopmost, check, done](bool saved) {
                            if (main) {
                                SetWindowPos(reinterpret_cast<HWND>(main->winId()), wasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                                    0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                                main->setGeometry(previousGeometry);
                            }
                            check(saved, "Actual main WebView2 interface rendering is captured successfully");
                            done();
                        });
                    });
                } else QTimer::singleShot(350, this, std::move(done));
                });
            });
        });
    });
}
