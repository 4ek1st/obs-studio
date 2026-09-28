// Included by the isolated OBS integration harness, outside any class definition.
#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"
#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QKeyEvent>
#include <QSpinBox>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSaveFile>
#include <QTimer>
#include <functional>
#include "capture-integration.inl"

namespace {
class DialogWorkflowChecks : public QObject {
	QPointer<OBSBasic> main;
	std::function<void(bool, const char *)> check;
	std::function<void()> done;
	QString sceneName = QStringLiteral("WebView Dialog Scene %1").arg(QDateTime::currentMSecsSinceEpoch());
	QString filterName = QStringLiteral("WebView Color Correction");
	QString sourceUuid;
	QPointer<QDialog> filters;
	bool completed = false;
	bool originalWarning = false;
	int settingsCategory = 0;

	void finish()
	{
		if (completed) return;
		completed = true;
		done();
		deleteLater();
	}

	void fail(const char *message)
	{
		check(false, message);
		// Close only dialogs in this isolated test process to release nested exec loops.
		for (auto *widget : QApplication::topLevelWidgets())
			if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible()) dialog->reject();
		finish();
	}

	static QDialog *shownDialog(const QByteArray &type)
	{
		for (auto *widget : QApplication::topLevelWidgets())
			if (auto *dialog = qobject_cast<QDialog *>(widget); dialog && dialog->isVisible() && dialog->inherits(type.constData())) return dialog;
		return nullptr;
	}

	void capture(QDialog *dialog, const QString &name, std::function<void()> next)
	{
		const auto directory = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
		if (directory.isEmpty()) { next(); return; }
		QDir().mkpath(directory);
		QPointer<QDialog> target(dialog);
		QPointer<DialogWorkflowChecks> guard(this);
		QTimer::singleShot(180, this, [guard, target, directory, name, next] {
			if (!guard || !target) return;
			auto *web = target->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
			if (!web) { guard->fail("Dialog WebView2 surface is available for capture"); return; }
			guard->check(web->geometry() == target->rect(), "WebView2 dialog overlay covers the complete native content rectangle");
			const auto expected = target->size() * target->devicePixelRatioF();
			const auto path = QDir(directory).filePath(name + ".png");
			if (name == QStringLiteral("OBSBasicSourceSelect")) {
				// Pair the two renderers on the exact same live dialog and layout.
				// No event loop turn occurs while the web child is temporarily hidden.
				// GPU widgets are not hidden/reparented by this diagnostic.
				const bool visible = web->isVisible();
				const QPointer<QWidget> focused = QApplication::focusWidget();
				web->hide();
				const auto native = target->grab();
				if (visible) { web->show(); web->raise(); }
				if (focused && focused->isVisible()) focused->setFocus(Qt::OtherFocusReason);
				guard->check(native.save(QDir(directory).filePath(name + "-native.png")),
					"Native source picker appearance is captured beside the same HTML dialog");
				OBSWeb::QtDialogBridge bridge(target);
				QSaveFile snapshot(QDir(directory).filePath(name + ".json"));
				const auto bytes = QJsonDocument(bridge.snapshot()).toJson();
				guard->check(snapshot.open(QIODevice::WriteOnly) && snapshot.write(bytes) == bytes.size() && snapshot.commit(),
					"Source picker widget snapshot is saved for exact native and HTML comparison");
			}
			web->capturePreview(path, [guard, next, expected, path](bool saved) {
				if (!guard) return;
				guard->check(saved, "Actual WebView2 dialog rendering is captured successfully");
				const auto captured = QImageReader(path).size();
				guard->check(saved && std::abs(captured.width() - expected.width()) <= 2 && std::abs(captured.height() - expected.height()) <= 2,
					"Actual dialog image dimensions match the full native client size");
				next();
			});
		});
	}

