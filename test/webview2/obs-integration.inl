// Compiled only with ENABLE_WEBVIEW2_INTEGRATION_TESTS. Disposable portable data only.
void runIntegrationChecks()
{
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-lifecycle-only"))) {
		blog(LOG_INFO, "[WebView2 test] Lifecycle-only test");
		QTimer::singleShot(2000, main, &QWidget::close);
		return;
	}
	// A previous portable test saves its enlarged window geometry.
	main->resize(1280, 840);
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
	check(main->isVisible() && main->centralWidget() == this && !isWindow(),
	      "WebView2 occupies the visible original OBS main window central widget");
	check(main->property("webview2NativeDocking").toBool() &&
	              main->dockOptions().testFlag(QMainWindow::AllowNestedDocks) &&
	              main->dockOptions().testFlag(QMainWindow::AllowTabbedDocks),
	      "Original OBS shell retains nested and tabbed native docking");
	const auto localizedState = snapshot();
	const auto localizedLabels = localizedState.value("labels").toObject();
	check(localizedState.value("locale").toString() == QString::fromUtf8(App()->GetLocale()) &&
	              localizedLabels.value("Basic.Main.Scenes").toString() == QTStr("Basic.Main.Scenes") &&
	              localizedLabels.value("WebView2.EmptyAudio").toString() == QTStr("WebView2.EmptyAudio") &&
	              !localizedLabels.value("WebView2.EmptyAudio").toString().isEmpty(),
	      "WebView2 receives the selected OBS locale with native and extension translations");
	const auto artifactDirectory = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
	if (!artifactDirectory.isEmpty()) {
		QDir().mkpath(artifactDirectory);
		QFile localeReport(QDir(artifactDirectory).filePath("localization-snapshot.json"));
		if (localeReport.open(QIODevice::WriteOnly))
			localeReport.write(QJsonDocument(QJsonObject{{"locale", localizedState.value("locale")},
				{"labels", localizedLabels}}).toJson());
	}
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-dock-tabs-test"))) {
		runDockTabsChecks(check, [this, fixture, check] {
			runDockTabDetachChecks(check, [this, fixture] {
				runFloatingGroupChromeChecks([fixture](bool passed, const char *name) {
					fixture->checks.append(QJsonObject{{"name", QString::fromUtf8(name)}, {"passed", passed}});
					blog(passed ? LOG_INFO : LOG_ERROR, "[WebView2 test] %s: %s", passed ? "PASS" : "FAIL", name);
				}, [this, fixture] {
				runWindowFrameChecks([fixture](bool passed, const char *name) {
					fixture->checks.append(QJsonObject{{"name", QString::fromUtf8(name)}, {"passed", passed}});
					blog(passed ? LOG_INFO : LOG_ERROR, "[WebView2 test] %s: %s", passed ? "PASS" : "FAIL", name);
				}, [this, fixture] {
					QFile report(QDir(qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS")).filePath("dock-tabs-report.json"));
					if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(fixture->checks).toJson());
					config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
					QTimer::singleShot(0, main, &QWidget::close);
				});
				});
			});
		});
		return;
	}
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-capture-test"))) {
		RunCaptureDialogChecks(static_cast<OBSBasic *>(main), check, [this, fixture] {
			QFile report(QDir(qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS")).filePath("capture-report.json"));
			if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(fixture->checks).toJson());
			QTimer::singleShot(200, main, &QWidget::close);
		});
		return;
	}
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2-window-drop-test"))) {
		RunWindowDropChecks(static_cast<OBSBasic *>(main), preview, check, [this, fixture] {
			QFile report(QDir(qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS")).filePath("window-drop-report.json"));
			if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(fixture->checks).toJson());
			config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
			QTimer::singleShot(200, main, &QWidget::close);
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
	// A fresh Chromium profile can finish navigation before its first layout
	// message reaches Qt. Wait for that actual production event, not a fixed
	// startup delay; the unchanged assertion below still fails after 5 seconds.
	auto *readinessTimer = new QTimer(this);
	readinessTimer->setInterval(25);
	auto readinessElapsed = std::make_shared<QElapsedTimer>();
	readinessElapsed->start();
	connect(readinessTimer, &QTimer::timeout, this, [this, fixture, check, readinessTimer, readinessElapsed] {
		if (!reportedPreviewGeometry && readinessElapsed->elapsed() < 5000) return;
		readinessTimer->stop();
		readinessTimer->deleteLater();
		blog(LOG_INFO, "[WebView2 test] Initial viewport readiness after %lld ms: %s",
		     static_cast<long long>(readinessElapsed->elapsed()), reportedPreviewGeometry ? "received" : "timed out");
		auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
		check(tree != nullptr, "Native SourceTree is available");
		check(reportedPreviewGeometry, "Actual WebView2 document delivered viewport geometry");
		bool allLabels = true;
		for (const auto &value : snapshot().value("controls").toArray())
			allLabels &= !value.toObject().value("text").toString().trimmed().isEmpty();
		check(allLabels, "All native controls including icon buttons have visible labels");
		const auto workspace = snapshot();
		QJsonArray redoShortcuts;
		for (const auto &value : workspace.value("actions").toArray()) {
			const auto action = value.toObject();
			if (action.value("name") == QStringLiteral("actionMainRedo"))
				redoShortcuts = action.value("shortcutKeys").toArray();
		}
		check(redoShortcuts.contains(QStringLiteral("Ctrl+Y")) &&
		              redoShortcuts.contains(QStringLiteral("Ctrl+Shift+Z")),
		      "Native Redo snapshot exposes both Ctrl+Y and Ctrl+Shift+Z");
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
		if (nativeCentral && width() >= 740) {
			const auto parkedSize = nativeCentral->size();
			nativeCentral->resize(420, parkedSize.height());
			static_cast<OBSBasic *>(main)->UpdateContextBarVisibility();
			static_cast<OBSBasic *>(main)->UpdateContextBar(true);
			check(snapshot().value("sourceTools").toBool(),
			      "Source toolbar availability follows the visible central workspace instead of the parked native view");
			nativeCentral->resize(parkedSize);
		}
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
			auto *undo = main->findChild<QAction *>(QStringLiteral("actionMainUndo"));
			auto *redo = main->findChild<QAction *>(QStringLiteral("actionMainRedo"));
			check(undo && redo && undo->isEnabled(), "Rotate enables the native Undo action exposed to the browser");
			if (undo && redo) {
				send("action.invoke", QJsonObject{{"id", registerAction(undo)}, {"context", snapshot().value("context")}});
				check(lastTestReply.value("ok").toBool() && obs_sceneitem_get_rot(fixture->itemB) == 0.0f && redo->isEnabled(),
				      "Production action route invokes native Undo and enables Redo");
				send("action.invoke", QJsonObject{{"id", registerAction(redo)}, {"context", snapshot().value("context")}});
				check(lastTestReply.value("ok").toBool() && obs_sceneitem_get_rot(fixture->itemB) == 90.0f,
				      "Production action route invokes native Redo on the original source");
			}
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
		auto resizedCanvas = std::make_shared<QJsonObject>();
		const auto resizedBoundsConnection = connect(browser, &WebView2Widget::messageReceived, this,
			[resizedCanvas](const QJsonObject &message) {
				const auto args = message.value("args").toObject();
				if (message.value("command") == "preview.bounds" && args.value("target") == "preview")
					*resizedCanvas = args;
				else if (message.value("command") == "preview.layout") {
					for (const auto &entry : args.value("surfaces").toArray()) {
						const auto surface = entry.toObject();
						if (surface.value("target") != "preview") continue;
						*resizedCanvas = surface;
						resizedCanvas->insert("viewportWidth", args.value("viewportWidth"));
						resizedCanvas->insert("viewportHeight", args.value("viewportHeight"));
					}
				}
			});
		main->resize(2048, 1136);
		QTimer::singleShot(500, this, [this, fixture, check, previousPreview, resizedCanvas, resizedBoundsConnection] {
			disconnect(resizedBoundsConnection);
			check(preview->width() > previousPreview.width(), "Native preview grows after the real WebView2 window resize");
			const QRect browserRect = browser->geometry();
			const QRect previewRect = preview->geometry();
			const int leftMargin = previewRect.left() - browserRect.left();
			const int rightMargin = browserRect.right() - previewRect.right();
			const double viewportWidth = resizedCanvas->value("viewportWidth").toDouble();
			const double viewportHeight = resizedCanvas->value("viewportHeight").toDouble();
			const double cssScaleX = viewportWidth > 0 ? browserRect.width() / viewportWidth : 0;
			const double cssScaleY = viewportHeight > 0 ? browserRect.height() / viewportHeight : 0;
			// Production #preview-grid reserves the original native scrollbar's
			// thickness, including Fit mode. The .preview-area has 8px padding
			// on each side. Compare margins around canvas + scrollbar, not around
			// the canvas alone. These exact expectations also catch doubled DPI,
			// stale viewport dimensions, wrong origins and an undersized canvas.
			const auto nativeBar = main->findChild<QScrollBar *>(QStringLiteral("previewYScrollBar"));
			const double scrollbarWidth = nativeBar ? nativeBar->width() : 0;
			const double outerMargin = 8 * cssScaleX;
			const bool matchesDeliveredCanvas = viewportWidth > 0 && viewportHeight > 0 &&
				std::abs(leftMargin - resizedCanvas->value("x").toDouble() * cssScaleX) <= 1 &&
				std::abs(previewRect.y() - browserRect.y() - resizedCanvas->value("y").toDouble() * cssScaleY) <= 1 &&
				std::abs(previewRect.width() - resizedCanvas->value("width").toDouble() * cssScaleX) <= 1 &&
				std::abs(previewRect.height() - resizedCanvas->value("height").toDouble() * cssScaleY) <= 1;
			blog(LOG_INFO, "[WebView2 test] Resized canvas host=%dx%d CSS=%.2fx%.2f rect=%d,%d %dx%d margins=%d,%d scrollbar=%.2f",
			     browserRect.width(), browserRect.height(), viewportWidth, viewportHeight,
			     previewRect.x(), previewRect.y(), previewRect.width(), previewRect.height(), leftMargin, rightMargin, scrollbarWidth);
			check(nativeBar && scrollbarWidth > 0 && matchesDeliveredCanvas && browserRect.contains(previewRect) &&
				      std::abs(leftMargin - outerMargin) <= 1 &&
				      std::abs(rightMargin - scrollbarWidth - outerMargin) <= 1 &&
				      std::abs(leftMargin - (rightMargin - scrollbarWidth)) <= 1 &&
				      previewRect.width() > browserRect.width() * 0.95,
			      "Resized native canvas matches delivered HTML bounds and symmetric outer margins including the right scrollbar");
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
			main->close();
			QTimer::singleShot(100, this, [this, fixture, check, blocker] {
				check(main->isVisible() && isVisible() && main->centralWidget() == this,
				      "Declining native shutdown keeps the original OBS shell and WebView2 usable");
				main->removeEventFilter(blocker);
				delete blocker;
				RunAudioMixerIntegrationChecks(main, check);
				auto *mixerDock = main->findChild<QDockWidget *>(QStringLiteral("mixerDock"));
				RunAudioMixerVisualChecks(static_cast<OBSBasic *>(main), webDockViews.value(mixerDock), [this] { publishState(true); }, check, [this, fixture, check] {
				runWorkspaceChecks(check, [this, fixture, check] {
				runPreviewParityChecks(check, [this, fixture, check] {
				RunDialogWorkflowChecks(static_cast<OBSBasic *>(main), check, [this, fixture, check] {
				RunOutputWorkflowChecks(static_cast<OBSBasic *>(main), check, [this, fixture, check] {
				auto *controlsDock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
				RunReplayOutputWorkflowChecks(static_cast<OBSBasic *>(main), webDockViews.value(controlsDock), [this] { publishState(true); }, check, [this, fixture, check] {
				obs_sceneitem_set_visible(fixture->itemA, false);
				obs_sceneitem_set_locked(fixture->itemA, true);
				savePersistenceFixture(check);
				BPtr<char> path = GetAppConfigPathPtr("obs-studio/webview2-integration.json");
				QFile report(QString::fromUtf8(path.Get()));
				if (report.open(QIODevice::WriteOnly))
					report.write(QJsonDocument(QJsonObject{{"checks", fixture->checks}}).toJson());
				config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
				QTimer::singleShot(0, main, &QWidget::close);
				});
				});
				});
				});
				});
				});
			});
		});
	});
	readinessTimer->start();
}
