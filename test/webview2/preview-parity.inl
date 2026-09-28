void runPreviewParityChecks(const std::function<void(bool, const char *)> &check,
                            const std::function<void()> &done)
{
    auto *basic = static_cast<OBSBasic *>(main);
    auto *editor = qobject_cast<OBSBasicPreview *>(preview);
    auto *combo = main->findChild<QComboBox *>("previewScalingMode");
    auto *x = main->findChild<QScrollBar *>("previewXScrollBar");
    auto *y = main->findChild<QScrollBar *>("previewYScrollBar");
    if (!editor || !combo || !x || !y) { check(false, "Original preview controllers exist"); done(); return; }
    const bool studio = obs_frontend_preview_program_mode_active();
    obs_frontend_set_preview_program_mode(false);
    const bool enabled = obs_frontend_preview_enabled();
    const bool fixed = editor->IsFixedScaling();
    const auto amount = editor->GetScalingAmount();
    const auto level = editor->GetScalingLevel();
    const auto oldX = editor->GetScrollX(), oldY = editor->GetScrollY();
    QMetaObject::invokeMethod(basic, "DisablePreview", Qt::DirectConnection);
    {
        // Exercise the production state exporter with a fresh, never-exposed
        // widget. Do not destroy or replace the application's live GPU display.
        OBSQTDisplay unexposed;
        check(unexposed.GetDisplay() == nullptr, "Startup fixture has no exposed HWND or GPU display");
        auto *livePreview = preview;
        preview = &unexposed;
        QJsonObject initial;
        addPreviewState(initial);
        check(!initial.value("previewControls").toObject().value("enabled").toBool(),
              "An absent display preserves the native disabled preview preference");
        obs_frontend_set_preview_enabled(true);
        initial = {};
        addPreviewState(initial);
        check(initial.value("previewControls").toObject().value("enabled").toBool() &&
                  !initial.value("previewControls").toObject().value("displayReady").toBool(),
              "Enabled preview remains requested before its first GPU display exists");
        preview = livePreview;
        obs_frontend_set_preview_enabled(false);
    }
    check(!snapshot().value("previewControls").toObject().value("enabled").toBool(),
          "Disabled native preview exports the Enable Preview state");
    const auto r = preview->geometry().translated(-browser->pos());
    execute({{"id", "preview-check"}, {"command", "preview.bounds"}, {"args", QJsonObject{
        {"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()},
        {"viewportWidth", browser->width()}, {"viewportHeight", browser->height()}, {"visible", true}}}});
    check(preview->isHidden() && !obs_display_enabled(preview->GetDisplay()),
          "A pending HTML geometry update cannot resurrect a disabled native preview");
    execute({{"id", "preview-check"}, {"command", "preview.enable"}});
    check(obs_display_enabled(preview->GetDisplay()), "HTML Enable Preview invokes the original native toggle");
    execute({{"id", "preview-check"}, {"command", "preview.enable"}});
    check(obs_frontend_preview_enabled(), "Repeated Enable Preview commands cannot toggle the native preference off");
    execute({{"id", "preview-check"}, {"command", "preview.scale"}, {"args", QJsonObject{{"index", 1}}}});
    editor->ClampScrollingOffsets();
    const auto state = snapshot().value("previewControls").toObject();
    check(editor->IsFixedScaling() && editor->GetScalingAmount() == 1.0f && combo->currentIndex() == 1,
          "HTML scaling choice uses the original OBS canvas scale controller");
    check(state.value("options").toArray().size() == combo->count() &&
              state.value("options").toArray().at(1).toObject().value("text").toString() == combo->itemText(1),
          "Preview scaling options retain native resolution labels and omit unavailable output scale");
    check(x->maximum() > x->minimum() && y->maximum() > y->minimum(),
          "Parked native scrollbars still calculate real canvas pan ranges");
    const int scrollX = x->maximum() / 3, scrollY = y->minimum() / 3;
    execute({{"id", "preview-check"}, {"command", "preview.scroll"}, {"args", QJsonObject{{"axis", "x"}, {"value", scrollX}}}});
    execute({{"id", "preview-check"}, {"command", "preview.scroll"}, {"args", QJsonObject{{"axis", "y"}, {"value", scrollY}}}});
    check(editor->GetScrollX() == -float(scrollX) && editor->GetScrollY() == -float(scrollY),
          "Both HTML scroll axes change the original preview scrolling offsets");
    execute({{"id", "preview-check"}, {"command", "preview.scroll"}, {"args", QJsonObject{{"axis", "x"}, {"value", x->maximum() + 1000}}}});
    check(!lastTestReply.value("ok").toBool() && editor->GetScrollX() == -float(scrollX),
          "Out of range preview pan is rejected without changing the canvas");
    execute({{"id", "preview-check"}, {"command", "preview.scale"}, {"args", QJsonObject{{"index", 0}}}});
    check(!editor->IsFixedScaling() && x->minimum() == x->maximum() && y->minimum() == y->maximum(),
          "Scale to Window restores native non-scrollable fit mode");
    editor->SetFixedScaling(fixed);
    editor->SetScalingLevelAndAmount(level, amount);
    editor->SetScrollingOffset(oldX, oldY);
    if (!enabled) QMetaObject::invokeMethod(basic, "DisablePreview", Qt::DirectConnection);
    obs_frontend_set_preview_program_mode(studio);
    publishState(true);
    QTimer::singleShot(250, this, done);
}
