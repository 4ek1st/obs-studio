#include "OBSWebView2.hpp"
#include "WebView2Widget.hpp"
#include "UiGeometry.hpp"
#include "AudioMixerBridge.hpp"
#include "ControlBridge.hpp"
#include "SliderBridge.hpp"
#include "QtDialogBridge.hpp"
#include "FloatingDockGroupChrome.hpp"
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSQTDisplay.hpp>
#include <widgets/OBSBasicPreview.hpp>
#include <widgets/AudioMixer.hpp>
#include <components/VolumeControl.hpp>
#include <components/SourceTree.hpp>
#include <QFile>
#include <QFontInfo>
#include <QBuffer>
#include <memory>
#include <utility/platform.hpp>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <qt-wrappers.hpp>
#include <graphics/graphics.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QGridLayout>
#include <QPushButton>
#include <QKeyEvent>
#include <QDir>
#include <QDialog>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLibrary>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QRegion>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>
#include <QToolBar>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QColor>
#include <QSpinBox>
#include <QSlider>
#include <QScrollBar>
#include <QDockWidget>
#include <QStatusBar>
#include <QContextMenuEvent>
#include <QCursor>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QToolButton>
#include <QTabBar>
#include <QTabWidget>
#include <QStyle>
#include <QStyleOptionDockWidget>
#include <QPainter>
#include <QHBoxLayout>
#include <QWindow>
#include <QApplication>
#include <Windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
#include "../../test/webview2/audio-integration.inl"
#include "../../test/webview2/dialog-integration.inl"
#include "../../test/webview2/output-integration.inl"
#include "../../test/webview2/replay-integration.inl"
#include "../../test/webview2/tool-data-integration.inl"
#include "../../test/webview2/parity-audit.inl"
#include "../../test/webview2/plugin-audit.inl"
#include "../../test/webview2/window-drop-integration.inl"
#endif

namespace {
class ThemeWindowFrames final : public QObject {
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    struct AppliedFrame { WId id; COLORREF caption; COLORREF text; BOOL dark; };
    static inline ThemeWindowFrames *instance = nullptr;
    QHash<QWidget *, AppliedFrame> applied;
    HWINEVENTHOOK showHook = nullptr;

    static bool setFrame(HWND hwnd, COLORREF caption, COLORREF text, BOOL dark, const char *name)
    {
        static auto setAttribute = reinterpret_cast<SetAttribute>(QLibrary::resolve(QStringLiteral("dwmapi"), "DwmSetWindowAttribute"));
        if (!setAttribute) return false;
        const auto darkResult = setAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const auto captionResult = setAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        const auto borderResult = setAttribute(hwnd, DWMWA_BORDER_COLOR, &caption, sizeof(caption));
        const auto textResult = setAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
        if (FAILED(darkResult) || FAILED(captionResult) || FAILED(borderResult) || FAILED(textResult)) {
            blog(LOG_WARNING, "[WebView2] Theme frame attributes unavailable for %s: dark=%lx caption=%lx border=%lx text=%lx",
                 name, darkResult, captionResult, borderResult, textResult);
            return false;
        }
        return true;
    }

    static void CALLBACK windowShown(HWINEVENTHOOK, DWORD, HWND hwnd, LONG objectId, LONG childId, DWORD, DWORD)
    {
        if (!instance || !hwnd || objectId != OBJID_WINDOW || childId != CHILDID_SELF) return;
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != GetCurrentProcessId() || GetAncestor(hwnd, GA_ROOT) != hwnd ||
            !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION)) return;
        if (auto *widget = QWidget::find(reinterpret_cast<WId>(hwnd)); widget && widget->isWindow()) {
            instance->apply(widget);
            return;
        }
        const auto palette = qApp->palette();
        const QColor background = palette.color(QPalette::Window);
        const QColor foreground = palette.color(QPalette::WindowText);
        setFrame(hwnd, RGB(background.red(), background.green(), background.blue()),
                 RGB(foreground.red(), foreground.green(), foreground.blue()),
                 background.lightness() < 128, "native application dialog");
    }

    static bool hasCaption(QWidget *window)
    {
        if (!window || !window->isWindow()) return false;
        const auto type = window->windowType();
        return type != Qt::Popup && type != Qt::ToolTip && type != Qt::SplashScreen;
    }