	void waitDialog(const QByteArray &type, const std::function<void(QDialog *)> &next)
	{
		auto *poll = new QTimer(this);
		auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
		poll->setInterval(100);
		connect(poll, &QTimer::timeout, this, [this, poll, elapsed, type, next] {
			if (completed) { poll->stop(); poll->deleteLater(); return; }
			auto *dialog = shownDialog(type);
			auto *web = dialog ? dialog->findChild<WebView2Widget *>(QStringLiteral("obsWebView2DialogSurface"), Qt::FindDirectChildrenOnly) : nullptr;
			if (dialog && web && web->isVisible()) {
				poll->stop(); poll->deleteLater();
				check(web->parentWidget() == dialog && dialog->findChildren<WebView2Widget *>(QString(), Qt::FindDirectChildrenOnly).size() == 1,
				      "dialog HTML overlay has the original QDialog parent and no duplicate host");
				const QPointer<QDialog> target(dialog);
				capture(dialog, QString::fromLatin1(type), [this, target, next] {
					if (target) next(target); else fail("Native dialog stays alive during capture");
				});
			} else if (elapsed->elapsed() > 25000) {
				poll->stop(); poll->deleteLater();
				const auto message = QStringLiteral("dialog workflow timed out waiting for %1 and its WebView2 document").arg(QString::fromLatin1(type)).toUtf8();
				fail(message.constData());
			}
		});
		poll->start();
	}

	void action(const char *name)
	{
		if (!main) { fail("dialog workflow main window expired"); return; }
		auto *target = main->findChild<QAction *>(QString::fromLatin1(name));
		if (!target) { fail("requested native OBS action was not found"); return; }
		QTimer::singleShot(0, target, [target] { target->trigger(); });
	}

	static QJsonObject findNode(const QJsonObject &state, const char *property, const QJsonValue &value)
	{
		for (const auto entry : state.value("nodes").toArray())
			if (entry.toObject().value(QLatin1String(property)) == value) return entry.toObject();
		return {};
	}

	bool command(OBSWeb::QtDialogBridge &bridge, const char *name, const QJsonObject &args, const char *label)
	{
		QString error;
		const bool ok = bridge.execute(QString::fromLatin1(name), args, error);
		check(ok, label);
		return ok;
	}

	bool button(QDialog *dialog, OBSWeb::QtDialogBridge &bridge, QDialogButtonBox::StandardButton role)
	{
		auto *box = dialog->findChild<QDialogButtonBox *>();
		auto *target = box ? box->button(role) : nullptr;
		if (!target) { check(false, "dialog standard action button exists"); return false; }
		// Give this fixture-owned lookup a temporary name; the renderer still executes the native button.
		const auto oldName = target->objectName();
		target->setObjectName(QStringLiteral("dialogWorkflowAction"));
		const auto node = findNode(bridge.snapshot(), "name", "dialogWorkflowAction");
		target->setObjectName(oldName);
		return command(bridge, "dialog.click", {{"id", node.value("id")}}, "HTML dialog command invokes the native standard button");
	}

	void sourceSelect()
	{
		if (!main) { finish(); return; }
		const auto scene = main->GetCurrentSceneSource();
		check(scene && QString::fromUtf8(obs_source_get_name(scene)) == sceneName, "native Add Scene creates the scene named in the HTML field");
		waitDialog("OBSBasicSourceSelect", [this](QDialog *dialog) {
			OBSWeb::QtDialogBridge bridge(dialog);
			const auto state = bridge.snapshot();
			const auto list = findNode(state, "name", "sourceTypeList");
			auto *nativeList = dialog->findChild<QListWidget *>(QStringLiteral("sourceTypeList"));
			int colorRow = -1;
			if (nativeList) for (int row = 0; row < nativeList->count(); ++row)
				if (nativeList->item(row)->data(Qt::UserRole + 1).toString() == "color_source") colorRow = row;
			QJsonObject color;
			for (const auto item : list.value("items").toArray()) if (item.toObject().value("row").toInt(-1) == colorRow) color = item.toObject();
			if (color.isEmpty()) { fail("color source type must be visible and available in native source picker"); return; }
			if (!command(bridge, "dialog.item", {{"id", list.value("id")}, {"item", color.value("id")}, {"action", "select"}}, "HTML source type selection reaches OBS source picker")) { finish(); return; }
			const auto create = findNode(bridge.snapshot(), "name", "createNewSource");
			if (create.isEmpty()) { fail("source picker exposes Create New after selecting color type"); return; }
			waitDialog("OBSBasicProperties", [this](QDialog *properties) { sourceProperties(properties); });
			command(bridge, "dialog.click", {{"id", create.value("id")}}, "native source picker creates color source and opens its properties");
		});
		action("actionAddSource");
	}

