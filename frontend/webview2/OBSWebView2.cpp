#include "OBSWebView2.hpp"
#include "WebView2Widget.hpp"
#include "UiGeometry.hpp"
#include <QCloseEvent>

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSQTDisplay.hpp>
#include <components/SourceTree.hpp>
#include <QFile>
#include <memory>
#include <utility/platform.hpp>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <graphics/graphics.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace {
QString SourceId(obs_source_t *source)
{
	return source ? QString::fromUtf8(obs_source_get_uuid(source)) : QString();
}

const QStringList controlNames = {
	QStringLiteral("streamButton"), QStringLiteral("broadcastButton"), QStringLiteral("recordButton"),
	QStringLiteral("pauseRecordButton"), QStringLiteral("replayBufferButton"), QStringLiteral("saveReplayButton"),
	QStringLiteral("virtualCamButton"), QStringLiteral("virtualCamConfigButton"),
	QStringLiteral("modeSwitch"), QStringLiteral("settingsButton"),
};

class OBSWebView2 final : public QWidget {
	QMainWindow *main;
	WebView2Widget *browser;
	OBSQTDisplay *preview;
	QLabel *errorLabel;
	QTimer *timer;
	QHash<QString, QPointer<QAction>> actions;
	QHash<QAction *, QString> actionIds;
	quint64 nextAction = 0;
	bool reportedPreviewGeometry = false;
	bool ownsApplicationSession = QCoreApplication::arguments().contains(QStringLiteral("--webview2"));
	QByteArray lastState;
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
	QJsonObject lastTestReply;
#endif
	std::mutex previewMutex;
	OBSWeakSource previewSource;

	QString registerAction(QAction *action)
	{
		if (actionIds.contains(action))
			return actionIds.value(action);
		const QString id = QStringLiteral("a%1").arg(++nextAction);
		actions.insert(id, action);
		actionIds.insert(action, id);
		connect(action, &QObject::destroyed, this, [this, action, id] {
			actions.remove(id);
			actionIds.remove(action);
		});
		return id;
	}

	QJsonArray menuState(const QList<QAction *> &list, int depth = 0)
	{
		QJsonArray result;
		if (depth > 8)
			return result;
		for (auto *action : list) {
			if (!action->isVisible() || action->property("webview2Entry").toBool())
				continue;
			QJsonObject entry{{"id", registerAction(action)}, {"text", action->text()},
					  {"enabled", action->isEnabled()}, {"separator", action->isSeparator()},
					  {"checkable", action->isCheckable()}, {"checked", action->isChecked()},
					  {"shortcut", action->shortcut().toString(QKeySequence::NativeText)}};
			if (action->menu())
				entry.insert("children", menuState(action->menu()->actions(), depth + 1));
			result.append(entry);
		}
		return result;
	}

	static void collectSelection(obs_scene_t *scene, QJsonArray &selection)
	{
		obs_scene_enum_items(scene, [](obs_scene_t *owner, obs_sceneitem_t *item, void *data) {
			auto &items = *static_cast<QJsonArray *>(data);
			if (obs_sceneitem_selected(item))
				items.append(QJsonObject{{"scene", SourceId(obs_scene_get_source(owner))},
							 {"id", QString::number(obs_sceneitem_get_id(item))},
							 {"uuid", SourceId(obs_sceneitem_get_source(item))}});
			if (obs_sceneitem_is_group(item))
				collectSelection(obs_sceneitem_group_get_scene(item), items);
			return true;
		}, &selection);
	}

