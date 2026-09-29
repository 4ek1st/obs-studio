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

void runDockTabsChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    auto *scenes = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
    auto *controls = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    check(scenes && controls && lock, "Native Scenes, Controls and dock lock controls exist");
    if (!scenes || !controls || !lock) { done(); return; }
    const auto layout = main->saveState();
    const bool wasLocked = lock->isChecked();
    lock->setChecked(false);
    main->showNormal();
    main->resize(1280, 840);
    main->addDockWidget(Qt::LeftDockWidgetArea, scenes);
    main->addDockWidget(Qt::RightDockWidgetArea, controls);
    scenes->show();
    controls->show();
    main->tabifyDockWidget(scenes, controls);
    controls->raise();
    QTimer::singleShot(200, this, [this, scenes = QPointer<QDockWidget>(scenes),
        controls = QPointer<QDockWidget>(controls), lock = QPointer<QAction>(lock),
        layout, wasLocked, check, done] {
        if (!scenes || !controls) { check(false, "Tab test docks survive tabification"); done(); return; }
        auto findTabs = [this, scenes, controls]() -> QTabBar * {
            for (auto *bar : main->findChildren<QTabBar *>()) {
                bool hasScenes = false, hasControls = false;
                for (int i = 0; i < bar->count(); ++i) {
                    hasScenes |= bar->tabText(i) == scenes->windowTitle();
                    hasControls |= bar->tabText(i) == controls->windowTitle();
                }
                if (hasScenes && hasControls) return bar;
            }
            return nullptr;
        };
        auto *tabs = findTabs();
        check(main->tabifiedDockWidgets(scenes).contains(controls) && tabs && tabs->isVisible() && tabs->count() >= 2,
              "Combining two panels displays both named native tabs");
        check(main->tabPosition(main->dockWidgetArea(scenes)) == QTabWidget::North && tabs &&
              tabs->mapToGlobal(QPoint(0, 0)).y() < controls->mapToGlobal(QPoint(0, 0)).y(),
              "Combined panel names appear in a tab strip above the panel content");
        if (tabs) {
            auto clickTab = [tabs](int index) {
                const auto global = tabs->mapToGlobal(tabs->tabRect(index).center());
                const QPointF local = tabs->mapFromGlobal(global);
                QMouseEvent press(QEvent::MouseButtonPress, local, local, QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, local, local, QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tabs, &press);
                QApplication::sendEvent(tabs, &release);
                QCoreApplication::processEvents();
            };
            int sceneIndex = -1, controlsIndex = -1;
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == scenes->windowTitle()) sceneIndex = i;
                if (tabs->tabText(i) == controls->windowTitle()) controlsIndex = i;
            }
            if (sceneIndex >= 0 && controlsIndex >= 0) {
                clickTab(sceneIndex);
                const bool sceneSelected = tabs->currentIndex() == sceneIndex && scenes->isVisible() &&
                    webDockViews.value(scenes) && webDockViews.value(scenes)->isVisible();
                clickTab(controlsIndex);
                check(sceneSelected && tabs->currentIndex() == controlsIndex && controls->isVisible() &&
                      webDockViews.value(controls) && webDockViews.value(controls)->isVisible(),
                      "Clicking either panel name switches the active native WebView dock");
            } else check(false, "Both panel names are clickable native tabs");
        }
        const auto tabbed = main->saveState();
        main->addDockWidget(Qt::RightDockWidgetArea, controls);
        check(main->restoreState(tabbed) && main->tabifiedDockWidgets(scenes).contains(controls),
              "Saving and restoring layout keeps both panels in the same tab group");
        const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            main->grab().save(QDir(artifacts).filePath(QStringLiteral("dock-tabs.png")));
        }
        main->addDockWidget(Qt::RightDockWidgetArea, controls);
        scenes->show();
        scenes->raise();
        QCoreApplication::processEvents();
        controls->setFloating(true);
        controls->resize(320, 300);
        controls->show();
        auto *title = controls->titleBarWidget();
        check(title && controls->isFloating(), "Movable panel has its original floating Qt drag handle");
        if (title) {
            auto mouse = [](QWidget *target, QEvent::Type type, QPoint global, Qt::MouseButton button, Qt::MouseButtons buttons) {
                const QPointF local = target->mapFromGlobal(global);
                QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(target, &event);
            };
            const QPoint grip = title->mapToGlobal(QPoint(20, title->height() / 2));
            const QPoint target = scenes->mapToGlobal(scenes->rect().center());
            blog(LOG_INFO, "[WebView2 tab drag] scenes visible=%d floating=%d rect=%d,%d %dx%d target=%d,%d main=%d,%d %dx%d controls floating=%d grip=%d,%d",
                scenes->isVisible(), scenes->isFloating(), scenes->x(), scenes->y(), scenes->width(), scenes->height(),
                target.x(), target.y(), main->x(), main->y(), main->width(), main->height(), controls->isFloating(), grip.x(), grip.y());
            mouse(title, QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, grip + QPoint(80, 50), Qt::NoButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
            auto *indicator = main->findChild<QWidget *>(QStringLiteral("qt_rubberband"));
            check(indicator && indicator->isVisible(), "Dragging over another panel shows an accepted native dock target");
            mouse(title, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        }
        QTimer::singleShot(180, this, [this, scenes, controls, lock, layout, wasLocked, check, done] {
            check(scenes && controls && !controls->isFloating() &&
                  main->tabifiedDockWidgets(scenes).contains(controls),
                  "Dropping one panel onto another joins them as clickable native tabs");
            restoreSurfaces();
            check(main->tabPosition(Qt::LeftDockWidgetArea) == nativeDockTabPositions[0] &&
                  main->dockOptions() == nativeDockOptions,
                  "Switching back to original OBS restores its previous dock tab and drag settings");
            resumeFrontend();
            check(main->tabPosition(Qt::LeftDockWidgetArea) == QTabWidget::North &&
                  main->dockOptions().testFlag(QMainWindow::GroupedDragging),
                  "Resuming WebView2 places top tabs and allows dragging them out again");
            main->restoreState(layout);
            if (lock) lock->setChecked(wasLocked);
            done();
        });
    });
}

void runMultiAxisDockChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    auto *scenes = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
    auto *sources = main->findChild<QDockWidget *>(QStringLiteral("sourcesDock"));
    auto *mixer = main->findChild<QDockWidget *>(QStringLiteral("mixerDock"));
    auto *transitions = main->findChild<QDockWidget *>(QStringLiteral("transitionsDock"));
    auto *controls = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    if (!scenes || !sources || !mixer || !transitions || !controls || !lock) {
        check(false, "Five movable native docks are available for multi-axis layout");
        done();
        return;
    }
    const auto original = main->saveState();
    const bool wasLocked = lock->isChecked();
    lock->setChecked(false);
    main->showNormal();
    main->resize(1600, 1000);
    main->addDockWidget(Qt::RightDockWidgetArea, scenes);
    main->addDockWidget(Qt::RightDockWidgetArea, sources);
    main->addDockWidget(Qt::RightDockWidgetArea, mixer);
    main->addDockWidget(Qt::RightDockWidgetArea, transitions);
    main->addDockWidget(Qt::RightDockWidgetArea, controls);
    main->splitDockWidget(scenes, sources, Qt::Horizontal);
    main->splitDockWidget(sources, transitions, Qt::Horizontal);
    main->splitDockWidget(scenes, mixer, Qt::Vertical);
    main->splitDockWidget(mixer, controls, Qt::Vertical);
    for (auto *dock : {scenes, sources, mixer, transitions, controls}) dock->show();
    QTimer::singleShot(220, this, [this, scenes = QPointer<QDockWidget>(scenes),
        sources = QPointer<QDockWidget>(sources), mixer = QPointer<QDockWidget>(mixer),
        transitions = QPointer<QDockWidget>(transitions),
        controls = QPointer<QDockWidget>(controls), lock = QPointer<QAction>(lock),
        original, wasLocked, check, done] {
        auto finish = [this, original, wasLocked, lock, done] {
            main->restoreState(original);
            if (lock) lock->setChecked(wasLocked);
            done();
        };
        if (!scenes || !sources || !mixer || !transitions || !controls) {
            check(false, "Multi-axis test docks survive layout changes");
            finish();
            return;
        }
        check(scenes->x() < sources->x() && sources->x() < transitions->x() &&
              scenes->y() < mixer->y() && mixer->y() < controls->y() &&
              main->dockWidgetArea(scenes) == Qt::RightDockWidgetArea &&
              main->dockOptions().testFlag(QMainWindow::AllowNestedDocks),
              "Native layout supports three columns and three rows at once");
        const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
        if (!artifacts.isEmpty())
            main->grab().save(QDir(artifacts).filePath(QStringLiteral("dock-three-by-three.png")));
        const auto multiAxisState = main->saveState();
        main->addDockWidget(Qt::RightDockWidgetArea, transitions);
        const bool restored = main->restoreState(multiAxisState);
        QCoreApplication::processEvents();
        check(restored && scenes->x() < sources->x() && sources->x() < transitions->x() &&
              scenes->y() < mixer->y() && mixer->y() < controls->y(),
              "Saving and restoring layout preserves three columns and rows");
        controls->setFloating(true);
        controls->resize(270, 220);
        controls->show();
        QTimer::singleShot(180, this, [this, scenes, sources, mixer, transitions,
            controls, check, finish] {
            auto *title = controls ? controls->titleBarWidget() : nullptr;
            if (!title || !sources) {
                check(false, "Floating controls have a draggable title");
                finish();
                return;
            }
            auto mouse = [](QWidget *widget, QEvent::Type type, QPoint global,
                            Qt::MouseButton button, Qt::MouseButtons buttons) {
                const QPointF local = widget->mapFromGlobal(global);
                QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(widget, &event);
            };
            const QPoint start = title->mapToGlobal(QPoint(20, title->height() / 2));
            const QPoint target = sources->mapToGlobal(QPoint(sources->width() / 2,
                                                              sources->height() - 8));
            mouse(title, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, start + QPoint(70, 40), Qt::NoButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
            auto *indicator = main->findChild<QWidget *>(QStringLiteral("qt_rubberband"));
            check(indicator && indicator->isVisible(),
                  "Dragging to a dock edge shows a native split target");
            mouse(title, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            QTimer::singleShot(200, this, [this, scenes, sources, mixer, transitions,
                controls, check, finish] {
                check(scenes && sources && mixer && transitions && controls && !controls->isFloating() &&
                      controls->y() > sources->y() &&
                      !main->tabifiedDockWidgets(sources).contains(controls),
                      "Dropping at the lower edge creates another dock row");
                finish();
            });
        });
    });
}

void runDockTabDetachChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    auto *scenes = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
    auto *controls = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    if (!scenes || !controls || !lock) { check(false, "Grouped panel tear-out fixture exists"); done(); return; }
    const auto original = main->saveState();
    const bool wasLocked = lock->isChecked();
    lock->setChecked(false);
    main->showNormal();
    main->resize(1280, 840);
    main->addDockWidget(Qt::LeftDockWidgetArea, scenes);
    main->addDockWidget(Qt::RightDockWidgetArea, controls);
    scenes->show();
    controls->show();
    main->tabifyDockWidget(scenes, controls);
    controls->raise();
    QTimer::singleShot(180, this, [this, scenes = QPointer<QDockWidget>(scenes),
        controls = QPointer<QDockWidget>(controls), lock = QPointer<QAction>(lock),
        original, wasLocked, check, done] {
        if (!scenes || !controls) { check(false, "Grouped docks survive before tab drag"); done(); return; }
        QPointer<QTabBar> tabs;
        int targetIndex = -1;
        for (auto *bar : main->findChildren<QTabBar *>()) {
            bool hasScenes = false;
            int controlsIndex = -1;
            for (int i = 0; i < bar->count(); ++i) {
                hasScenes |= bar->tabText(i) == scenes->windowTitle();
                if (bar->tabText(i) == controls->windowTitle()) controlsIndex = i;
            }
            if (hasScenes && controlsIndex >= 0 && bar->isVisible()) {
                tabs = bar;
                targetIndex = controlsIndex;
                break;
            }
        }
        check(tabs && targetIndex >= 0 && main->tabifiedDockWidgets(scenes).contains(controls),
              "Grouped dock exposes a draggable tab for the panel being removed");
        if (tabs) {
            auto mouse = [](QWidget *widget, QEvent::Type type, QPoint global, Qt::MouseButton button, Qt::MouseButtons buttons) {
                const QPointF local = widget->mapFromGlobal(global);
                QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(widget, &event);
            };
            const QPoint grip = tabs->mapToGlobal(tabs->tabRect(targetIndex).center());
            const QPoint outside = main->mapToGlobal(QPoint(main->width() + 140, main->height() / 2));
            blog(LOG_INFO, "[WebView2 tab tear-out] tab movable=%d grip=%d,%d outside=%d,%d",
                 tabs->isMovable(), grip.x(), grip.y(), outside.x(), outside.y());
            mouse(tabs, QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton);
            if (tabs) mouse(tabs, QEvent::MouseMove, grip + QPoint(35, 1), Qt::NoButton, Qt::LeftButton);
            if (tabs) mouse(tabs, QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
            if (tabs) mouse(tabs, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
        }
        QTimer::singleShot(220, this, [this, scenes, controls, lock, original, wasLocked, check, done] {
            check(scenes && controls && controls->isFloating() && scenes->isVisible() &&
                  !main->tabifiedDockWidgets(scenes).contains(controls) &&
                  webDockViews.value(scenes) && webDockViews.value(controls),
                  "Dragging a grouped tab outside OBS detaches that panel while the other remains usable");
            main->restoreState(original);
            if (lock) lock->setChecked(wasLocked);
            done();
        });
    });
}

void runFloatingGroupChromeChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    auto *scenes = main->findChild<QDockWidget *>(QStringLiteral("scenesDock"));
    auto *controls = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    if (!scenes || !controls || !lock) { check(false, "Floating group fixture exists"); done(); return; }
    const auto original = main->saveState();
    const bool wasLocked = lock->isChecked();
    lock->setChecked(false);
    main->showNormal();
    main->resize(1280, 840);
    main->addDockWidget(Qt::LeftDockWidgetArea, scenes);
    main->addDockWidget(Qt::RightDockWidgetArea, controls);
    scenes->show();
    controls->show();
    main->tabifyDockWidget(scenes, controls);
    controls->raise();
    QTimer::singleShot(180, this, [this, scenes = QPointer<QDockWidget>(scenes),
        controls = QPointer<QDockWidget>(controls), lock = QPointer<QAction>(lock),
        original, wasLocked, check, done] {
        auto mouse = [](QWidget *widget, QEvent::Type type, QPoint global,
                        Qt::MouseButton button, Qt::MouseButtons buttons) {
            const QPointF local = widget->mapFromGlobal(global);
            QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(widget, &event);
        };
        auto *title = controls ? controls->titleBarWidget() : nullptr;
        check(title && title->isVisible(), "Tabbed panel exposes its native group drag title");
        if (title) {
            const QPoint grip = title->mapToGlobal(QPoint(30, title->height() / 2));
            const QPoint outside = main->mapToGlobal(QPoint(main->width() + 180, main->height() / 2));
            mouse(title, QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, grip + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
            mouse(title, QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
            mouse(title, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
        }
        QTimer::singleShot(220, this, [this, scenes, controls,
            lock, original, wasLocked, check, done, mouse] {
            QPointer<QWidget> group;
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->inherits("QDockWidgetGroupWindow") && widget->parentWidget() == main &&
                    widget->findChildren<QDockWidget *>().contains(controls)) { group = widget; break; }
            check(group && group->isVisible() && group->findChildren<QDockWidget *>().contains(scenes),
                  "Dragging a tabbed panel title creates a floating group with both panels");
            if (group) {
                const auto hwnd = reinterpret_cast<HWND>(group->winId());
                COLORREF borderColor = 0;
                using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
                const auto getAttribute = reinterpret_cast<GetAttribute>(
                    QLibrary::resolve(QStringLiteral("dwmapi"), "DwmGetWindowAttribute"));
                const HRESULT borderResult = getAttribute ? getAttribute(hwnd, DWMWA_BORDER_COLOR,
                    &borderColor, sizeof(borderColor)) : E_NOTIMPL;
                blog(LOG_INFO, "[WebView2 group frame] style=%llx exStyle=%llx borderResult=%lx borderColor=%06lx palette=%s",
                    static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                    static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                    static_cast<unsigned long>(borderResult), static_cast<unsigned long>(borderColor),
                    group->palette().color(QPalette::Window).name().toUtf8().constData());
                check(group->styleSheet().contains(QStringLiteral("border: 1px solid %1")
                      .arg(themeEdgeColor(group->palette()).name())) &&
                      group->style()->pixelMetric(QStyle::PM_DockWidgetFrameWidth, nullptr, group) == 1,
                      "Floating group uses a visible theme frame instead of the bright Qt bevel");
                const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
                if (!artifacts.isEmpty()) {
                    group->grab().save(QDir(artifacts).filePath(QStringLiteral("frame-floating-group.png")));
                    group->raise();
                    group->activateWindow();
                    QCoreApplication::processEvents();
                    const QRect frame = group->frameGeometry();
                    if (auto *screen = group->screen())
                        screen->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
                            .save(QDir(artifacts).filePath(QStringLiteral("frame-floating-group-native.png")));
                }
                QTabBar *tabs = nullptr;
                for (auto *bar : group->findChildren<QTabBar *>())
                    if (bar->isVisible()) { tabs = bar; break; }
                if (tabs && tabs->tabAt(QPoint(tabs->width() - 10, tabs->height() / 2)) >= 0) {
                    group->resize(group->width() + 180, group->height());
                    QCoreApplication::processEvents();
                }
                const QPoint blank = tabs ? tabs->mapToGlobal(QPoint(tabs->width() - 10, tabs->height() / 2)) : QPoint();
                check(tabs && tabs->tabAt(tabs->mapFromGlobal(blank)) < 0,
                      "Floating group has a blank strip beside its tabs");
                if (tabs && tabs->tabAt(tabs->mapFromGlobal(blank)) < 0) {
                    const QPoint before = group->pos();
                    mouse(tabs, QEvent::MouseButtonPress, blank, Qt::LeftButton, Qt::LeftButton);
                    mouse(tabs, QEvent::MouseMove, blank + QPoint(85, 48), Qt::NoButton, Qt::LeftButton);
                    mouse(tabs, QEvent::MouseButtonRelease, blank + QPoint(85, 48), Qt::LeftButton, Qt::NoButton);
                    check(group && group->pos() == before + QPoint(85, 48),
                          "Dragging the blank top strip moves the entire floating group");
                }
            }
            main->restoreState(original);
            if (lock) lock->setChecked(wasLocked);
            done();
        });
    });
}

void runWindowFrameChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    auto captureHandle = [](HWND hwnd, const QString &name) -> QImage {
        const auto artifacts = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
        if (!hwnd) return {};
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) return {};
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return {};
        HDC source = GetWindowDC(hwnd);
        if (!source) return {};
        HDC target = CreateCompatibleDC(source);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        QImage image;
        if (target && bitmap && pixels) {
            HGDIOBJ previous = SelectObject(target, bitmap);
            if (!PrintWindow(hwnd, target, PW_RENDERFULLCONTENT))
                BitBlt(target, 0, 0, width, height, source, 0, 0, SRCCOPY);
            image = QImage(reinterpret_cast<uchar *>(pixels), width, height, QImage::Format_RGB32)
                .copy(0, 0, width, qMin(height, 70));
            if (!artifacts.isEmpty()) image.save(QDir(artifacts).filePath(name));
            SelectObject(target, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (target) DeleteDC(target);
        ReleaseDC(hwnd, source);
        return image;
    };
    auto captureFrame = [captureHandle](QWidget *window, const QString &name) -> QImage {
        if (!window) return {};
        window->raise();
        window->activateWindow();
        QCoreApplication::processEvents();
        return captureHandle(reinterpret_cast<HWND>(window->winId()), name);
    };
    auto matchesPalette = [](const QImage &frame, const QWidget *window,
                             QPalette::ColorRole role = QPalette::Window) {
        if (frame.isNull() || frame.width() < 16 || frame.height() < 16 || !window) return false;
        const QColor expected = window->palette().color(role);
        const QColor actual = frame.pixelColor(frame.width() / 2, 5);
        return std::abs(actual.red() - expected.red()) <= 12 &&
               std::abs(actual.green() - expected.green()) <= 12 &&
               std::abs(actual.blue() - expected.blue()) <= 12;
    };
    const auto mainFrame = captureFrame(main, QStringLiteral("frame-main.png"));
    check(matchesPalette(mainFrame, main), "Main OBS caption uses its Qt theme instead of the Windows accent color");

    QDialog dialog(main);
    dialog.setWindowTitle(QStringLiteral("WebView2 themed window test"));
    dialog.show();
    QCoreApplication::processEvents();
    const auto dialogFrame = captureFrame(&dialog, QStringLiteral("frame-dialog.png"));
    check(matchesPalette(dialogFrame, &dialog), "New application dialogs inherit the themed native caption");
    auto palette = dialog.palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#263449")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#f0e0d0")));
    dialog.setPalette(palette);
    QCoreApplication::processEvents();
    const auto changedFrame = captureFrame(&dialog, QStringLiteral("frame-dialog-custom.png"));
    check(matchesPalette(changedFrame, &dialog) &&
          changedFrame.pixelColor(changedFrame.width() / 2, 5) != dialogFrame.pixelColor(dialogFrame.width() / 2, 5),
          "Changing a window's palette updates its native title frame without reopening it");
    dialog.close();

    HWND native = CreateWindowExW(0, L"STATIC", L"WebView2 native dialog frame test", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 360, 180, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (native) {
        ShowWindow(native, SW_SHOWNOACTIVATE);
        QCoreApplication::processEvents();
        const auto nativeFrame = captureHandle(native, QStringLiteral("frame-native-window.png"));
        check(matchesPalette(nativeFrame, main), "Native Windows dialogs owned by OBS also receive the application theme");
        DestroyWindow(native);
    } else check(false, "Native Windows dialog frame fixture exists");

    auto *dock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
    if (!dock || !lock) { check(false, "Floating themed dock fixture exists"); done(); return; }
    const auto original = main->saveState();
    const bool wasLocked = lock->isChecked();
    lock->setChecked(false);
    dock->setFloating(true);
    dock->show();
    QCoreApplication::processEvents();
    const auto dockFrame = captureFrame(dock, QStringLiteral("frame-floating-dock.png"));
    const QImage titleImage = dock->titleBarWidget() ? dock->titleBarWidget()->grab().toImage() : QImage();
    const QColor titleColor = titleImage.isNull() ? QColor() : titleImage.pixelColor(titleImage.width() / 2, 5);
    const QColor frameColor = dockFrame.isNull() ? QColor() : dockFrame.pixelColor(dockFrame.width() / 2, 5);
    check(titleColor.isValid() && frameColor.isValid() &&
          std::abs(titleColor.red() - frameColor.red()) <= 12 &&
          std::abs(titleColor.green() - frameColor.green()) <= 12 &&
          std::abs(titleColor.blue() - frameColor.blue()) <= 12,
          "Floating WebView2 panels also use their Qt theme for the window frame");
    main->restoreState(original);
    lock->setChecked(wasLocked);
    done();
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

    QJsonObject layout{{"viewportWidth", browser->width()}, {"viewportHeight", browser->height()},
        {"surfaces", QJsonArray{bounds, QJsonObject{{"target", "program"}, {"x", 0}, {"y", 0},
            {"width", 0}, {"height", 0}, {"visible", false}}}}, {"overlays", QJsonArray{}}};
    execute({{"id", "layout-batch-test"}, {"command", "preview.layout"}, {"args", layout}});
    check(lastTestReply.value("ok").toBool() && preview->GetDisplay() == display &&
          preview->geometry() == video.translated(browser->pos()),
          "One layout message updates both apertures without replacing the native GPU display");
    const auto beforeInvalid = preview->geometry();
    auto invalidSurfaces = layout.value("surfaces").toArray();
    auto invalidProgram = invalidSurfaces.at(1).toObject(); invalidProgram.insert("width", -1);
    invalidSurfaces[1] = invalidProgram; layout.insert("surfaces", invalidSurfaces);
    execute({{"id", "layout-invalid-test"}, {"command", "preview.layout"}, {"args", layout}});
    check(!lastTestReply.value("ok").toBool() && preview->geometry() == beforeInvalid,
          "An invalid second aperture rejects the complete resize before changing either display");

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
    check(!toggle, "Native dock title has no pin or attach control");
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

void runQueuedNativeRemovalCheck(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    struct Fixture {
        OBSSource previous;
        OBSSceneAutoRelease scene{obs_scene_create("WebView queued removal lifetime")};
        OBSSourceAutoRelease source{obs_source_create("color_source_v3", "WebView queued removal source", nullptr, nullptr)};
        OBSWeakSource weak;
    };
    auto fixture = std::make_shared<Fixture>();
    fixture->previous = static_cast<OBSBasic *>(main)->GetCurrentSceneSource();
    fixture->weak = OBSGetWeakRef(fixture->source.Get());
    obs_scene_add(fixture->scene, fixture->source);
    obs_frontend_set_current_scene(obs_scene_get_source(fixture->scene));
    QTimer::singleShot(100, this, [this, fixture, check, done] {
        auto *tree = main->findChild<SourceTree *>("sources");
        QPointer<SourceTreeItem> oldWidget = tree->GetItemWidget(0);
        obs_sceneitem_t *item = obs_scene_find_source(fixture->scene, obs_source_get_name(fixture->source));
        check(item && oldWidget, "Queued removal fixture has an actual native SourceTree item and callback");
        if (item && oldWidget) {
            // The real libobs callback runs off the GUI thread and queues Remove.
            // Drop the old model/widget owners before processing that MetaCall,
            // reproducing the Undo/Redo + scene-reset lifetime from the crash.
            auto *worker = QThread::create([item] { obs_sceneitem_remove(item); });
            worker->start();
            worker->wait();
            delete worker;
            tree->Clear();
            if (oldWidget) QCoreApplication::sendPostedEvents(oldWidget, QEvent::DeferredDelete);
            fixture->source = nullptr;
            check(!!OBSGetStrongRef(fixture->weak), "Queued native removal retains its scene item and source after old widgets are destroyed");
        }
        QTimer::singleShot(100, this, [this, fixture, check, done] {
            check(!OBSGetStrongRef(fixture->weak), "Queued native removal releases its source after safe GUI delivery");
            obs_frontend_set_current_scene(fixture->previous);
            obs_source_remove(obs_scene_get_source(fixture->scene));
            done();
        });
    });
}

void runGroupFinishCase(int mode, std::function<void(bool, const char *)> check, std::function<void()> done)
{
    if (mode == 4) { done(); return; }
    struct GroupFinishFixture {
        OBSSource previous;
        OBSSceneAutoRelease scene;
        OBSSourceAutoRelease a, b;
    };
    auto fixture = std::make_shared<GroupFinishFixture>();
    fixture->previous = static_cast<OBSBasic *>(main)->GetCurrentSceneSource();
    const auto suffix = QString::number(mode).toUtf8();
    fixture->scene = obs_scene_create((QByteArray("WebView group finish ") + suffix).constData());
    fixture->a = obs_source_create("color_source_v3", (QByteArray("WebView group finish A ") + suffix).constData(), nullptr, nullptr);
    fixture->b = obs_source_create("color_source_v3", (QByteArray("WebView group finish B ") + suffix).constData(), nullptr, nullptr);
    obs_scene_add(fixture->scene, fixture->a);
    obs_scene_add(fixture->scene, fixture->b);
    obs_frontend_set_current_scene(obs_scene_get_source(fixture->scene));
    QTimer::singleShot(100, this, [this, fixture, mode, check, done] {
        auto *tree = main->findChild<SourceTree *>("sources");
        tree->selectAll();
        execute({{"id", "group-finish-start"}, {"command", "native.command"},
            {"args", QJsonObject{{"id", "source.group"}, {"context", snapshot().value("context")}}}});
        QTimer::singleShot(100, this, [this, fixture, mode, check, done] {
            auto *tree = main->findChild<SourceTree *>("sources");
            QJsonObject group;
            QPointer<SourceTreeItem> editorItem;
            for (int row = 0; row < tree->model()->rowCount(); ++row)
                if (obs_sceneitem_is_group(tree->Get(row))) editorItem = tree->GetItemWidget(row);
            for (const auto &value : snapshot().value("sources").toArray())
                if (value.toObject().value("group").toBool()) { group = value.toObject(); break; }
            check(editorItem && editorItem->IsEditing(), "Lifecycle fixture retains its pending native group editor");
            if (editorItem) {
                auto *edit = editorItem->findChild<QLineEdit *>();
                QFocusEvent focusOut(QEvent::FocusOut);
                if (edit) QApplication::sendEvent(edit, &focusOut);
                check(edit && editorItem->IsEditing(), "Moving focus to Chromium does not prematurely finalize the group undo action");
            }
            if (mode == 3) {
                OBSSourceAutoRelease source = obs_get_source_by_uuid(group.value("uuid").toString().toUtf8().constData());
                check(!!source, "Pending group source exists before external deletion");
                if (source) obs_source_remove(source);
            } else if (mode == 2) {
                // Exercise native/plugin scene switching, bypassing execute()'s
                // normal blur-before-command cleanup.
                obs_frontend_set_current_scene(fixture->previous);
            } else {
                group.insert("scene", snapshot().value("currentScene"));
                group.insert("context", snapshot().value("context"));
                group.insert("save", mode == 0);
                execute({{"id", "group-finish-name"}, {"command", "source.rename"}, {"args", group}});
                check(lastTestReply.value("ok").toBool(), mode == 0 ?
                    "Unchanged group name explicitly commits the native transaction" :
                    "Escape explicitly finishes grouping without changing the default name");
            }
            QTimer::singleShot(100, this, [this, fixture, editorItem, mode, check, done] {
                check(!editorItem || !editorItem->IsEditing(), "Group editor is finalized after commit, Escape, native scene switch or source deletion");
                auto *basic = static_cast<OBSBasic *>(main);
                auto undoWorked = std::make_shared<bool>(false);
                basic->undo_s.add_action("WebView group finish balance", [undoWorked](const std::string &) { *undoWorked = true; },
                    [](const std::string &) {}, "", "");
                basic->undo_s.undo();
                check(*undoWorked, mode == 0 ? "Unchanged group commit leaves undo enabled" : mode == 1 ?
                    "Escape from group naming leaves undo enabled" : mode == 2 ?
                    "Native scene switch during group naming leaves undo enabled" : "External group source deletion leaves undo enabled without a stale item dereference");
                basic->undo_s.clear();
                obs_frontend_set_current_scene(fixture->previous);
                obs_source_remove(obs_scene_get_source(fixture->scene));
                obs_source_remove(fixture->a);
                obs_source_remove(fixture->b);
                runGroupFinishCase(mode + 1, check, done);
            });
        });
    });
}

void runWorkspaceChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
    done = [this, check, complete = std::move(done)] {
        runGroupFinishCase(0, check, [this, check, complete] {
            runQueuedNativeRemovalCheck(check, [this, check, complete] { runOverlayAndFloatingDockChecks(check, complete); });
        });
    };
    struct WorkspaceFixture {
        OBSSceneAutoRelease scene{obs_scene_create("WebView Workspace Editing")};
        OBSSceneAutoRelease sceneSecond{obs_scene_create("WebView Workspace Ordering B")};
        OBSSceneAutoRelease sceneThird{obs_scene_create("WebView Workspace Ordering C")};
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
        auto selectedCount = [this] {
            int count = 0;
            for (const auto &value : snapshot().value("sources").toArray()) count += value.toObject().value("selected").toBool();
            return count;
        };
        send("source.select", c);
        auto rangeB = b, rangeA = a;
        rangeB.insert("range", true); rangeA.insert("range", true);
        send("source.select", rangeB); send("source.select", rangeA);
        check(selectedCount() == 3, "Repeated Shift navigation retains its original source-selection anchor");
        auto focusB = b; focusB.insert("focusOnly", true);
        send("source.select", focusB);
        check(selectedCount() == 3, "Ctrl navigation changes native current index without replacing source selection");
        send("source.select", c);
        auto additiveA = a; additiveA.insert("additive", true); send("source.select", additiveA);
        rangeB.insert("additive", true); send("source.select", rangeB);
        check(selectedCount() == 3, "Ctrl+Shift range extends the existing noncontiguous native source selection");
        send("source.select", a);
        check(send("source.selectAll", {{"scene", snapshot().value("currentScene")}}) && selectedCount() == 3,
              "Select All selects every source through the native SourceTree");
        class HoverEvents final : public QObject {
        public:
            int enters = 0, leaves = 0;
            bool eventFilter(QObject *, QEvent *event) override {
                enters += event->type() == QEvent::Enter;
                leaves += event->type() == QEvent::Leave;
                return false;
            }
        } hoverEvents;
        auto *sourceTree = main->findChild<SourceTree *>(QStringLiteral("sources"));
        auto *hoverItem = sourceTree ? sourceTree->GetItemWidget(sourceRow(c)) : nullptr;
        if (hoverItem) hoverItem->installEventFilter(&hoverEvents);
        auto hover = c; hover.insert("value", true); send("source.hover", hover);
        check(hoverItem && hoverEvents.enters == 1, "Source row hover reaches the original native item Enter handler");
        hover.insert("value", false); send("source.hover", hover);
        check(hoverItem && hoverEvents.leaves == 1, "Leaving the source row reaches the original native item Leave handler");
        if (hoverItem) hoverItem->removeEventFilter(&hoverEvents);
        send("source.select", c);
        auto endDrop = c; endDrop.insert("position", "end");
        check(send("source.move", endDrop) && snapshot().value("sources").toArray().last().toObject().value("uuid") == SourceId(fixture->c),
              "Dropping in source viewport empty space moves the selection to the native list end");
        static_cast<OBSBasic *>(main)->undo_s.undo();
        const auto grid = snapshot().value("sceneGrid").toObject();
        auto *nativeScenes = main->findChild<QListWidget *>(QStringLiteral("scenes"));
        check(nativeScenes && grid.value("width").toInt() == nativeScenes->property("gridItemWidth").toInt() &&
                  grid.value("height").toInt() == nativeScenes->property("gridItemHeight").toInt() && grid.value("height").toInt() > 0,
              "Scene grid dimensions come from the original themed SceneTree");
        const auto sceneUuid = SourceId(obs_scene_get_source(fixture->scene));
        const auto secondUuid = SourceId(obs_scene_get_source(fixture->sceneSecond));
        auto sceneIndex = [this](const QString &uuid) {
            const auto list = snapshot().value("scenes").toArray();
            for (int i = 0; i < list.size(); ++i) if (list[i].toObject().value("uuid").toString() == uuid) return i;
            return -1;
        };
        check(send("scene.move", {{"uuid", sceneUuid}, {"target", secondUuid}, {"position", "after"}}) && sceneIndex(sceneUuid) == sceneIndex(secondUuid) + 1,
              "Scene drop after a row uses native scene ordering at the requested insertion edge");
        check(send("scene.move", {{"uuid", sceneUuid}, {"target", secondUuid}, {"position", "before"}}) && sceneIndex(secondUuid) == sceneIndex(sceneUuid) + 1,
              "Scene drop before a row preserves the requested native insertion edge");
        check(send("scene.move", {{"uuid", sceneUuid}, {"position", "end"}}) && sceneIndex(sceneUuid) == snapshot().value("scenes").toArray().size() - 1,
              "Scene viewport drop reaches the native list end");
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
            check(grouped && editing, "Grouping keeps the original name transaction pending for the WebView input");
            auto *basic = static_cast<OBSBasic *>(main);
            QJsonObject group;
            for (const auto &value : snapshot().value("sources").toArray())
                if (value.toObject().value("group").toBool()) { group = value.toObject(); break; }
            group.insert("scene", snapshot().value("currentScene"));
            group.insert("context", snapshot().value("context"));
            group.insert("name", QStringLiteral("WebView named group undo transaction"));
            execute({{"id", "group-name-test"}, {"command", "source.rename"}, {"args", group}});
            check(lastTestReply.value("ok").toBool(), "Custom group name commits through the production source.rename bridge");
            auto trigger = [this](const char *name) {
                auto *action = main->findChild<QAction *>(QLatin1String(name));
                if (!action) return false;
                execute({{"id", "group-action-test"}, {"command", "action.invoke"},
                    {"args", QJsonObject{{"id", registerAction(action)}, {"context", snapshot().value("context")}}}});
                return lastTestReply.value("ok").toBool();
            };
            const bool undone = trigger("actionMainUndo");
            bool anyGroup = false;
            for (int row = 0; row < tree->model()->rowCount(); ++row) anyGroup |= obs_sceneitem_is_group(tree->Get(row));
            check(undone && !anyGroup, "One native Undo ungroups the sources after entering a custom WebView group name");
            const bool redone = trigger("actionMainRedo");
            bool namedGroup = false;
            for (int row = 0; row < tree->model()->rowCount(); ++row) {
                const auto item = tree->Get(row);
                namedGroup |= obs_sceneitem_is_group(item) && QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))) == group.value("name").toString();
            }
            check(redone && namedGroup, "One native Redo restores grouping and its custom name together");
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