	void sourceProperties(QDialog *dialog)
	{
		obs_source_t *source = nullptr;
		obs_scene_enum_items(obs_scene_from_source(main->GetCurrentSceneSource()),
			[](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
				if (!obs_sceneitem_selected(item)) return true;
				*static_cast<obs_source_t **>(opaque) = obs_sceneitem_get_source(item);
				return false;
			}, &source);
		check(source && QByteArray(obs_source_get_unversioned_id(source)) == "color_source", "Create New adds the real color source to the current scene");
		if (!source) { fail("new source is selected for properties"); return; }
		sourceUuid = QString::fromUtf8(obs_source_get_uuid(source));
		OBSWeb::QtDialogBridge bridge(dialog);
		const auto state = bridge.snapshot();
		const auto width = findNode(state, "type", "number");
		if (!width.isEmpty()) {
			command(bridge, "dialog.input", {{"id", width.value("id")}, {"value", 854}}, "HTML number updates real dynamic color source properties");
			command(bridge, "dialog.finish", {{"id", width.value("id")}}, "dynamic property edit emits its commit signal");
		} else {
			QSpinBox *spin = nullptr;
			for (auto *candidate : dialog->findChildren<QSpinBox *>())
				if (candidate->isVisible() && candidate->isEnabled() && !candidate->isReadOnly()) { spin = candidate; break; }
			if (!spin) { fail("color source dynamic properties contain an editable native spinbox"); return; }
			const auto oldName = spin->objectName(); spin->setObjectName("dialogWorkflowWidth");
			const auto native = findNode(bridge.snapshot(), "name", "dialogWorkflowWidth"); spin->setObjectName(oldName);
			auto *surface = dialog->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
			check(native.value("type") == "native" && surface && !surface->mask().contains(spin->mapTo(dialog, spin->rect().center())),
				"custom OBS width editor is a reachable native island in the WebView dialog");
			auto *editor = spin->findChild<QLineEdit *>();
			if (!editor) { fail("native width spinbox exposes its original text editor"); return; }
			editor->selectAll(); editor->insert("854");
			QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(spin, &enter);
			check(spin->value() == 854 && dialog->isVisible(), "original OBS spinbox commits width without accepting its properties dialog");
		}
		button(dialog, bridge, QDialogButtonBox::Ok);
		QTimer::singleShot(150, this, [this] {
			OBSSourceAutoRelease current = obs_get_source_by_uuid(sourceUuid.toUtf8().constData());
			OBSDataAutoRelease settings = current ? obs_source_get_settings(current) : nullptr;
			check(settings && obs_data_get_int(settings, "width") == 854, "accepted properties persist through the source's real OBS settings");
			waitDialog("OBSBasicFilters", [this](QDialog *dialog) { filterDialog(dialog); });
			// This supported frontend API supplies the source; actionOpenSourceFilters expects a QAction sender.
			if (current) obs_frontend_open_source_filters(current);
			else fail("created source expired before opening its filters");
		});
	}

	void filterDialog(QDialog *dialog)
	{
		filters = dialog;
		OBSWeb::QtDialogBridge bridge(dialog);
		const auto add = findNode(bridge.snapshot(), "name", "addEffectFilter");
		if (add.isEmpty()) { fail("filter dialog exposes its Add Effect Filter button"); return; }
		auto *poll = new QTimer(this); poll->setInterval(100);
		auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
		connect(poll, &QTimer::timeout, this, [this, poll, elapsed] {
			if (completed) { poll->stop(); poll->deleteLater(); return; }
			QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
			if (menu && menu->isVisible()) {
				QAction *color = nullptr;
				for (auto *candidate : menu->actions()) if (candidate->data().toString().startsWith("color_filter")) color = candidate;
				if (!color) { poll->stop(); poll->deleteLater(); fail("native filter popup exposes color correction"); return; }
				poll->stop(); poll->deleteLater();
				check(true, "native filter popup remains operable above the dialog HTML surface");
				waitDialog("NameDialog", [this](QDialog *nameDialog) {
					OBSWeb::QtDialogBridge names(nameDialog);
					const auto field = findNode(names.snapshot(), "type", "text");
					command(names, "dialog.input", {{"id", field.value("id")}, {"value", filterName}}, "nested HTML filter naming uses the original Qt dialog");
					button(nameDialog, names, QDialogButtonBox::Ok);
					QTimer::singleShot(200, this, [this] { verifyFilter(); });
				});
				menu->close();
				color->trigger();
			} else if (elapsed->elapsed() > 10000) { poll->stop(); poll->deleteLater(); fail("filter popup did not appear"); }
		});
		poll->start();
		command(bridge, "dialog.click", {{"id", add.value("id")}}, "HTML Add Filter invokes the native filter menu");
	}

