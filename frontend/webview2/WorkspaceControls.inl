// Member implementations for the WebView2 workspace adapter.
// Original Qt controllers remain the authority for actions, undo and saving.
QString registerButton(QAbstractButton *button)
{
    if (buttonIds.contains(button))
        return buttonIds.value(button);
    const auto id = QStringLiteral("b%1").arg(++nextButton);
    liveButtons.insert(id, button);
    buttonIds.insert(button, id);
    connect(button, &QObject::destroyed, this, [this, button, id] {
        liveButtons.remove(id);
        buttonIds.remove(button);
    });
    return id;
}

QJsonArray toolbarState(const char *name)
{
    auto *toolbar = main->findChild<QToolBar *>(QLatin1String(name));
    return toolbar ? menuState(toolbar->actions()) : QJsonArray{};
}

QJsonArray sourceRows(obs_source_t *current)
{
    QJsonArray rows;
    auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
    if (!tree || !current)
        return rows;
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        const OBSSceneItem item = tree->Get(row);
        if (!item)
            continue;
        auto *source = obs_sceneitem_get_source(item);
        auto *owner = obs_sceneitem_get_scene(item);
        OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
        auto *basic = static_cast<OBSBasic *>(main);
        const auto type = QByteArray(obs_source_get_id(source));
        const auto icon = type == "scene" ? basic->GetSceneIcon() : type == "group" ? basic->GetGroupIcon() : basic->GetSourceIcon(type.constData());
        if (!sourceIcons.contains(icon.cacheKey())) {
            if (sourceIcons.size() > 256) sourceIcons.clear();
            QByteArray bytes;
            QBuffer buffer(&bytes);
            if (buffer.open(QIODevice::WriteOnly) && icon.pixmap(32, 32).save(&buffer, "PNG"))
                sourceIcons.insert(icon.cacheKey(), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
        }
        rows.append(QJsonObject{{"id", QString::number(obs_sceneitem_get_id(item))},
            {"uuid", SourceId(source)}, {"owner", SourceId(obs_scene_get_source(owner))},
            {"name", QString::fromUtf8(obs_source_get_name(source))},
            {"icon", sourceIcons.value(icon.cacheKey())},
            {"visible", obs_sceneitem_visible(item)}, {"locked", obs_sceneitem_locked(item)},
            {"selected", obs_sceneitem_selected(item)}, {"group", obs_sceneitem_is_group(item)},
            {"collapsed", obs_data_get_bool(settings, "collapsed")},
            {"depth", owner == obs_scene_from_source(current) ? 0 : 1},
            {"interactive", bool(obs_source_get_output_flags(source) & OBS_SOURCE_INTERACTION)}});
    }
    return rows;
}

int sourceRow(const QJsonObject &args)
{
    auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
    auto *basic = static_cast<OBSBasic *>(main);
    const auto current = basic->GetCurrentSceneSource();
    if (!tree || args.value("scene").toString() != SourceId(current))
        return -1;
    const QString owner = args.value("owner").toString(SourceId(current));
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        const OBSSceneItem item = tree->Get(row);
        if (item && SourceId(obs_scene_get_source(obs_sceneitem_get_scene(item))) == owner &&
            QString::number(obs_sceneitem_get_id(item)) == args.value("id").toString() &&
            SourceId(obs_sceneitem_get_source(item)) == args.value("uuid").toString())
            return row;
    }
    return -1;
}

void finishGroupName()
{
    auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
    if (!tree)
        return;
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        auto *widget = tree->GetItemWidget(row);
        if (widget && widget->IsEditing())
            QMetaObject::invokeMethod(widget, "ExitEditMode", Qt::DirectConnection, Q_ARG(bool, true));
    }
}

void syncProgramSurface()
{
    if (!surfacesBorrowed || !obs_frontend_preview_program_mode_active())
        return;
    if (!programPreview) {
        programPreview = main->findChild<OBSQTDisplay *>(QStringLiteral("studioProgramPreview"));
        if (programPreview) {
            programPlacement.take(programPreview, this);
        }
    }
}

bool isWebDock(QDockWidget *dock) const
{
    static const QStringList names{"scenesDock", "sourcesDock", "mixerDock", "transitionsDock", "controlsDock"};
    return names.contains(dock->objectName());
}

QString webDockAssets, webDockProfile;
QHash<QDockWidget *, QPointer<WebView2Widget>> webDockViews;
bool restoringWebDocks = false;