public:
    explicit ThemeWindowFrames(QObject *parent) : QObject(parent)
    {
        instance = this;
        qApp->installEventFilter(this);
        showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &windowShown,
                                   GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }

    ~ThemeWindowFrames() override
    {
        if (showHook) UnhookWinEvent(showHook);
        instance = nullptr;
        qApp->removeEventFilter(this);
    }

    void apply(QWidget *window)
    {
        if (!hasCaption(window)) return;
        const auto id = window->internalWinId();
        if (!id) return;
        const auto palette = window->palette();
        const QColor background = palette.color(QPalette::Window);
        const QColor foreground = palette.color(QPalette::WindowText);
        const COLORREF caption = RGB(background.red(), background.green(), background.blue());
        const COLORREF text = RGB(foreground.red(), foreground.green(), foreground.blue());
        const BOOL dark = background.lightness() < 128;
        const auto previous = applied.constFind(window);
        if (previous != applied.cend() && previous->id == id && previous->caption == caption &&
            previous->text == text && previous->dark == dark) return;
        const auto hwnd = reinterpret_cast<HWND>(id);
        if (setFrame(hwnd, caption, text, dark, window->objectName().toUtf8().constData())) {
            if (previous == applied.cend())
                connect(window, &QObject::destroyed, this, [this, window] { applied.remove(window); });
            applied.insert(window, {id, caption, text, dark});
        }
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (auto *window = qobject_cast<QWidget *>(object)) {
            switch (event->type()) {
            case QEvent::WinIdChange:
            case QEvent::Show:
            case QEvent::PaletteChange:
            case QEvent::ApplicationPaletteChange:
            case QEvent::WindowActivate:
            case QEvent::WindowDeactivate:
                apply(window);
                break;
            default: break;
            }
        }
        return QObject::eventFilter(object, event);
    }
};

void installThemeWindowFrames()
{
    static auto *frames = new ThemeWindowFrames(qApp);
    for (auto *window : QApplication::topLevelWidgets()) frames->apply(window);
}

struct NativePlacement {
	QPointer<QWidget> parent;
	QPointer<QLayout> layout;
	int index = -1, row = 0, column = 0, rows = 1, columns = 1;
	Qt::Alignment alignment{};
	bool visible = false;
	static QLayout *owner(QLayout *layout, QWidget *widget)
	{
		if (!layout) return nullptr;
		if (layout->indexOf(widget) >= 0) return layout;
		for (int i = 0; i < layout->count(); ++i)
			if (auto *found = owner(layout->itemAt(i)->layout(), widget)) return found;
		return nullptr;
	}
	void take(QWidget *widget, QWidget *destination)
	{
		parent = widget->parentWidget();
		visible = !widget->isHidden();
		layout = parent ? owner(parent->layout(), widget) : nullptr;
		if (layout) {
			index = layout->indexOf(widget);
			alignment = layout->itemAt(index)->alignment();
			if (auto *grid = qobject_cast<QGridLayout *>(layout.data()))
				grid->getItemPosition(index, &row, &column, &rows, &columns);
			layout->removeWidget(widget);
		}
		widget->setParent(destination);
		widget->hide();
	}
	void restore(QWidget *widget)
	{
		if (!widget || !parent) return;
		widget->hide();
		widget->setParent(parent);
		if (auto *grid = qobject_cast<QGridLayout *>(layout.data()))
			grid->addWidget(widget, row, column, rows, columns, alignment);
		else if (auto *box = qobject_cast<QBoxLayout *>(layout.data()))
			box->insertWidget(index, widget, 0, alignment);
		else if (layout) layout->addWidget(widget);
		widget->setVisible(visible);
		parent = nullptr;
		layout = nullptr;
	}
};

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
	QPointer<QWidget> nativeCentral;
	bool workspaceMounted = false;
	std::array<QTabWidget::TabPosition, 4> nativeDockTabPositions{};
	QMainWindow::DockOptions nativeDockOptions{};
	bool dockTabPositionsSaved = false;
	FloatingDockGroupChrome *floatingDockGroupChrome = nullptr;
	WebView2Widget *browser;
	OBSQTDisplay *preview;
	QPointer<OBSQTDisplay> programPreview;
	NativePlacement previewPlacement, programPlacement;
	bool surfacesBorrowed = false;
	bool nativeEditor = false;
	std::unique_ptr<OBSWeb::AudioMixerBridge> audio;
	QTimer *meterTimer = nullptr;
	QHash<QString, QPointer<QAbstractButton>> liveButtons;
	QHash<QAbstractButton *, QString> buttonIds;
	quint64 nextButton = 0;
	QSet<QDockWidget *> routedDocks;
	QHash<qint64, QString> sourceIcons;
	QString toolbarSource;
	QPersistentModelIndex sourceSelectionAnchor;
	QString sourceAnchorScene;
	QLabel *errorLabel;
	QTimer *timer;
	QHash<QString, QPointer<QAction>> actions;
	QHash<QAction *, QString> actionIds;
	quint64 nextAction = 0;
	bool reportedPreviewGeometry = false;
	QByteArray lastState;
	bool failedFrontend = false;
	QRegion htmlOverlays;
	bool htmlModalOpen = false;
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
		// Preserve list-specific shortcut scope (for example the two Delete actions).
		if (action->shortcutContext() == Qt::WindowShortcut || action->shortcutContext() == Qt::ApplicationShortcut)
			addAction(action);
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
			QJsonArray shortcutKeys;
			for (const auto &shortcut : action->shortcuts())
				shortcutKeys.append(shortcut.toString(QKeySequence::PortableText));
			QJsonObject entry{{"id", registerAction(action)}, {"text", action->text()},
					  {"name", action->objectName().isEmpty() && action->menu() ? action->menu()->objectName() : action->objectName()},
					  {"enabled", action->isEnabled()}, {"separator", action->isSeparator()},
					  {"checkable", action->isCheckable()}, {"checked", action->isChecked()},
					  {"shortcutKey", action->shortcut().toString(QKeySequence::PortableText)},
					  {"shortcutKeys", shortcutKeys},
					  {"shortcutGlobal", action->shortcutContext() == Qt::WindowShortcut || action->shortcutContext() == Qt::ApplicationShortcut},
					  {"shortcut", action->shortcut().toString(QKeySequence::NativeText)}};
			if (action->menu())
				entry.insert("children", menuState(action->menu()->actions(), depth + 1));
			result.append(entry);
		}
		return result;
	}