	void verifyFilter()
	{
		OBSSourceAutoRelease source = obs_get_source_by_uuid(sourceUuid.toUtf8().constData());
		OBSSourceAutoRelease filter = source ? obs_source_get_filter_by_name(source, filterName.toUtf8().constData()) : nullptr;
		check(bool(filter), "native filter controller creates the named color correction filter");
		if (filters) {
			OBSWeb::QtDialogBridge bridge(filters);
			const auto state = bridge.snapshot();
			bool dynamicProperties = false;
			for (const auto entry : state.value("nodes").toArray()) {
				const auto control = entry.toObject();
				if (control.value("type") == "number" || (control.value("type") == "native" &&
					(control.value("class") == "OBS::SpinBox" || control.value("class") == "OBS::DoubleSpinBox"))) dynamicProperties = true;
			}
			check(dynamicProperties, "added filter's dynamic numeric properties retain HTML or original native editors");
			button(filters, bridge, QDialogButtonBox::Close);
		}
		originalWarning = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStartingStream");
		waitDialog("OBSBasicSettings", [this](QDialog *settings) { settingsPage(settings); });
		action("action_Settings");
	}

	void settingsPage(QDialog *dialog)
	{
		if (!dialog) { fail("settings dialog unexpectedly closed"); return; }
		OBSWeb::QtDialogBridge bridge(dialog);
		const auto state = bridge.snapshot();
		if (settingsCategory == 0) {
			const auto warning = findNode(state, "name", "warnBeforeStreamStart");
			if (warning.isEmpty()) { fail("General settings warning checkbox is rendered"); return; }
			command(bridge, "dialog.click", {{"id", warning.value("id")}}, "HTML settings edit uses the existing change tracking");
		}
		const auto categories = findNode(state, "name", "listWidget");
		const auto items = categories.value("items").toArray();
		if (++settingsCategory < items.size()) {
			const auto item = items.at(settingsCategory).toObject();
			command(bridge, "dialog.item", {{"id", categories.value("id")}, {"item", item.value("id")}, {"action", "select"}},
				"HTML settings category selection updates the native settings page");
			const QPointer<QDialog> guard(dialog);
			QTimer::singleShot(180, this, [this, guard] { settingsPage(guard); });
			return;
		}
		check(settingsCategory >= 9, "all nine OBS settings categories are reachable through the HTML bridge");
		button(dialog, bridge, QDialogButtonBox::Cancel);
		QTimer::singleShot(200, this, [this] {
			check(config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStartingStream") == originalWarning,
			      "Cancel discards edited settings without changing the stored configuration");
			waitDialog("OBSBasicAdvAudio", [this](QDialog *audio) {
				OBSWeb::QtDialogBridge bridge(audio);
				check(!bridge.snapshot().value("nodes").toArray().isEmpty(), "Advanced Audio Properties has a live HTML widget snapshot");
				command(bridge, "dialog.key", {{"key", "Escape"}}, "Advanced Audio Properties closes through native Escape handling");
				RunCaptureDialogChecks(main, check, [this] { finish(); });
			});
			action("actionAdvAudioProperties");
		});
	}

public:
	DialogWorkflowChecks(OBSBasic *window, std::function<void(bool, const char *)> report, std::function<void()> completed)
		: QObject(window), main(window), check(std::move(report)), done(std::move(completed)) {}

	void run()
	{
		waitDialog("NameDialog", [this](QDialog *dialog) {
			OBSWeb::QtDialogBridge bridge(dialog);
			const auto field = findNode(bridge.snapshot(), "type", "text");
			if (!command(bridge, "dialog.input", {{"id", field.value("id")}, {"value", sceneName}}, "HTML scene naming updates the native NameDialog field")) { finish(); return; }
			button(dialog, bridge, QDialogButtonBox::Ok);
			QTimer::singleShot(150, this, [this] { sourceSelect(); });
		});
		action("actionAddScene");
	}
};
} // namespace

static void RunDialogWorkflowChecks(OBSBasic *main, const std::function<void(bool, const char *)> &check,
				    const std::function<void()> &done)
{
	(new DialogWorkflowChecks(main, check, done))->run();
}
