// Keep the original OBS preview controls authoritative even while their Qt
// container is parked behind the WebView workspace.
bool previewRequestedEnabled() const
{
    // The frontend API reads OBSBasic::previewEnabled, independently of the
    // swap chain. Studio Mode temporarily requires the preview even when the
    // saved single-view preference is off. A not-yet-exposed HWND has no display.
    return obs_frontend_preview_enabled() || obs_frontend_preview_program_mode_active();
}

void addPreviewState(QJsonObject &state)
{
    QJsonObject controls{{"enabled", previewRequestedEnabled()}, {"displayReady", preview && preview->GetDisplay() != nullptr},
        {"hostWidth", browser->width()}, {"hostHeight", browser->height()}};
    if (auto *button = main->findChild<QAbstractButton *>(QStringLiteral("enablePreviewButton")))
        controls.insert("enableText", button->text());
    if (auto *label = main->findChild<QLabel *>(QStringLiteral("previewScalePercent")))
        controls.insert("percent", label->text());
    if (auto *combo = main->findChild<QComboBox *>(QStringLiteral("previewScalingMode"))) {
        QJsonArray options;
        for (int i = 0; i < combo->count(); ++i)
            options.append(QJsonObject{{"index", i}, {"text", combo->itemText(i)}});
        controls.insert("options", options);
        controls.insert("index", combo->currentIndex());
        controls.insert("placeholder", combo->placeholderText());
    }
    auto *editor = qobject_cast<OBSBasicPreview *>(preview);
    controls.insert("fixed", editor && editor->IsFixedScaling());
    for (const auto *name : {"previewXScrollBar", "previewYScrollBar"}) {
        if (auto *bar = main->findChild<QScrollBar *>(QLatin1String(name)))
            controls.insert(QLatin1String(name), QJsonObject{{"min", bar->minimum()}, {"max", bar->maximum()},
                {"value", bar->value()}, {"page", bar->pageStep()}, {"step", bar->singleStep()},
                {"extent", bar->orientation() == Qt::Horizontal ? bar->height() : bar->width()}});
    }
    state.insert("previewControls", controls);
}

bool executePreview(const QJsonObject &message)
{
    const auto command = message.value("command").toString();
    if (command != "preview.enable" && command != "preview.scale" && command != "preview.scroll" &&
        command != "preview.context") return false;
    const auto id = message.value("id").toString();
    const auto args = message.value("args").toObject();
    if (!workspaceMounted || !surfacesBorrowed) {
        reject(id, "Unavailable", "The WebView preview is not active."); return true;
    }
    if (command == "preview.enable") {
        // Enable, rather than toggle: a repeated command or a suspended/missing
        // display must never invert an already enabled native preference.
        if (!previewRequestedEnabled()) obs_frontend_set_preview_enabled(true);
    } else if (command == "preview.context") {
        reply(id, QJsonObject{});
        QMetaObject::invokeMethod(main, "on_previewDisabledWidget_customContextMenuRequested", Qt::DirectConnection);
        return true;
    } else if (command == "preview.scale") {
        auto *combo = main->findChild<QComboBox *>(QStringLiteral("previewScalingMode"));
        const auto value = args.value("index");
        if (!combo || !combo->isEnabled() || !value.isDouble() || value.toDouble() != value.toInt(-1) ||
            value.toInt() < 0 || value.toInt() >= combo->count()) {
            reject(id, "InvalidArgs", "Unknown preview scaling option."); return true;
        }
        combo->setCurrentIndex(value.toInt());
    } else {
        const auto axis = args.value("axis").toString();
        auto *bar = main->findChild<QScrollBar *>(axis == "x" ? QStringLiteral("previewXScrollBar") :
                                                axis == "y" ? QStringLiteral("previewYScrollBar") : QStringLiteral("invalidPreviewAxis"));
        const auto value = args.value("value");
        auto *editor = qobject_cast<OBSBasicPreview *>(preview);
        if (!bar || !editor || !editor->IsFixedScaling() || !value.isDouble() || !std::isfinite(value.toDouble()) ||
            value.toDouble() != value.toInt(INT_MIN) || value.toInt() < bar->minimum() || value.toInt() > bar->maximum()) {
            reject(id, "InvalidArgs", "Invalid preview scroll position."); return true;
        }
        // The horizontal native connection listens to sliderMoved; the vertical
        // one to valueChanged. Preserve both original handlers.
        bar->setValue(value.toInt());
        if (axis == "x") QMetaObject::invokeMethod(bar, "sliderMoved", Qt::DirectConnection, Q_ARG(int, value.toInt()));
    }
    publishState(true);
    reply(id, QJsonObject{});
    return true;
}