#include "WorkspaceControls.inl"
#include "PreviewControls.inl"

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
		const QJsonArray sourceList = sourceRows(scene);
		QJsonArray controls;
		for (const auto &name : controlNames) {
			if (auto *button = main->findChild<QAbstractButton *>(name)) {
				auto *dock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
				if (button->isVisibleTo(dock && dock->widget() ? dock->widget() : main)) {
					auto *layout = NativePlacement::owner(button->parentWidget()->layout(), button);
					controls.append(OBSWeb::ControlPresentation(button, name, qobject_cast<QHBoxLayout *>(layout) ? layout->objectName() : name, sourceIcons));
				}
			}
		}
		QJsonObject labels;
		for (const auto *key : {"Basic.Main.Scenes", "Basic.Main.Sources", "Mixer",
					"Basic.Main.Controls", "Basic.SceneTransitions", "Basic.MainMenu.File",
					"Basic.MainMenu.Edit", "Basic.MainMenu.View", "Basic.MainMenu.Profile",
					"OK", "Cancel", "Rename", "Properties", "Filters", "Interact",
					"Transition", "StudioMode.Preview", "StudioMode.Program",
					"Basic.Main.PreviewConextMenu.Enable", "ContextBar.NoSelectedSource",
					"Basic.MainMenu.View.ContextBar", "Basic.MainMenu.Edit.Scale.ZoomIn",
					"Basic.MainMenu.Edit.Scale.ZoomOut", "Basic.Main.GroupItems",
					"Basic.TransitionDuration", "Basic.AdvAudio", "Basic.AdvAudio.VolumeSource",
					"Basic.AudioMixer.Options", "Basic.AudioMixer.HiddenTotal",
					"Basic.AudioMixer.ShowHidden", "Basic.AudioMixer.Layout.Horizontal",
					"Basic.AudioMixer.Layout.Vertical", "Basic.AudioMixer.Category.Global",
					"Basic.AudioMixer.Category.Pinned", "Basic.AudioMixer.Category.Active",
					"Basic.AudioMixer.Category.Inactive", "Mute", "Unmute", "MonitorOn",
					"MonitorOff", "AddScene", "AddSource",
					"WebView2.Menus", "WebView2.MainWindow", "WebView2.MainWindow.Tooltip",
					"WebView2.CloseNotification", "WebView2.PreviewAndProgram",
					"WebView2.PreviewTrimLeft", "WebView2.PreviewTrimRight",
					"WebView2.PreviewScrollVertical", "WebView2.PreviewScrollHorizontal",
					"WebView2.ManualTransition", "WebView2.TransitionSettings",
					"WebView2.PreviewScaling", "WebView2.PanelHeight",
					"WebView2.SceneTools", "WebView2.SourceTools", "WebView2.Audio",
					"WebView2.Milliseconds", "WebView2.Connecting", "WebView2.NoConnection",
					"WebView2.ExpandGroup", "WebView2.CollapseGroup",
					"WebView2.ShowSource", "WebView2.HideSource", "WebView2.LockSource",
					"WebView2.UnlockSource", "WebView2.EmptySources", "WebView2.EmptyScenes",
					"WebView2.EmptyAudio", "WebView2.ChannelActions", "WebView2.SelectedSources",
					"WebView2.ConfigureQuickTransition", "WebView2.AddQuickTransition"}) {
			labels.insert(QString::fromLatin1(key), QTStr(key));
		}
		QJsonObject state{{"title", main->windowTitle()}, {"locale", QString::fromUtf8(App()->GetLocale())},
				   {"scenes", sceneList}, {"sources", sourceList},
				   {"currentScene", SourceId(scene)}, {"controls", controls},
				   {"menus", menuState(main->menuBar()->actions())}, {"labels", labels},
				   {"studioMode", studio}, {"recording", obs_frontend_recording_active()},
				   {"streaming", obs_frontend_streaming_active()},
				   {"paused", obs_frontend_recording_paused()},
				   {"fps", std::round(obs_get_active_fps() * 100.0) / 100.0}};
		const auto palette = main->palette();
		state.insert("appearance", QJsonObject{{"background", palette.color(QPalette::Window).name()},
			{"panel", palette.color(QPalette::Base).name()}, {"raised", palette.color(QPalette::Button).name()},
			{"text", palette.color(QPalette::WindowText).name()}, {"muted", palette.color(QPalette::Disabled, QPalette::Text).name()},
			{"selected", palette.color(QPalette::Highlight).name()}, {"selectedText", palette.color(QPalette::HighlightedText).name()},
			{"light", palette.color(QPalette::Window).lightness() > 150}});
		setWindowTitle(main->windowTitle() + QStringLiteral(" — WebView2"));
		addWorkspaceState(state);
		addPreviewState(state);
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
			postWorkspaceMessage(QJsonObject{{"version", 1}, {"event", "state.changed"}, {"data", state}});
		}
	}

	void reply(const QString &id, const QJsonValue &result)
	{
		#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
		lastTestReply = QJsonObject{{"ok", true}, {"result", result}};
#endif
		postWorkspaceMessage(QJsonObject{{"version", 1}, {"id", id}, {"ok", true}, {"result", result}});
	}

	void reject(const QString &id, const QString &code, const QString &message)
	{
		#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
		lastTestReply = QJsonObject{{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
#endif
		postWorkspaceMessage(QJsonObject{{"version", 1}, {"id", id}, {"ok", false},
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
		const QPointer<OBSWebView2> guard(this);
		const auto pendingCommand = message.value("command").toString();
		if (pendingCommand != "source.rename" && pendingCommand != "state.get" && pendingCommand != "preview.bounds" && pendingCommand != "preview.layout" &&
		    pendingCommand != "source.hover" && pendingCommand != "menu.prepare") {
			if (auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"))) tree->FinishWebViewEdits();
		}
		if (executePreview(message)) return;
		if (executeWorkspace(message))
			return;
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
			publishState(true);
			reply(id, QJsonObject{{"scene", SourceId(source)}});
		} else if (command == QStringLiteral("source.select")) {
			OBSSourceAutoRelease current = obs_frontend_preview_program_mode_active()
							      ? obs_frontend_get_current_preview_scene()
							      : obs_frontend_get_current_scene();
			auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"));
			if (!tree || args.value("scene").toString() != SourceId(current)) {
				reject(id, QStringLiteral("StaleContext"), QStringLiteral("The scene has changed. Select the source again."));
				return;
			}
            const int row = sourceRow(args);
            if (row < 0) {
                reject(id, QStringLiteral("Unavailable"), QStringLiteral("The source item no longer exists."));
                return;
            }
            const auto index = tree->model()->index(row, 0);
            const auto currentIndex = tree->selectionModel()->currentIndex();
            const QString sceneId = SourceId(current);
            if (sourceAnchorScene != sceneId || sourceSelectionAnchor.model() != tree->model()) {
                sourceSelectionAnchor = QPersistentModelIndex();
                sourceAnchorScene = sceneId;
            }
            if (args.value("focusOnly").toBool()) {
                tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                sourceSelectionAnchor = index;
            } else if (args.value("range").toBool() && (sourceSelectionAnchor.isValid() || currentIndex.isValid())) {
                if (!sourceSelectionAnchor.isValid()) sourceSelectionAnchor = currentIndex;
                tree->selectionModel()->select(QItemSelection(sourceSelectionAnchor, index),
                    args.value("additive").toBool() ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect);
                tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            } else {
                tree->selectionModel()->setCurrentIndex(index, args.value("additive").toBool()
                    ? QItemSelectionModel::Toggle : QItemSelectionModel::ClearAndSelect);
                sourceSelectionAnchor = index;
            }
            publishState(true);
            reply(id, QJsonObject{});
		} else if (command == QStringLiteral("menu.prepare")) {
			const QPointer<QAction> action = actions.value(args.value("id").toString());
			if (!action || !action->menu() || !action->isEnabled() || !action->isVisible()) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The menu is no longer available."));
				return;
			}
			QPointer<QMenu> menu = action->menu();
			QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
			if (!guard) return;
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
			if (!guard) return;
			if (action && (action->objectName() == "resetUI" || action->objectName() == "resetDocks"))
				browser->postMessage(QJsonObject{{"version", 1}, {"event", "workspace.reset"}, {"data", QJsonObject{}}});
			publishState(true);
		} else if (command == QStringLiteral("control.click")) {
			if (!validateContext(id, args))
				return;
			const auto name = args.value("id").toString();
			const QPointer<QAbstractButton> button =
				liveButtons.contains(name) ? liveButtons.value(name).data() : (controlNames.contains(name) ? main->findChild<QAbstractButton *>(name) : nullptr);
			QWidget *visibilityRoot = main;
			if (button && nativeCentral && nativeCentral->isAncestorOf(button)) visibilityRoot = nativeCentral;
			if (!button || !button->isEnabled() || !button->isVisibleTo(visibilityRoot)) {
				reject(id, QStringLiteral("Unavailable"), QStringLiteral("The control is no longer available."));
				return;
			}
			reply(id, QJsonObject{{"accepted", true}});
			OBSWeb::ActivateControlButton(button);
			if (guard) publishState(true);
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
		} else if (command == QStringLiteral("preview.bounds") || command == QStringLiteral("preview.layout")) {
            if (!surfacesBorrowed) { reply(id, QJsonObject{}); return; }
            const bool batch = command == QStringLiteral("preview.layout");
            const auto entries = batch ? args.value("surfaces").toArray() : QJsonArray{args};
            if (entries.size() != (batch ? 2 : 1)) { reject(id, "InvalidArgs", "Invalid preview layout."); return; }
            struct Placement { QString target; QRect bounds; bool visible; };
            QList<Placement> placements;
            QSet<QString> targets;
            for (const auto &entry : entries) {
                auto item = entry.toObject();
                item.insert("viewportWidth", args.value("viewportWidth"));
                item.insert("viewportHeight", args.value("viewportHeight"));
                const QString target = item.value("target").toString(QStringLiteral("preview"));
                const auto bounds = OBSWeb::PreviewRect(item, browser->geometry());
                if ((target != "preview" && target != "program") || targets.contains(target) || !bounds) {
                    reject(id, "InvalidArgs", "Invalid preview bounds or viewport."); return;
                }
                targets.insert(target);
                placements.append({target, *bounds, item.value("visible").toBool(true)});
            }
            QRegion overlays;
            if (args.contains("overlays")) {
                const auto value = args.value("overlays");
                if (!value.isArray() || value.toArray().size() > 32) {
                    reject(id, "InvalidArgs", "Invalid overlay rectangles."); return;
                }
                for (const auto entry : value.toArray()) {
                    if (!entry.isObject()) { reject(id, "InvalidArgs", "Invalid overlay rectangle."); return; }
                    auto overlay = entry.toObject();
                    overlay.insert("viewportWidth", args.value("viewportWidth"));
                    overlay.insert("viewportHeight", args.value("viewportHeight"));
                    const auto rect = OBSWeb::PreviewRect(overlay, browser->geometry());
                    if (!rect) { reject(id, "InvalidArgs", "Overlay is outside the viewport."); return; }
                    overlays |= rect->translated(-browser->pos());
                }
            }
            htmlOverlays = overlays;
            htmlModalOpen = args.value("modal").toBool();
            syncProgramSurface();
            // Both native displays and their shared mask move in one UI turn.
            // Never wait for another browser round trip between the two sides.
            for (const auto &placement : placements) {
                OBSQTDisplay *surface = placement.target == "program" ? programPreview.data() : preview;
                if (surface) {
                    surface->setGeometry(placement.bounds);
                    const bool enabled = placement.target == "program" || previewRequestedEnabled();
                    surface->setVisible(enabled && placement.visible && !placement.bounds.isEmpty());
                    surface->installEventFilter(this);
                }
            }
            updatePreviewMask();
            if (targets.contains("preview") && !reportedPreviewGeometry) {
                reportedPreviewGeometry = true;
                const auto bounds = preview->geometry();
                blog(LOG_INFO, "[WebView2] Native editor viewport %.1fx%.1f CSS, host %dx%d Qt, rect %d,%d %dx%d",
                     args.value("viewportWidth").toDouble(), args.value("viewportHeight").toDouble(),
                     browser->width(), browser->height(), bounds.x(), bounds.y(), bounds.width(), bounds.height());
            }
            if (batch) {
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
                lastTestReply = QJsonObject{{"ok", true}, {"result", QJsonObject{}}};
#endif
                // Resize acknowledgements belong only to the central document;
                // broadcasting at frame rate would wake every dock renderer.
                browser->postMessage({{"version", 1}, {"id", id}, {"ok", true}, {"result", QJsonObject{}}});
            } else reply(id, QJsonObject{});
		} else if (command == QStringLiteral("window.original")) {
			timer->stop();
			meterTimer->stop();
			restoreSurfaces();
			setProperty("webview2OwnsSession", false);
			hide();
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
#include "../../test/webview2/persistence-integration.inl"
#include "../../test/webview2/workspace-integration.inl"
#include "../../test/webview2/preview-parity.inl"
#include "../../test/webview2/obs-integration.inl"
#endif

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (surfacesBorrowed && (watched == preview || watched == programPreview.data())) {
			if (htmlModalOpen && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel ||
					      event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
				browser->setFocus(Qt::OtherFocusReason);
				return true;
			}
			if (!htmlOverlays.isEmpty() && event->type() == QEvent::MouseButtonPress) {
				browser->postMessage({{"version", 1}, {"event", "overlays.dismiss"}, {"data", QJsonObject{}}});
				return true;
			}
		}
		return QWidget::eventFilter(watched, event);
	}
	void dragEnterEvent(QDragEnterEvent *event) override { QApplication::sendEvent(main, event); }
	void dropEvent(QDropEvent *event) override { QApplication::sendEvent(main, event); }
	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);
		// Window state, tray and native preview suspension belong to OBSBasic.
		// This widget only occupies its central area; it is not a second window.
	}
	void closeEvent(QCloseEvent *event) override
	{
		if (!workspaceMounted) {
			QWidget::closeEvent(event);
			return;
		}
		// Keep this window alive if OBS declines shutdown (for example while
		// confirming an active recording). Shutdown deletes it after acceptance.
		event->ignore();
		QTimer::singleShot(0, main, &QWidget::close);
	}

public:
	bool beginInlineRename(bool source)
	{
		if (!workspaceMounted || !isVisible()) return false;
		auto *dock = main->findChild<QDockWidget *>(source ? QStringLiteral("sourcesDock") : QStringLiteral("scenesDock"));
		const auto surface = webDockViews.value(dock);
		if (!surface || !surface->isVisible() || surface->property("webview2Failed").toBool()) return false;
		QJsonObject row;
		if (source) {
			for (const auto &value : snapshot().value("sources").toArray())
				if (value.toObject().value("selected").toBool()) { row = value.toObject(); break; }
		} else {
			const auto current = static_cast<OBSBasic *>(main)->GetCurrentSceneSource();
			if (current) row.insert("uuid", SourceId(current));
		}
		if (row.isEmpty()) return false;
		// Grouping has just inserted a row; deliver it before asking the browser
		// to create the inline input for that row.
		publishState(true);
		surface->postMessage({{"version", 1}, {"event", "workspace.rename"},
			{"data", QJsonObject{{"kind", source ? "source" : "scene"}, {"row", row}}}});
		return true;
	}
	void mountWorkspace()
	{
		if (workspaceMounted) return;
		if (!dockTabPositionsSaved) {
			nativeDockOptions = main->dockOptions();
			int index = 0;
			for (const auto area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
			                        Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea})
				nativeDockTabPositions[index++] = main->tabPosition(area);
			dockTabPositionsSaved = true;
		}
		// A combined dock should reveal both panel names beside its header.
		// QMainWindow still owns native drag, tab switching and layout persistence.
		// Qt requires GroupedDragging to unplug an individual tab by dragging it.
		main->setDockOptions(main->dockOptions() | QMainWindow::GroupedDragging);
		main->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
		nativeCentral = main->takeCentralWidget();
		if (nativeCentral) {
			// Keep ui pointers, source toolbars and Studio Mode controllers alive
			// under their original OBSBasic owner, even while their view is replaced.
			nativeCentral->setParent(main);
			nativeCentral->hide();
		}
		main->setCentralWidget(this);
		workspaceMounted = true;
		setProperty("webview2NativeDocking", true);
		main->setProperty("webview2NativeDocking", true);
		floatingDockGroupChrome = new FloatingDockGroupChrome(main, this);
		setProperty("webview2OwnsSession", false);
		show();
	}
	void unmountWorkspace()
	{
		if (!workspaceMounted) return;
		delete floatingDockGroupChrome;
		floatingDockGroupChrome = nullptr;
		if (dockTabPositionsSaved) {
			main->setDockOptions(nativeDockOptions);
			int index = 0;
			for (const auto area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
			                        Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea})
				main->setTabPosition(area, nativeDockTabPositions[index++]);
			dockTabPositionsSaved = false;
		}
		workspaceMounted = false;
		setProperty("webview2NativeDocking", false);
		main->setProperty("webview2NativeDocking", false);
		if (main->centralWidget() == this) main->takeCentralWidget();
		hide();
		setParent(main);
		if (nativeCentral) {
			main->setCentralWidget(nativeCentral);
			nativeCentral->show();
		}
		nativeCentral = nullptr;
	}
	void updatePreviewMask()
	{
		if (!surfacesBorrowed) { browser->clearMask(); return; }
		QRegion region(browser->rect());
		for (auto *surface : {preview, programPreview.data()}) {
			if (!surface || surface->parentWidget() != this || surface->isHidden()) continue;
			region -= surface->geometry().translated(-browser->pos());
			surface->lower();
		}
		region |= htmlOverlays.intersected(QRegion(browser->rect()));
		// The browser remains above native GPU displays. Only its video slots are
		// punched out; HTML menus cover their own overlap without hiding the video.
		if (region == QRegion(browser->rect())) browser->clearMask();
		else browser->setMask(region);
		browser->raise();
	}
	bool hasFailed() const { return failedFrontend; }
	void restoreSurfaces()
	{
		if (auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources"))) tree->FinishWebViewEdits();
		restoreWebDocks();
		if (!surfacesBorrowed) return;
		browser->clearMask();
		htmlOverlays = {};
		htmlModalOpen = false;
		programPlacement.restore(programPreview);
		if (nativeEditor) previewPlacement.restore(preview);
		surfacesBorrowed = false;
		unmountWorkspace();
	}
	void resumeFrontend()
	{
		if (!surfacesBorrowed) {
			if (nativeEditor) previewPlacement.take(preview, this);
			surfacesBorrowed = true;
			programPreview = nullptr;
			syncProgramSurface();
		}
		mountWorkspace();
		main->showNormal();
		browser->postMessage(QJsonObject{{"version", 1}, {"event", "viewport.invalidate"}, {"data", QJsonObject{}}});
		publishState(true);
		timer->start();
		meterTimer->start();
		main->raise();
		main->activateWindow();
	}
	explicit OBSWebView2(QMainWindow *parent, const QString &assets, const QString &profile)
		: QWidget(parent), main(parent)
	{
		setObjectName(QStringLiteral("obsWebView2Window"));
		setAcceptDrops(true);
		setProperty("webview2OwnsSession", false);
		setWindowTitle(QStringLiteral("OBS — WebView2"));
		setAttribute(Qt::WA_DeleteOnClose);
		setMinimumSize(160, 120);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		errorLabel = new QLabel(this);
		errorLabel->setWordWrap(true);
		errorLabel->hide();
		layout->addWidget(errorLabel);
		browser = new WebView2Widget(this, assets, profile);
		initializeWebDocks(assets, profile);
		layout->addWidget(browser);
        preview = main->findChild<OBSBasicPreview *>(QStringLiteral("preview"));
        nativeEditor = preview != nullptr;
        if (nativeEditor) {
            previewPlacement.take(preview, this);
            surfacesBorrowed = true;
        } else {
            preview = new OBSQTDisplay(this);
            connect(preview, &OBSQTDisplay::DisplayCreated, this, [this] {
                obs_display_add_draw_callback(preview->GetDisplay(), drawPreview, this);
            });
        }
        surfacesBorrowed = true;
        preview->hide();
        audio = std::make_unique<OBSWeb::AudioMixerBridge>(main);
        meterTimer = new QTimer(this);
        // Match OBS's native VolumeMeter refresh cadence instead of stepping at 20 Hz.
        meterTimer->setTimerType(Qt::PreciseTimer);
        meterTimer->setInterval(16);
        connect(meterTimer, &QTimer::timeout, this, [this] {
            if (property("webview2NativeDocking").toBool()) {
                auto *dock = main->findChild<QDockWidget *>(QStringLiteral("mixerDock"));
                if (!dock || !dock->isVisible()) return;
            }
            postWorkspaceMessage(QJsonObject{{"version", 1}, {"event", "audio.levels"}, {"data", audio->levels()}});
        });
        InstallWebView2Dialogs(main, assets, profile);
		if (auto *tree = main->findChild<SourceTree *>(QStringLiteral("sources")))
			connect(tree, &SourceTree::WebViewEditFinished, this, [this] {
				postWorkspaceMessage({{"version", 1}, {"event", "workspace.rename.finished"}, {"data", QJsonObject{}}});
			});
		mountWorkspace();
		timer = new QTimer(this);
		timer->setInterval(500);
		connect(timer, &QTimer::timeout, this, [this] { publishState(); });
		connect(browser, &WebView2Widget::ready, this, [this] {
			blog(LOG_INFO, "[WebView2] Local interface ready");
			meterTimer->start();
			publishState(true);
			timer->start();
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
			const auto arguments = QCoreApplication::arguments();
			if (IsPluginAuditRequested()) RunPluginAudit(static_cast<OBSBasic *>(main), true);
			else if (arguments.contains(QStringLiteral("--webview2-self-test")) &&
			    arguments.contains(QStringLiteral("--portable")) &&
			    arguments.contains(QStringLiteral("--only-bundled-plugins"))) {
				if (arguments.contains(QStringLiteral("--webview2-parity-audit"))) RunFrontendParityAudit(static_cast<OBSBasic *>(main), true);
				else if (arguments.contains(QStringLiteral("--webview2-persistence-test"))) runPersistenceChecks();
				else runIntegrationChecks();
			}
#endif
		});
		connect(browser, &WebView2Widget::messageReceived, this,
			[this](const QJsonObject &message) { execute(message); });
		connect(browser, &WebView2Widget::externalDrop, this, [this](const QString &id, const OBSWeb::ExternalDropData &drop) {
			const QPointer<OBSWebView2> guard(this);
			reply(id, QJsonObject{{"accepted", true}});
			OBSWeb::DispatchExternalDrop(main, drop);
			if (guard) publishState(true);
		});
		connect(browser, &WebView2Widget::failed, this, [this](const QString &error) {
			if (failedFrontend) return;
			failedFrontend = true;
			timer->stop();
			meterTimer->stop();
			restoreSurfaces();
			setProperty("webview2OwnsSession", false);
			hide();
			main->show();
			main->raise();
			blog(LOG_ERROR, "[WebView2] %s", error.toUtf8().constData());
			QMessageBox::warning(main, QStringLiteral("WebView2"),
				error + QStringLiteral("\nWebView2 is unavailable. OBS remains open. You can retry from Tools → WebView2."));
		});
	}

    ~OBSWebView2() override
    {
        timer->stop();
        meterTimer->stop();
        audio.reset();
        restoreSurfaces();
        destroyWebDocks();
        if (!nativeEditor) {
            if (preview->GetDisplay())
                obs_display_remove_draw_callback(preview->GetDisplay(), drawPreview, this);
            delete preview;
        }
        delete browser;
    }
};
} // namespace

