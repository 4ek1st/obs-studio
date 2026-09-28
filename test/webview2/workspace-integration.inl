// Member of OBSWebView2; only compiled for the disposable portable test build.
void runWorkspaceChecks(std::function<void(bool, const char *)> check, std::function<void()> done)
{
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
                if (stats->toggleViewAction()->isChecked()) stats->toggleViewAction()->trigger();
                stats->toggleViewAction()->trigger();
                check(stats->isVisible() && stats->isFloating(), "Additional native dock opens from WebView2 while the old workspace is hidden");
                stats->hide();
            } else check(false, "Stats dock is available");
            const auto home = previewPlacement.parent;
            const auto homeLayout = previewPlacement.layout;
            const auto row = previewPlacement.row, column = previewPlacement.column;
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
                    QTimer::singleShot(250, this, [this, artifacts, check, done] {
                        browser->capturePreview(QDir(artifacts).filePath("workspace.png"), [check, done](bool saved) {
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