	QJsonObject snapshot()
	{
		const bool studio = obs_frontend_preview_program_mode_active();
		OBSSourceAutoRelease scene = studio ? obs_frontend_get_current_preview_scene()
						   : obs_frontend_get_current_scene();
		{
			std::lock_guard<std::mutex> lock(previewMutex);
			previewSource = OBSGetWeakRef(scene);
		}
		obs_frontend_source_list scenes{};
		obs_frontend_get_scenes(&scenes);
		QJsonArray sceneList;
		for (size_t i = 0; i < scenes.sources.num; ++i) {
			auto *source = scenes.sources.array[i];
			sceneList.append(QJsonObject{{"uuid", SourceId(source)},
						     {"name", QString::fromUtf8(obs_source_get_name(source))}});
		}
		obs_frontend_source_list_free(&scenes);
		QJsonArray sourceList;
		if (scene) {
			obs_scene_enum_items(
				obs_scene_from_source(scene),
				[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
					auto *source = obs_sceneitem_get_source(item);
					static_cast<QJsonArray *>(data)->append(
						QJsonObject{{"id", QString::number(obs_sceneitem_get_id(item))},
							    {"uuid", SourceId(source)},
							    {"name", QString::fromUtf8(obs_source_get_name(source))},
							    {"visible", obs_sceneitem_visible(item)},
							    {"locked", obs_sceneitem_locked(item)},
							    {"selected", obs_sceneitem_selected(item)}});
					return true;
				}, &sourceList);
		}
		QJsonArray controls;
		for (const auto &name : controlNames) {
			if (auto *button = main->findChild<QAbstractButton *>(name)) {
				if (button->isVisibleTo(main))
					controls.append(QJsonObject{{"id", name}, {"text", OBSWeb::ControlLabel(button->text(), button->accessibleName(), button->toolTip())},
								    {"enabled", button->isEnabled()},
								    {"checked", button->isChecked()}, {"checkable", button->isCheckable()}});
			}
		}
		QJsonObject labels;
		for (const auto *key : {"Basic.Main.Scenes", "Basic.Main.Sources", "Basic.Main.Mixer",
					"Basic.Main.Controls", "Basic.Main.Transition", "Basic.MainMenu.File",
					"Basic.MainMenu.Edit", "Basic.MainMenu.View", "Basic.MainMenu.Profile"}) {
			labels.insert(QString::fromLatin1(key), QTStr(key));
		}
		QJsonObject state{{"title", main->windowTitle()}, {"scenes", sceneList}, {"sources", sourceList},
				   {"currentScene", SourceId(scene)}, {"controls", controls},
				   {"menus", menuState(main->menuBar()->actions())}, {"labels", labels},
				   {"studioMode", studio}, {"recording", obs_frontend_recording_active()},
				   {"streaming", obs_frontend_streaming_active()},
				   {"paused", obs_frontend_recording_paused()},
				   {"fps", std::round(obs_get_active_fps() * 100.0) / 100.0}};
		BPtr<char> profile = obs_frontend_get_current_profile();
		BPtr<char> collection = obs_frontend_get_current_scene_collection();
		QJsonObject context{{"profile", QString::fromUtf8(profile.Get())},
				    {"collection", QString::fromUtf8(collection.Get())}};
		for (const auto *key : {"currentScene", "sources", "controls", "studioMode", "recording", "streaming", "paused"})
			context.insert(QLatin1String(key), state.value(QLatin1String(key)));
		QJsonArray selection;
		if (scene)
			collectSelection(obs_scene_from_source(scene), selection);
		context.insert("selection", selection);
		state.insert("context", QString::fromLatin1(QCryptographicHash::hash(
			QJsonDocument(context).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
		return state;
	}

	void publishState(bool force = false)
	{
		const auto state = snapshot();
		const auto serialized = QJsonDocument(state).toJson(QJsonDocument::Compact);
		if (force || serialized != lastState) {
			lastState = serialized;
			browser->postMessage(QJsonObject{{"version", 1}, {"event", "state.changed"}, {"data", state}});
		}
	}

	void reply(const QString &id, const QJsonValue &result)
	{
		#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
		lastTestReply = QJsonObject{{"ok", true}, {"result", result}};
#endif
		browser->postMessage(QJsonObject{{"version", 1}, {"id", id}, {"ok", true}, {"result", result}});
	}

	void reject(const QString &id, const QString &code, const QString &message)
	{
		#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
		lastTestReply = QJsonObject{{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
#endif
		browser->postMessage(QJsonObject{{"version", 1}, {"id", id}, {"ok", false},
						{"error", QJsonObject{{"code", code}, {"message", message}}}});
	}

	bool validateContext(const QString &id, const QJsonObject &args)
	{
		const auto expected = args.value("context");
		if (!expected.isString() || expected != snapshot().value("context")) {
			reject(id, QStringLiteral("StaleContext"), QStringLiteral("OBS state changed. Try the action again."));
			publishState(true);
			return false;
		}
		return true;
	}

	void execute(const QJsonObject &message)
	{
		const auto id = message.value("id").toString();
		const auto command = message.value("command").toString();
		const auto args = message.value("args").toObject();
		if (command == QStringLiteral("state.get")) {
			reply(id, snapshot());
		} else if (command == QStringLiteral("scene.select")) {
			const auto uuid = args.value("uuid");
			OBSSourceAutoRelease source = uuid.isString()
							     ? obs_get_source_by_uuid(uuid.toString().toUtf8().constData())
							     : nullptr;
			bool belongs = false;
			obs_frontend_source_list scenes{};
			obs_frontend_get_scenes(&scenes);
			for (size_t i = 0; i < scenes.sources.num; ++i)
				belongs |= source && scenes.sources.array[i] == source;
			obs_frontend_source_list_free(&scenes);
			if (!belongs) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The scene no longer exists."));
				return;
			}
			if (obs_frontend_preview_program_mode_active())
				obs_frontend_set_current_preview_scene(source);
			else
				obs_frontend_set_current_scene(source);
			reply(id, QJsonObject{{"scene", SourceId(source)}});
			publishState();
		} else if (command == QStringLiteral("source.select")) {
			OBSSourceAutoRelease current = obs_frontend_preview_program_mode_active()
							      ? obs_frontend_get_current_preview_scene()
							      : obs_frontend_get_current_scene();
			auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
			if (!tree || args.value("scene").toString() != SourceId(current)) {
				reject(id, QStringLiteral("StaleContext"), QStringLiteral("The scene has changed. Select the source again."));
				return;
			}
			for (int row = 0; row < tree->model()->rowCount(); ++row) {
				const OBSSceneItem item = tree->Get(row);
				if (obs_sceneitem_get_scene(item) != obs_scene_from_source(current) ||
				    QString::number(obs_sceneitem_get_id(item)) != args.value("id").toString() ||
				    SourceId(obs_sceneitem_get_source(item)) != args.value("uuid").toString())
					continue;
				const auto index = tree->model()->index(row, 0);
				tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
				reply(id, QJsonObject{});
				publishState();
				return;
			}
			reject(id, QStringLiteral("Unavailable"), QStringLiteral("The source item no longer exists."));
		} else if (command == QStringLiteral("menu.prepare")) {
			const QPointer<QAction> action = actions.value(args.value("id").toString());
			if (!action || !action->menu() || !action->isEnabled() || !action->isVisible()) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The menu is no longer available."));
				return;
			}
			QPointer<QMenu> menu = action->menu();
			QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
			if (menu)
				reply(id, menuState(menu->actions()));
			else
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The menu was removed."));
		} else if (command == QStringLiteral("action.invoke")) {
			if (!validateContext(id, args))
				return;
			const QPointer<QAction> action = actions.value(args.value("id").toString());
			if (!action || !action->isEnabled() || !action->isVisible() || action->menu()) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The action is no longer available."));
				return;
			}
			reply(id, QJsonObject{{"accepted", true}});
			// WebView2Widget already queued this message outside the COM callback.
			// Execute before another scene/profile event can change the target.
			action->trigger();
		} else if (command == QStringLiteral("control.click")) {
			if (!validateContext(id, args))
				return;
			const auto name = args.value("id").toString();
			const QPointer<QAbstractButton> button =
				controlNames.contains(name) ? main->findChild<QAbstractButton *>(name) : nullptr;
			if (!button || !button->isEnabled() || !button->isVisibleTo(main)) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The control is no longer available."));
				return;
			}
			reply(id, QJsonObject{{"accepted", true}});
			button->click();
		} else if (command == QStringLiteral("source.properties") || command == QStringLiteral("source.filters")) {
			OBSSourceAutoRelease source =
				obs_get_source_by_uuid(args.value("uuid").toString().toUtf8().constData());
			if (!source) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The source no longer exists."));
				return;
			}
			reply(id, QJsonObject{{"accepted", true}});
			if (command == QStringLiteral("source.properties"))
				obs_frontend_open_source_properties(source);
			else
				obs_frontend_open_source_filters(source);
		} else if (command == QStringLiteral("preview.bounds")) {
			const auto bounds = OBSWeb::PreviewRect(args, browser->geometry());
			if (!bounds) {
				reject(id, QStringLiteral("InvalidArgs"), QStringLiteral("Invalid preview bounds or viewport."));
				return;
			}
			preview->setGeometry(*bounds);
			preview->setVisible(args.value("visible").toBool(true) && !bounds->isEmpty());
			preview->raise();
			if (!reportedPreviewGeometry) {
				reportedPreviewGeometry = true;
				blog(LOG_INFO, "[WebView2] Preview viewport %.1fx%.1f CSS, host %dx%d Qt, rect %d,%d %dx%d",
				     args.value("viewportWidth").toDouble(), args.value("viewportHeight").toDouble(),
				     browser->width(), browser->height(), bounds->x(), bounds->y(), bounds->width(), bounds->height());
			}
			reply(id, QJsonObject{});
		} else if (command == QStringLiteral("window.original")) {
			main->showNormal();
			main->raise();
			main->activateWindow();
			reply(id, QJsonObject{});
		} else {
			reject(id, QStringLiteral("UnknownCommand"), QStringLiteral("This command is not supported."));
		}
	}