void InstallWebView2Frontend(OBSBasic *window)
{
#ifdef OBS_WEBVIEW2_INTEGRATION_TESTS
	const auto arguments = QCoreApplication::arguments();
	if (arguments.contains(QStringLiteral("--webview2-qt-baseline")) && IsPluginAuditRequested()) {
		QTimer::singleShot(1000, window, [window] { RunPluginAudit(window, false); });
		return;
	}
	if (arguments.contains(QStringLiteral("--webview2-qt-baseline")) &&
	    arguments.contains(QStringLiteral("--webview2-self-test")) &&
	    arguments.contains(QStringLiteral("--portable")) &&
	    arguments.contains(QStringLiteral("--only-bundled-plugins"))) {
		if (arguments.contains(QStringLiteral("--webview2-parity-audit")))
			QTimer::singleShot(1000, window, [window] { RunFrontendParityAudit(window, false); });
		else QTimer::singleShot(4000, window, &QWidget::close);
		return;
	}
#endif
	installThemeWindowFrames();
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("WebView2"));
	action->setProperty("webview2Entry", true);
	QObject::connect(action, &QAction::triggered, window, [window] {
		if (auto *existing = window->findChild<QWidget *>(QStringLiteral("obsWebView2Window"), Qt::FindDirectChildrenOnly)) {
			auto *frontend = static_cast<OBSWebView2 *>(existing);
			if (!frontend->hasFailed()) { frontend->resumeFrontend(); return; }
			delete frontend;
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

bool BeginWebView2Rename(OBSBasic *window, bool source)
{
	if (!window || !window->property("webview2NativeDocking").toBool()) return false;
	auto *frontend = window->findChild<QWidget *>(QStringLiteral("obsWebView2Window"), Qt::FindDirectChildrenOnly);
	return frontend && static_cast<OBSWebView2 *>(frontend)->beginInlineRename(source);
}