// QDockWidget owns all movement, resizing, drop targets, splits and tabs.
// A custom Qt title bar suppresses the operating-system caption on a floating
// dock. Ignored mouse events still reach QDockWidget's original drag handler.
class WebDockTitleBar final : public QWidget {
    QPointer<QDockWidget> dock;
    QLabel *title;
    QToolButton *toggle;
    QToolButton *close;
    QString dockText, floatText;

    void updateControls()
    {
        if (!dock) return;
        title->setText(dock->windowTitle());
        const bool floating = dock->isFloating();
        toggle->setText(floating ? dockText : floatText);
        toggle->setToolTip(toggle->text());
        toggle->setAccessibleName(toggle->text());
        toggle->setToolButtonStyle(floating ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
        toggle->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
        toggle->setEnabled(dock->features().testFlag(QDockWidget::DockWidgetFloatable));
        close->setVisible(dock->features().testFlag(QDockWidget::DockWidgetClosable));
        updateGeometry();
        update();
    }
protected:
    void mousePressEvent(QMouseEvent *event) override { event->ignore(); }
    void mouseMoveEvent(QMouseEvent *event) override { event->ignore(); }
    void mouseReleaseEvent(QMouseEvent *event) override { event->ignore(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { event->ignore(); }
    void paintEvent(QPaintEvent *) override
    {
        if (!dock) return;
        QStyleOptionDockWidget option;
        option.initFrom(dock);
        option.rect = rect();
        option.movable = dock->features().testFlag(QDockWidget::DockWidgetMovable);
        QPainter painter(this);
        style()->drawControl(QStyle::CE_DockWidgetTitle, &option, &painter, this);
    }
public:
    WebDockTitleBar(QDockWidget *parent, const QString &attachText, const QString &detachText)
        : QWidget(parent), dock(parent), dockText(attachText), floatText(detachText)
    {
        setObjectName(QStringLiteral("obsWebView2DockTitleBar"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 1, 2, 1);
        layout->setSpacing(2);
        title = new QLabel(this);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(title, 1);
        toggle = new QToolButton(this);
        toggle->setObjectName(QStringLiteral("obsWebView2DockToggle"));
        toggle->setAutoRaise(true);
        layout->addWidget(toggle);
        close = new QToolButton(this);
        close->setObjectName(QStringLiteral("obsWebView2DockClose"));
        close->setAutoRaise(true);
        close->setIcon(style()->standardIcon(QStyle::SP_DockWidgetCloseButton));
        close->setToolTip(QDockWidget::tr("Close"));
        close->setAccessibleName(close->toolTip());
        layout->addWidget(close);
        connect(toggle, &QToolButton::clicked, this, [this] {
            if (!dock || !dock->features().testFlag(QDockWidget::DockWidgetFloatable)) return;
            dock->setFloating(!dock->isFloating());
            dock->show();
            dock->raise();
        });
        connect(close, &QToolButton::clicked, this, [this] {
            if (dock && dock->features().testFlag(QDockWidget::DockWidgetClosable)) dock->close();
        });
        connect(parent, &QDockWidget::topLevelChanged, this, [this] { updateControls(); });
        connect(parent, &QDockWidget::featuresChanged, this, [this] { updateControls(); });
        connect(parent, &QWidget::windowTitleChanged, this, [this] { updateControls(); });
        updateControls();
    }
    QSize minimumSizeHint() const override
    {
        const auto hint = layout()->minimumSize();
        return {hint.width() + 40, std::max(hint.height(), fontMetrics().height() + 8)};
    }
    QSize sizeHint() const override { return minimumSizeHint(); }
};

struct WebDockChrome {
    QPointer<QWidget> original;
    QPointer<WebDockTitleBar> owned;
};
QHash<QDockWidget *, WebDockChrome> webDockChrome;

void installWebDockChrome(QDockWidget *dock)
{
    if (webDockChrome.contains(dock)) return;
    const bool russian = QByteArray(App()->GetLocale()).startsWith("ru");
    auto *title = new WebDockTitleBar(dock, russian ? QStringLiteral("Закрепить") : QDockWidget::tr("Dock"),
        russian ? QStringLiteral("Отделить панель") : QDockWidget::tr("Float"));
    const QPointer<QWidget> original = dock->titleBarWidget();
    webDockChrome.insert(dock, {original, title});
    if (original) original->hide();
    dock->setTitleBarWidget(title);
}

void restoreWebDockChrome(QDockWidget *dock)
{
    if (!webDockChrome.contains(dock)) return;
    const auto chrome = webDockChrome.take(dock);
    if (dock->titleBarWidget() == chrome.owned) {
        dock->setTitleBarWidget(chrome.original);
        if (chrome.original) chrome.original->show();
    }
    if (chrome.owned) delete chrome.owned.data();
}

class WebDockResizeFilter final : public QObject {
    QPointer<WebView2Widget> view;
public:
    WebDockResizeFilter(QWidget *content, WebView2Widget *surface) : QObject(surface), view(surface)
    {
        setObjectName(QStringLiteral("obsWebView2DockResizeFilter"));
        content->installEventFilter(this);
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (view && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            view->setGeometry(static_cast<QWidget *>(object)->rect());
            view->raise();
        }
        return false;
    }
};

void initializeWebDocks(const QString &assets, const QString &profile)
{
    webDockAssets = assets;
    webDockProfile = profile;
    for (auto *dock : main->findChildren<QDockWidget *>())
        if (isWebDock(dock) && dock->widget())
            dock->widget()->setProperty("_obsWebView2ExternalSurface", true);
}

void postWorkspaceMessage(const QJsonObject &message)
{
    const bool meters = message.value("event") == QStringLiteral("audio.levels");
    if (!meters || !property("webview2NativeDocking").toBool()) browser->postMessage(message);
    // Bridge request IDs contain a random session UUID. Only the requesting
    // surface consumes replies; state and meter events reach every panel.
    for (auto surface : webDockViews)
        if (surface && (!message.contains("event") || surface->isVisible()) &&
            (!meters || surface->property("webview2Dock") == QStringLiteral("mixerDock"))) surface->postMessage(message);
}

void restoreWebDocks()
{
    // Keep original dock contents, controller objects and Qt floating state.
    // The same docks become the normal Qt panels when the native UI is shown.
    restoringWebDocks = true;
    for (auto surface : webDockViews)
        if (surface) surface->hide();
    for (auto *dock : webDockChrome.keys()) restoreWebDockChrome(dock);
    restoringWebDocks = false;
}

void destroyWebDocks()
{
    // The native dock content outlives this frontend. Release our browser hosts
    // explicitly so reopening the frontend cannot accumulate hidden controllers.
    restoreWebDocks();
    const auto surfaces = webDockViews;
    webDockViews.clear();
    for (auto surface : surfaces)
        if (surface) delete surface.data();
}

void showWebDock(QDockWidget *dock)
{
    if (!dock || !isWebDock(dock) || !dock->widget() || restoringWebDocks ||
        webDockAssets.isEmpty() || !surfacesBorrowed || !isVisible() ||
        !property("webview2NativeDocking").toBool()) return;
    auto surface = webDockViews.value(dock);
    if (!surface) {
        auto *content = dock->widget();
        surface = new WebView2Widget(content, webDockAssets, webDockProfile);
        surface->setObjectName(QStringLiteral("obsWebView2DockSurface"));
        surface->setProperty("webview2Dock", dock->objectName());
        surface->setGeometry(content->rect());
        webDockViews.insert(dock, surface);
        new WebDockResizeFilter(content, surface);
        connect(surface, &WebView2Widget::ready, this, [this, surface, dock = QPointer<QDockWidget>(dock)] {
            if (!surface || !dock) return;
            surface->setProperty("webview2Ready", true);
            surface->postMessage({{"version", 1}, {"event", "workspace.panel"},
                                  {"data", QJsonObject{{"name", dock->objectName()}, {"nativeDocking", true}}}});
            surface->postMessage({{"version", 1}, {"event", "state.changed"}, {"data", snapshot()}});
            surface->raise();
        });
        connect(surface, &WebView2Widget::messageReceived, this, [this, surface](const QJsonObject &message) {
            if (!surface) return;
            if (message.value("command") == QStringLiteral("preview.bounds")) {
                // A child document cannot move or hide the main GPU preview.
                surface->postMessage({{"version", 1}, {"id", message.value("id")}, {"ok", true}, {"result", QJsonObject{}}});
                return;
            }
            execute(message);
        });
        connect(surface, &WebView2Widget::externalDrop, this,
                [this, surface](const QString &id, const OBSWeb::ExternalDropData &drop) {
            if (!surface) return;
            surface->postMessage({{"version", 1}, {"id", id}, {"ok", true}, {"result", QJsonObject{{"accepted", true}}}});
            OBSWeb::DispatchExternalDrop(main, drop);
        });
        connect(surface, &WebView2Widget::failed, this, [this, surface, dock = QPointer<QDockWidget>(dock)](const QString &error) {
            if (surface) { surface->setProperty("webview2Failed", true); surface->hide(); }
            if (dock) { restoreWebDockChrome(dock); dock->setToolTip(error); }
            blog(LOG_ERROR, "[WebView2] Floating panel failed: %s", error.toUtf8().constData());
        });
    }
    surface->setGeometry(dock->widget()->rect());
    if (surface->property("webview2Failed").toBool()) return;
    installWebDockChrome(dock);
    surface->show();
    surface->raise();
}

void showNativeDock(QDockWidget *dock)
{
    // Plugin widgets retain their original dock area, tabs and ownership.
    dock->show();
    dock->raise();
    dock->activateWindow();
}

void routeDocks()
{
    if (restoringWebDocks) return;
    for (auto *dock : main->findChildren<QDockWidget *>()) {
        if (routedDocks.contains(dock)) continue;
        routedDocks.insert(dock);
        connect(dock, &QObject::destroyed, this, [this, dock] {
            routedDocks.remove(dock);
            webDockViews.remove(dock);
            webDockChrome.remove(dock);
        });
        connect(dock, &QDockWidget::topLevelChanged, this,
                [this, dock = QPointer<QDockWidget>(dock)](bool) {
            if (!dock || !isWebDock(dock)) return;
            showWebDock(dock);
            QTimer::singleShot(0, this, [this] { publishState(true); });
        });
        connect(dock, &QDockWidget::visibilityChanged, this,
                [this, dock = QPointer<QDockWidget>(dock)](bool visible) {
            if (dock && visible && isWebDock(dock)) showWebDock(dock);
        });
        connect(dock->toggleViewAction(), &QAction::triggered, this,
                [this, dock = QPointer<QDockWidget>(dock)](bool visible) {
            if (dock && visible && isVisible()) {
                if (isWebDock(dock)) showWebDock(dock);
                else showNativeDock(dock);
            }
            publishState(true);
        });
    }
    if (surfacesBorrowed && isVisible())
        for (auto *dock : main->findChildren<QDockWidget *>())
            if (isWebDock(dock) && !dock->isHidden()) showWebDock(dock);
}

void addWorkspaceState(QJsonObject &state)
{
    syncProgramSurface();
    routeDocks();
    state.insert("nativeEditor", nativeEditor);
    state.insert("sceneToolbar", toolbarState("scenesToolbar"));
    state.insert("sourceToolbar", toolbarState("sourcesToolbar"));
    state.insert("actions", menuState(main->findChildren<QAction *>()));
    state.insert("audio", audio ? audio->snapshot() : QJsonArray{});
    QString selectedSource;
    int selectedCount = 0;
    for (const auto &entry : state.value("sources").toArray()) {
        const auto source = entry.toObject();
        if (source.value("selected").toBool()) { selectedSource = source.value("uuid").toString(); ++selectedCount; }
    }
    if (selectedCount != 1) selectedSource.clear();
    if (toolbarSource != selectedSource) {
        toolbarSource = selectedSource;
        static_cast<OBSBasic *>(main)->UpdateContextBar(true);
    }
    auto *toolbarArea = main->findChild<QWidget *>("emptySpace");
    state.insert("sourceTools", !selectedSource.isEmpty() && toolbarArea && toolbarArea->layout() && toolbarArea->layout()->count() > 0);
    QJsonArray transitions;
    obs_frontend_source_list list{};
    obs_frontend_get_transitions(&list);
    for (size_t i = 0; i < list.sources.num; ++i)
        transitions.append(QJsonObject{{"uuid", SourceId(list.sources.array[i])},
            {"name", QString::fromUtf8(obs_source_get_name(list.sources.array[i]))}});
    obs_frontend_source_list_free(&list);
    OBSSourceAutoRelease transition = obs_frontend_get_current_transition();
    state.insert("transitions", transitions);
    state.insert("currentTransition", SourceId(transition));
    state.insert("transitionDuration", obs_frontend_get_transition_duration());
    state.insert("transitionFixed", transition && obs_transition_fixed(transition));
    if (auto *combo = main->findChild<QComboBox *>(QStringLiteral("transitions")))
        state.insert("transitionEnabled", combo->isEnabled());
    QJsonArray transitionControls, quick;
    for (const auto &name : {"transitionAdd", "transitionRemove", "transitionProps"}) {
        if (auto *button = main->findChild<QAbstractButton *>(QLatin1String(name))) {
            QString label = OBSWeb::ControlLabel(button->text(), button->accessibleName(), button->toolTip());
            if (label.isEmpty())
                label = name == QByteArray("transitionAdd") ? QTStr("Add") :
                        name == QByteArray("transitionRemove") ? QTStr("Remove") : QTStr("Properties");
            transitionControls.append(QJsonObject{{"id", registerButton(button)}, {"text", label},
                                                   {"enabled", button->isEnabled()}, {"name", QLatin1String(name)}});
        }
    }
    for (auto *button : main->findChildren<QAbstractButton *>()) {
        if (button->property("id").toInt() > 0 && button->isVisibleTo(nativeCentral ? nativeCentral.data() : main))
            quick.append(QJsonObject{{"id", registerButton(button)}, {"text", button->text()},
                                     {"enabled", button->isEnabled()}});
    }
    state.insert("transitionControls", transitionControls);
    state.insert("quickTransitions", quick);
    if (auto *button = main->findChild<QAbstractButton *>(QStringLiteral("studioAddQuickTransition")))
        state.insert("addQuickTransition", registerButton(button));
    if (auto *tbar = main->findChild<QSlider *>(QStringLiteral("studioTBar")))
        state.insert("tbar", tbar->value());
    auto *basic = static_cast<OBSBasic *>(main);
    const auto program = basic->GetProgramSource();
    state.insert("programName", program ? QString::fromUtf8(obs_source_get_name(program)) : QString());
    state.insert("cpu", basic->GetCPUUsage());
    QJsonArray status;
    for (auto *label : main->statusBar()->findChildren<QLabel *>())
        if (label->isVisibleTo(main->statusBar()) && !label->text().isEmpty())
            status.append(label->text());
    state.insert("statusText", status);
    QJsonArray docks;
    for (auto *dock : main->findChildren<QDockWidget *>())
        docks.append(QJsonObject{{"name", dock->objectName()}, {"title", dock->windowTitle()},
            {"visible", !dock->isHidden()}, {"floating", dock->isFloating()},
            {"floatable", dock->features().testFlag(QDockWidget::DockWidgetFloatable)},
            {"action", registerAction(dock->toggleViewAction())}});
    state.insert("docks", docks);
    QJsonObject mixerToolbar;
    if (auto *mixer = main->findChild<AudioMixer *>()) {
        for (auto *button : mixer->findChildren<QPushButton *>()) {
            if (button->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts).contains("toggle-hidden"))
                mixerToolbar.insert("hidden", QJsonObject{{"id", registerButton(button)},
                    {"text", button->text()}, {"tooltip", button->toolTip()},
                    {"enabled", button->isEnabled()}, {"checked", button->isChecked()}});
            else if (button->menu() && button->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts).contains("toolbar-button"))
                mixerToolbar.insert("optionsText", button->text());
        }
        for (const auto *name : {"actionMixerToolbarToggleLayout", "actionMixerToolbarAdvAudio"})
            if (auto *action = mixer->findChild<QAction *>(QLatin1String(name))) {
                const auto key = QByteArray(name).endsWith("ToggleLayout") ? "layoutAction" : "advancedAction";
                mixerToolbar.insert(QLatin1String(key), registerAction(action));
            }
    }
    state.insert("mixerToolbar", mixerToolbar);
    QJsonObject preferences;
    preferences.insert("nativeDocking", property("webview2NativeDocking").toBool());
    for (const auto *name : {"toggleListboxToolbars", "toggleContextBar", "toggleSourceIcons", "toggleStatusBar",
                             "actionSceneGridMode", "lockDocks"})
        if (auto *action = main->findChild<QAction *>(QLatin1String(name)))
            preferences.insert(QLatin1String(name), action->isChecked());
    preferences.insert("verticalMixer", config_get_bool(App()->GetUserConfig(), "BasicWindow", "VerticalVolumeControl"));
    state.insert("workspace", preferences);
    auto appearance = state.value("appearance").toObject();
    const QFontInfo font(main->font());
    appearance.insert("fontFamily", font.family());
    appearance.insert("fontSize", font.pixelSize());
    state.insert("appearance", appearance);
}

bool executeWorkspace(const QJsonObject &message)
{
    const QPointer<OBSWebView2> guard(this);
    const QString id = message.value("id").toString(), command = message.value("command").toString();
    const auto args = message.value("args").toObject();
    auto invalid = [&](const QString &error) {
        reject(id, QStringLiteral("InvalidArgs"), error);
        return true;
    };
    if (command == QStringLiteral("dock.detach") || command == QStringLiteral("dock.attach")) {
        const auto name = args.value("name");
        auto *dock = name.isString() ? main->findChild<QDockWidget *>(name.toString()) : nullptr;
        auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
        if (!dock || !isWebDock(dock)) return invalid(QStringLiteral("Unknown workspace panel."));
        if ((lock && lock->isChecked()) || !dock->features().testFlag(QDockWidget::DockWidgetFloatable))
            return invalid(QStringLiteral("Unlock docks before moving workspace panels."));
        const bool floating = command == QStringLiteral("dock.detach");
        if (floating && !dock->isFloating()) {
            const auto bounded = [&](const char *key, double fallback, double minimum, double maximum) {
                const auto value = args.value(QLatin1String(key));
                return value.isDouble() && std::isfinite(value.toDouble()) ?
                    std::clamp(value.toDouble(), minimum, maximum) : fallback;
            };
            const double viewport = bounded("viewportWidth", browser->width(), 100.0, 20000.0);
            const double scale = browser->width() / viewport;
            const int panelWidth = int(bounded("width", 320.0, 180.0, 4000.0) * scale);
            const int panelHeight = int(bounded("height", 300.0, 180.0, 3000.0) * scale);
            const QPoint grabOffset(int(bounded("offsetX", 36.0, 0.0, 4000.0) * scale),
                                    int(bounded("offsetY", 16.0, 0.0, 3000.0) * scale));
            dock->setFloating(true);
            dock->resize(panelWidth, panelHeight);
            dock->move(QCursor::pos() - grabOffset);
            showWebDock(dock);
            dock->raise();
            dock->activateWindow();
        } else if (!floating) {
            dock->setFloating(false);
            dock->show();
            showWebDock(dock);
        }
        reply(id, QJsonObject{{"name", dock->objectName()}, {"floating", dock->isFloating()}});
        publishState(true);
        return true;
    }
    if (command.startsWith(QStringLiteral("audio."))) {
        QString error;
        if (!audio || !audio->execute(command, args, error))
            reject(id, QStringLiteral("Unavailable"), error);
        else {
            reply(id, QJsonObject{});
            publishState(true);
        }
        return true;
    }
    if (command == QStringLiteral("transition.select")) {
        const QString uuid = args.value("uuid").toString();
        bool found = false;
        obs_frontend_source_list list{};
        obs_frontend_get_transitions(&list);
        for (size_t i = 0; i < list.sources.num; ++i)
            found |= uuid == SourceId(list.sources.array[i]);
        obs_frontend_source_list_free(&list);
        if (!found)
            return invalid(QStringLiteral("Transition no longer exists."));
        static_cast<OBSBasic *>(main)->SetCurrentTransition(uuid);
        reply(id, QJsonObject{});
        publishState(true);
        return true;
    }
    if (command == QStringLiteral("transition.duration")) {
        const auto value = args.value("value");
        auto *spin = main->findChild<QSpinBox *>(QStringLiteral("transitionDuration"));
        if (!spin || !spin->isEnabled() || !value.isDouble() ||
            value.toDouble() != value.toInt(-1) || value.toInt() < spin->minimum() || value.toInt() > spin->maximum())
            return invalid(QStringLiteral("Invalid transition duration."));
        spin->setValue(value.toInt());
        reply(id, QJsonObject{});
        publishState(true);
        return true;
    }
    if (command == QStringLiteral("studio.tbar")) {
        auto *slider = main->findChild<QSlider *>(QStringLiteral("studioTBar"));
        const auto value = args.value("value");
        if (!obs_frontend_preview_program_mode_active() || !slider || !value.isDouble() ||
            value.toDouble() != value.toInt(-1) || value.toInt() < slider->minimum() || value.toInt() > slider->maximum())
            return invalid(QStringLiteral("Manual transition is not available."));
        slider->setValue(value.toInt());
        if (args.value("release").toBool())
            QMetaObject::invokeMethod(slider, "sliderReleased", Qt::DirectConnection);
        reply(id, QJsonObject{});
        publishState(true);
        return true;
    }
    const bool sourceCommand = command == "source.visibility" || command == "source.lock" ||
        command == "source.expand" || command == "source.rename" || command == "source.move";
    if (sourceCommand) {
        if (!validateContext(id, args))
            return true;
        const int row = sourceRow(args);
        auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
        if (row < 0 || !tree)
            return invalid(QStringLiteral("The source item no longer exists in this scene."));
        QPointer<SourceTreeItem> widget = tree->GetItemWidget(row);
        if (!widget)
            return invalid(QStringLiteral("The native source control is unavailable."));
        if (command == "source.rename") {
            const auto name = args.value("name");
            if (!name.isString() || name.toString().trimmed().isEmpty() || name.toString().size() > 170)
                return invalid(QStringLiteral("Enter a name between 1 and 170 characters."));
            if (!widget->IsEditing())
                QMetaObject::invokeMethod(widget, "EnterEditMode", Qt::DirectConnection);
            if (auto *edit = widget->findChild<QLineEdit *>())
                edit->setText(name.toString().trimmed());
            reply(id, QJsonObject{});
            QMetaObject::invokeMethod(widget, "ExitEditMode", Qt::DirectConnection, Q_ARG(bool, true));
        } else if (command == "source.move") {
            const int target = sourceRow(args.value("target").toObject());
            const QString position = args.value("position").toString();
            const int nativePosition = position == "inside" ? 0 : position == "before" ? 1 : position == "after" ? 2 : -1;
            if (target < 0 || nativePosition < 0 || !tree->MoveSelectedItems(target, nativePosition))
                return invalid(QStringLiteral("This source cannot be moved to the requested position."));
            reply(id, QJsonObject{});
        } else {
            if (!args.value("value").isBool())
                return invalid(QStringLiteral("Expected a boolean control value."));
            const bool value = args.value("value").toBool();
            QCheckBox *control = nullptr;
            for (auto *checkbox : widget->findChildren<QCheckBox *>()) {
                if ((command == "source.visibility" && checkbox->accessibleName() == QTStr("Basic.Main.Sources.Visibility")) ||
                    (command == "source.lock" && checkbox->accessibleName() == QTStr("Basic.Main.Sources.Lock")) ||
                    (command == "source.expand" && checkbox->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts).contains("indicator-expand")))
                    control = checkbox;
            }
            if (!control)
                return invalid(QStringLiteral("The native control is unavailable."));
            const bool checked = command == "source.expand" ? !value : value;
            if (control->isChecked() != checked)
                control->click();
            reply(id, QJsonObject{});
        }
        if (guard) publishState(true);
        return true;
    }
    if (command == "scene.rename" || command == "scene.move") {
        if (!validateContext(id, args))
            return true;
        auto *basic = static_cast<OBSBasic *>(main);
        if (args.value("uuid").toString() != SourceId(basic->GetCurrentSceneSource()))
            return invalid(QStringLiteral("Select the scene again."));
        if (command == "scene.rename") {
            const auto name = args.value("name");
            if (!name.isString() || name.toString().trimmed().isEmpty() || name.toString().size() > 170)
                return invalid(QStringLiteral("Enter a name between 1 and 170 characters."));
            QLineEdit editor;
            editor.setText(name.toString());
            reply(id, QJsonObject{});
            QMetaObject::invokeMethod(main, "SceneNameEdited", Qt::DirectConnection, Q_ARG(QWidget *, &editor));
        } else {
            obs_frontend_source_list scenes{};
            obs_frontend_get_scenes(&scenes);
            int from = -1, to = -1;
            for (size_t i = 0; i < scenes.sources.num; ++i) {
                if (SourceId(scenes.sources.array[i]) == args.value("uuid").toString()) from = int(i);
                if (SourceId(scenes.sources.array[i]) == args.value("target").toString()) to = int(i);
            }
            obs_frontend_source_list_free(&scenes);
            if (from < 0 || to < 0)
                return invalid(QStringLiteral("The scene no longer exists."));
            const char *slot = to > from ? "on_actionSceneDown_triggered" : "on_actionSceneUp_triggered";
            for (int i = 0; i < std::abs(to - from); ++i)
                QMetaObject::invokeMethod(main, slot, Qt::DirectConnection);
            reply(id, QJsonObject{});
        }
        if (guard) publishState(true);
        return true;
    }
    if (command == "source.interact") {
        OBSSourceAutoRelease source = obs_get_source_by_uuid(args.value("uuid").toString().toUtf8().constData());
        if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_INTERACTION))
            return invalid(QStringLiteral("This source does not support interaction."));
        reply(id, QJsonObject{});
        obs_frontend_open_source_interaction(source);
        return true;
    }
    if (command != "native.command")
        return false;
    if (!validateContext(id, args))
        return true;
    const QString operation = args.value("id").toString();
    auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
    static const QHash<QString, QByteArray> nativeSlots = {
        {"scene.duplicate", "DuplicateSelectedScene"},
        {"studio.transition", "TransitionClicked"}
    };
    if (nativeSlots.contains(operation)) {
        reply(id, QJsonObject{{"accepted", true}});
        QMetaObject::invokeMethod(main, nativeSlots.value(operation).constData(), Qt::DirectConnection);
    } else if (operation == "source.group" || operation == "source.ungroup" || operation == "source.addGroup") {
        if (!tree)
            return invalid(QStringLiteral("Sources are unavailable."));
        reply(id, QJsonObject{});
        if (operation == "source.group") tree->GroupSelectedItems();
        else if (operation == "source.ungroup") tree->UngroupSelectedGroups();
        else tree->AddGroup();
        // Native grouping queues its name editor and holds the undo stack until
        // that editor commits. Queue behind it to keep that transaction balanced.
        QTimer::singleShot(0, this, [this] { finishGroupName(); publishState(true); });
    } else if (operation == "source.tools") {
        auto *area = main->findChild<QWidget *>("emptySpace");
        if (!area || !area->layout() || !area->layout()->count())
            return invalid(QStringLiteral("This source has no additional toolbar."));
        QPointer<QWidget> controls = area->layout()->itemAt(0)->widget();
        if (!controls) return invalid(QStringLiteral("Source toolbar is unavailable."));
        reply(id, QJsonObject{});
        QDialog dialog(main);
        dialog.setWindowTitle(QTStr("Basic.Main.Sources"));
        dialog.resize(850, 130);
        auto *layout = new QVBoxLayout(&dialog);
        NativePlacement placement;
        placement.take(controls, &dialog);
        layout->addWidget(controls);
        controls->show();
        dialog.exec();
        if (!guard) return true;
        if (controls && area->layout()->count() == 0) placement.restore(controls);
        static_cast<OBSBasic *>(main)->UpdateContextBar(true);
    } else if (operation == "source.context") {
        const int row = sourceRow(args);
        if (row < 0)
            return invalid(QStringLiteral("Source no longer exists."));
        reply(id, QJsonObject{});
        static_cast<OBSBasic *>(main)->CreateSourcePopupMenu(row, false);
    } else if (operation == "scene.context") {
        auto *scenes = main->findChild<QListWidget *>(QStringLiteral("scenes"));
        if (!scenes || !scenes->currentItem())
            return invalid(QStringLiteral("Scene is unavailable."));
        reply(id, QJsonObject{});
        QMetaObject::invokeMethod(main, "on_scenes_customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, scenes->visualItemRect(scenes->currentItem()).center()));
    } else if (operation == "audio.context") {
        VolumeControl *selected = nullptr;
        for (auto *control : main->findChildren<VolumeControl *>())
            if (SourceId(OBSGetStrongRef(control->weakSource())) == args.value("uuid").toString())
                selected = control;
        if (!selected)
            return invalid(QStringLiteral("Audio source is unavailable."));
        reply(id, QJsonObject{});
        const QPoint point = selected->rect().center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, QCursor::pos());
        QApplication::sendEvent(selected, &event);
    } else if (operation == "audio.options") {
        auto *mixer = main->findChild<AudioMixer *>();
        if (!mixer) return invalid(QStringLiteral("Audio mixer is unavailable."));
        reply(id, QJsonObject{});
        mixer->showMixerContextMenu();
    } else if (operation == "studio.quick.options") {
        auto *button = qobject_cast<QPushButton *>(liveButtons.value(args.value("button").toString()).data());
        if (!button || button->property("id").toInt() <= 0 || !button->menu())
            return invalid(QStringLiteral("Quick transition is unavailable."));
        reply(id, QJsonObject{});
        button->menu()->popup(QCursor::pos());
    } else if (operation == "studio.options") {
        auto *button = main->findChild<QAbstractButton *>(QStringLiteral("studioConfigTransitions"));
        if (!button)
            return invalid(QStringLiteral("Studio Mode is not active."));
        reply(id, QJsonObject{});
        button->click();
    } else if (operation == "window.close") {
        reply(id, QJsonObject{});
        QTimer::singleShot(0, main, &QWidget::close);
    } else {
        return invalid(QStringLiteral("Unknown workspace command."));
    }
    if (guard) publishState(true);
    return true;
}