	static void drawPreview(void *data, uint32_t width, uint32_t height)
	{
		auto *self = static_cast<OBSWebView2 *>(data);
		OBSSource source;
		{
			std::lock_guard<std::mutex> lock(self->previewMutex);
			source = OBSGetStrongRef(self->previewSource);
		}
		if (!source)
			return;
		const uint32_t baseWidth = obs_source_get_width(source);
		const uint32_t baseHeight = obs_source_get_height(source);
		if (!baseWidth || !baseHeight)
			return;
		const float scale = std::min(float(width) / baseWidth, float(height) / baseHeight);
		const int cx = int(baseWidth * scale);
		const int cy = int(baseHeight * scale);
		gs_viewport_push();
		gs_projection_push();
		gs_set_viewport((int(width) - cx) / 2, (int(height) - cy) / 2, cx, cy);
		gs_ortho(0.0f, float(baseWidth), 0.0f, float(baseHeight), -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_projection_pop();
		gs_viewport_pop();
	}

#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
#include "../../test/webview2/obs-integration.inl"
#endif

protected:
	void closeEvent(QCloseEvent *event) override
	{
		if (!ownsApplicationSession) {
			QWidget::closeEvent(event);
			return;
		}
		// Keep this window alive if OBS declines shutdown (for example while
		// confirming an active recording). Shutdown deletes it after acceptance.
		event->ignore();
		QTimer::singleShot(0, main, &QWidget::close);
	}

public:
	explicit OBSWebView2(QMainWindow *parent, const QString &assets, const QString &profile)
		: QWidget(parent, Qt::Window), main(parent)
	{
		setObjectName(QStringLiteral("obsWebView2Window"));
		setWindowTitle(QStringLiteral("OBS — WebView2"));
		setAttribute(Qt::WA_DeleteOnClose);
		resize(1280, 840);
		setMinimumSize(850, 600);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		errorLabel = new QLabel(this);
		errorLabel->setWordWrap(true);
		errorLabel->hide();
		layout->addWidget(errorLabel);
		browser = new WebView2Widget(this, assets, profile);
		layout->addWidget(browser);
		preview = new OBSQTDisplay(this);
		preview->SetDisplayBackgroundColor(QColor(16, 16, 19));
		preview->hide();
		connect(preview, &OBSQTDisplay::DisplayCreated, this, [this] {
			obs_display_add_draw_callback(preview->GetDisplay(), drawPreview, this);
		});
		timer = new QTimer(this);
		timer->setInterval(500);
		connect(timer, &QTimer::timeout, this, [this] { publishState(); });
		connect(browser, &WebView2Widget::ready, this, [this] {
			blog(LOG_INFO, "[WebView2] Local interface ready");
			publishState(true);
			timer->start();
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
			const auto arguments = QCoreApplication::arguments();
			if (arguments.contains(QStringLiteral("--webview2-self-test")) &&
			    arguments.contains(QStringLiteral("--portable")) &&
			    arguments.contains(QStringLiteral("--only-bundled-plugins")))
				runIntegrationChecks();
#endif
		});
		connect(browser, &WebView2Widget::messageReceived, this,
			[this](const QJsonObject &message) { execute(message); });
		connect(browser, &WebView2Widget::failed, this, [this](const QString &error) {
			timer->stop();
			preview->hide();
			errorLabel->setText(error + QStringLiteral("\nClose this window and reopen WebView2 from OBS Tools."));
			errorLabel->show();
			blog(LOG_ERROR, "[WebView2] %s", error.toUtf8().constData());
		});
	}

	~OBSWebView2() override
	{
		timer->stop();
		// The draw callback must stop before the source reference and mutex die.
		if (preview->GetDisplay())
			obs_display_remove_draw_callback(preview->GetDisplay(), drawPreview, this);
		delete preview;
		delete browser;
	}
};
} // namespace

