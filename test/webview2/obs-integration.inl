// Compiled only with ENABLE_WEBVIEW2_INTEGRATION_TESTS. Disposable portable data only.
void runIntegrationChecks()
{
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-lifecycle-only"))) {
		blog(LOG_INFO, "[WebView2 test] Lifecycle-only test");
		QTimer::singleShot(2000, this, &QWidget::close);
		return;
	}
	// A previous portable test saves its enlarged window geometry.
	resize(1280, 840);
	struct Fixture {
		OBSSceneAutoRelease first{obs_scene_create("WebView2 integration A")};
		OBSSceneAutoRelease second{obs_scene_create("WebView2 integration B")};
		OBSSourceAutoRelease a{obs_source_create("color_source_v3", "WebView2 source A", nullptr, nullptr)};
		OBSSourceAutoRelease b{obs_source_create("color_source_v3", "WebView2 source B", nullptr, nullptr)};
		obs_sceneitem_t *itemA = nullptr;
		obs_sceneitem_t *itemB = nullptr;
        obs_sceneitem_t *groupChild = nullptr;
        obs_sceneitem_t *otherChild = nullptr;
		QJsonArray checks;
	};
	auto fixture = std::make_shared<Fixture>();
	auto check = [fixture](bool passed, const char *name) {
		fixture->checks.append(QJsonObject{{"name", QString::fromUtf8(name)}, {"passed", passed}});
		blog(passed ? LOG_INFO : LOG_ERROR, "[WebView2 test] %s: %s", passed ? "PASS" : "FAIL", name);
	};
	check(QApplication::activeModalWidget() == nullptr, "Core-only startup has no plugin error dialog");
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-capture-test"))) {
		RunCaptureDialogChecks(static_cast<OBSBasic *>(main), check, [this, fixture] {
			QFile report(QDir(qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS")).filePath("capture-report.json"));
			if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(fixture->checks).toJson());
			QTimer::singleShot(200, this, &QWidget::close);
		});
		return;
	}
	if (!fixture->first || !fixture->second || !fixture->a || !fixture->b) {
		blog(LOG_ERROR, "[WebView2 test] Could not create fixtures");
		return;
	}
	fixture->itemA = obs_scene_add(fixture->first, fixture->a);
	fixture->itemB = obs_scene_add(fixture->first, fixture->b);
	auto *group = obs_scene_add_group(fixture->first, "WebView2 group");
	fixture->otherChild = obs_scene_add(obs_sceneitem_group_get_scene(group), fixture->a);
	fixture->groupChild = obs_scene_add(obs_sceneitem_group_get_scene(group), fixture->b);
	check(obs_sceneitem_get_id(fixture->groupChild) == obs_sceneitem_get_id(fixture->itemB),
	      "Group collision fixture uses equal IDs in different owner scenes");
	obs_frontend_set_current_scene(obs_scene_get_source(fixture->first));
	QTimer::singleShot(400, this, [this, fixture, check] {
		auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
		check(tree != nullptr, "Native SourceTree is available");
		check(reportedPreviewGeometry, "Actual WebView2 document delivered viewport geometry");
		bool allLabels = true;
		for (const auto &value : snapshot().value("controls").toArray())
			allLabels &= !value.toObject().value("text").toString().trimmed().isEmpty();
		check(allLabels, "All native controls including icon buttons have visible labels");
		const auto workspace = snapshot();
		check(workspace.value("sceneToolbar").toArray().size() >= 4,
		      "Scene creation and editing toolbar is exposed to WebView2");
		check(workspace.value("sourceToolbar").toArray().size() >= 4,
		      "Source creation and editing toolbar is exposed to WebView2");
		check(workspace.value("transitions").toArray().size() >= 2,
		      "Built-in scene transitions are exposed to WebView2");
		check(workspace.value("nativeEditor").toBool(),
		      "WebView2 uses the actual editable OBS preview surface");
		if (!tree)
			return;
		tree->selectionModel()->clearSelection();
		tree->SelectItem(fixture->itemA, true);
		const auto firstId = SourceId(obs_scene_get_source(fixture->first));
		auto send = [this](const char *command, QJsonObject args) {
			execute(QJsonObject{{"id", "integration"}, {"command", QString::fromUtf8(command)}, {"args", args}});
		};
		send("source.select", QJsonObject{{"scene", firstId}, {"id", QString::number(obs_sceneitem_get_id(fixture->itemB))},
						 {"uuid", SourceId(fixture->b)}});
		check(lastTestReply.value("ok").toBool() && obs_sceneitem_selected(fixture->itemB) &&
			      !obs_sceneitem_selected(fixture->itemA) && !obs_sceneitem_selected(fixture->groupChild),
		      "Web selection updates native selection despite same-ID shared source inside group");
		auto *rotate = main->findChild<QAction *>(QStringLiteral("actionRotate90CW"));
		check(rotate != nullptr, "Native Rotate action exists");
		if (rotate) {
			send("action.invoke", QJsonObject{{"id", registerAction(rotate)}, {"context", snapshot().value("context")}});
			check(obs_sceneitem_get_rot(fixture->itemB) == 90.0f && obs_sceneitem_get_rot(fixture->itemA) == 0.0f,
			      "Rotate acts on the selected source before the next queued command");
		}
		tree->selectionModel()->clearSelection();
		tree->SelectItem(fixture->otherChild, true);
		const auto groupContext = snapshot().value("context");
		tree->selectionModel()->clearSelection();
		tree->SelectItem(fixture->groupChild, true);
		check(groupContext != snapshot().value("context"), "Context includes selection inside groups");
		if (rotate) {
			send("action.invoke", QJsonObject{{"id", registerAction(rotate)}, {"context", groupContext}});
			check(lastTestReply.value("error").toObject().value("code") == "StaleContext" &&
				      obs_sceneitem_get_rot(fixture->groupChild) == 0.0f && obs_sceneitem_get_rot(fixture->otherChild) == 0.0f,
			      "Changed group selection rejects an old action");
		}
		tree->selectionModel()->clearSelection();
		tree->SelectItem(fixture->itemB, true);
		const auto stale = snapshot().value("context");
		obs_frontend_set_current_scene(obs_scene_get_source(fixture->second));
		if (rotate) {
			send("action.invoke", QJsonObject{{"id", registerAction(rotate)}, {"context", stale}});
			check(lastTestReply.value("error").toObject().value("code") == "StaleContext",
			      "Scene change rejects an action from an old snapshot");
		}
		const bool beforeStudio = obs_frontend_preview_program_mode_active();
		send("control.click", QJsonObject{{"id", "modeSwitch"}, {"context", stale}});
		check(lastTestReply.value("error").toObject().value("code") == "StaleContext" &&
			      beforeStudio == obs_frontend_preview_program_mode_active(),
		      "Stale control cannot change studio mode");
		send("source.select", QJsonObject{{"scene", firstId}, {"id", QString::number(obs_sceneitem_get_id(fixture->itemB))},
						 {"uuid", SourceId(fixture->b)}});
		check(!lastTestReply.value("ok").toBool(), "Source selection rejects the wrong scene");
		send("scene.select", QJsonObject{{"uuid", firstId}});
		check(snapshot().value("currentScene") == firstId, "Scene command changes actual OBS scene");
		send("scene.select", QJsonObject{{"uuid", "missing-scene"}});
		check(!lastTestReply.value("ok").toBool(), "Missing scene returns a structured error");
		if (auto *menu = main->findChild<QMenu *>(QStringLiteral("scalingMenu"))) {
			send("menu.prepare", QJsonObject{{"id", registerAction(menu->menuAction())}});
			auto *canvas = main->findChild<QAction *>(QStringLiteral("actionScaleCanvas"));
			check(lastTestReply.value("ok").toBool() && canvas && !canvas->text().contains(QStringLiteral("%1")),
			      "Menu preparation refreshes native dynamic labels");
		} else {
			check(false, "Native scaling menu exists");
		}
		send("test.unsupported", QJsonObject{});
		check(!lastTestReply.value("ok").toBool(), "Unknown command returns a structured error");
		send("control.click", QJsonObject{{"id", "modeSwitch"}, {"context", snapshot().value("context")}});
		check(obs_frontend_preview_program_mode_active() != beforeStudio, "Control changes actual OBS studio mode");
		obs_frontend_set_preview_program_mode(beforeStudio);
		const auto previousPreview = preview->geometry();
		resize(2048, 1136);
		QTimer::singleShot(500, this, [this, fixture, check, previousPreview] {
			check(preview->width() > previousPreview.width(), "Native preview grows after the real WebView2 window resize");
			const QRect browserRect = browser->geometry();
			const QRect previewRect = preview->geometry();
			const int leftMargin = previewRect.left() - browserRect.left();
			const int rightMargin = browserRect.right() - previewRect.right();
			check(browserRect.contains(previewRect) && std::abs(leftMargin - rightMargin) <= 1 &&
				      previewRect.width() > browserRect.width() * 0.95,
			      "Resized preview fills its intended width with symmetric margins");
			check(obs_sceneitem_get_rot(fixture->itemA) == 0.0f && obs_sceneitem_get_rot(fixture->itemB) == 90.0f,
			      "No old rotate remains queued after scene changes");
			class DeclineClose final : public QObject {
			public:
				explicit DeclineClose(QObject *parent) : QObject(parent) {}
				bool eventFilter(QObject *, QEvent *event) override
				{
					if (event->type() != QEvent::Close)
						return false;
					static_cast<QCloseEvent *>(event)->ignore();
					return true;
				}
			};
			auto *blocker = new DeclineClose(main);
			main->installEventFilter(blocker);
			close();
			QTimer::singleShot(100, this, [this, fixture, check, blocker] {
				check(isVisible(), "Declining native shutdown keeps the WebView2 window usable");
				main->removeEventFilter(blocker);
				delete blocker;
				RunAudioMixerIntegrationChecks(main, check);
				RunAudioMixerVisualChecks(static_cast<OBSBasic *>(main), browser, [this] { publishState(true); }, check, [this, fixture, check] {
				runWorkspaceChecks(check, [this, fixture, check] {
				RunDialogWorkflowChecks(static_cast<OBSBasic *>(main), check, [this, fixture, check] {
				RunOutputWorkflowChecks(static_cast<OBSBasic *>(main), check, [this, fixture, check] {
				obs_sceneitem_set_visible(fixture->itemA, false);
				obs_sceneitem_set_locked(fixture->itemA, true);
				savePersistenceFixture(check);
				BPtr<char> path = GetAppConfigPathPtr("obs-studio/webview2-integration.json");
				QFile report(QString::fromUtf8(path.Get()));
				if (report.open(QIODevice::WriteOnly))
					report.write(QJsonDocument(QJsonObject{{"checks", fixture->checks}}).toJson());
				config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
				QTimer::singleShot(0, this, &QWidget::close);
				});
				});
				});
				});
			});
		});
	});
}