void InstallWebView2Frontend(OBSBasic *window)
{
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
	const auto arguments = QCoreApplication::arguments();
	if (arguments.contains(QStringLiteral("--webview2-qt-baseline")) &&
	    arguments.contains(QStringLiteral("--webview2-self-test")) &&
	    arguments.contains(QStringLiteral("--portable")) &&
	    arguments.contains(QStringLiteral("--only-bundled-plugins"))) {
		QTimer::singleShot(4000, window, &QWidget::close);
		return;
	}
#endif
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("WebView2"));
	action->setProperty("webview2Entry", true);
	QObject::connect(action, &QAction::triggered, window, [window] {
		if (auto *existing = window->findChild<QWidget *>(QStringLiteral("obsWebView2Window"), Qt::FindDirectChildrenOnly)) {
			existing->showNormal();
			existing->raise();
			existing->activateWindow();
			return;
		}
		std::string assets;
		if (!GetDataFilePath("webview2", assets)) {
			blog(LOG_ERROR, "[WebView2] Packaged interface resources were not found");
			QMessageBox::warning(window, QStringLiteral("WebView2"),
					     QStringLiteral("The packaged WebView2 interface is missing. Rebuild or reinstall this fork with its data/obs-studio/webview2 folder."));
			return;
		}
		BPtr<char> profile = GetAppConfigPathPtr("obs-studio/webview2");
		auto *frontend = new OBSWebView2(window, QString::fromUtf8(assets.c_str()),
					       QString::fromUtf8(profile.Get()));
		frontend->show();
	});
	if (QCoreApplication::arguments().contains(QStringLiteral("--webview2")))
		QTimer::singleShot(0, action, &QAction::trigger);
}

void ShutdownWebView2Frontend(OBSBasic *window)
{
	// OBS shuts libobs down before destroying all Qt children.
	delete window->findChild<QWidget *>(QStringLiteral("obsWebView2Window"), Qt::FindDirectChildrenOnly);
}
