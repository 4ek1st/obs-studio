// Disposable integration-only workflows. The caller supplies an explicit QA video
// and artifacts directory; no production recording/profile paths are discovered.
#include <models/SceneCollection.hpp>
#include <QAbstractItemModel>
#include <QAbstractItemDelegate>
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <functional>

namespace {
class ToolDataWorkflowChecks final : public QObject {
    QPointer<OBSBasic> main;
    QPointer<QDialog> current;
    bool web;
    bool finished = false;
    bool cancellingRemux = false;
    bool importFlagsSaved = false;
    bool hadPrompt = false, hadSearch = false, oldPrompt = false, oldSearch = false;
    QString directory, video, output, collectionName, renamedCollection, originalCollection;
    QJsonObject fixture;
    QTimer timer;
    QElapsedTimer elapsed;
    std::function<void(bool, const char *)> check;
    std::function<void()> done;

    QJsonObject nodeFor(OBSWeb::QtDialogBridge &bridge, QWidget *widget) {
        if (!widget || !current || !current->isAncestorOf(widget)) return {};
        const QRect expected(widget->mapTo(current, QPoint()), widget->size());
        for (const auto &entry : bridge.snapshot().value("nodes").toArray()) {
            const auto node = entry.toObject(), rect = node.value("rect").toObject();
            const QRect actual(rect.value("x").toInt(), rect.value("y").toInt(), rect.value("width").toInt(), rect.value("height").toInt());
            if (actual == expected && node.value("class").toString() == QString::fromLatin1(widget->metaObject()->className())) return node;
        }
        return {};
    }
    QJsonObject cell(OBSWeb::QtDialogBridge &bridge, QTableView *table, int row, int column) {
        for (const auto &entry : nodeFor(bridge, table).value("items").toArray()) {
            const auto item = entry.toObject();
            if (item.value("row") == row && item.value("column") == column) return item;
        }
        return {};
    }
    bool click(QAbstractButton *button) {
        if (!button || !button->isVisible() || !button->isEnabled()) return false;
        if (!web) { button->click(); return true; }
        OBSWeb::QtDialogBridge bridge(current); QString error;
        return bridge.execute("dialog.click", {{"id", nodeFor(bridge, button).value("id")}}, error);
    }
    bool button(QDialogButtonBox::StandardButton which) {
        auto *box = current ? current->findChild<QDialogButtonBox *>() : nullptr;
        return box && click(box->button(which));
    }
    void waitFor(std::function<bool()> predicate, std::function<void()> success, int limit,
                 const char *failure, std::function<void()> otherwise) {
        timer.stop(); disconnect(&timer, nullptr, this, nullptr); elapsed.restart();
        connect(&timer, &QTimer::timeout, this, [this, predicate, success, limit, failure, otherwise] {
            if (predicate()) { timer.stop(); QTimer::singleShot(0, this, success); }
            else if (elapsed.elapsed() > limit) { timer.stop(); check(false, failure); otherwise(); }
        });
        timer.start(80);
    }
    void open(const char *actionName, const char *type, std::function<void()> opened, std::function<void()> failed) {
        current = nullptr;
        auto *action = main ? main->findChild<QAction *>(QString::fromLatin1(actionName)) : nullptr;
        check(action && action->isEnabled(), "Tool data workflow uses an enabled original QAction");
        if (!action || !action->isEnabled()) { failed(); return; }
        waitFor([this, type] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || !dialog->inherits(type)) continue;
                current = dialog;
                if (!web) return true;
                auto *surface = dialog->findChild<WebView2Widget *>("obsWebView2DialogSurface");
                return surface && surface->isVisible();
            }
            return false;
        }, std::move(opened), 15000, "Tool data dialog opens and its requested frontend is ready", std::move(failed));
        QTimer::singleShot(0, action, &QAction::trigger);
    }
    bool dropFile(const QString &path) {
        QString error;
        const auto data = OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}}, {path}, error);
        if (!current || !data) return false;
        if (web) { OBSWeb::QtDialogBridge bridge(current); return bridge.drop(*data, error); }
        return OBSWeb::DispatchExternalDrop(current, *data);
    }
    void editCell(QTableView *table, int row, int column, const QString &text, std::function<void(bool)> edited) {
        if (!current || !table) { edited(false); return; }
        const QPointer<QTableView> view(table);
        if (web) {
            OBSWeb::QtDialogBridge bridge(current); QString error;
            if (!bridge.execute("dialog.item", {{"id", nodeFor(bridge, table).value("id")},
                    {"item", cell(bridge, table, row, column).value("id")}, {"action", "select"}}, error)) { edited(false); return; }
        } else { table->setCurrentIndex(table->model()->index(row, column)); }
        QTimer::singleShot(160, this, [this, view, row, column, text, edited] {
            if (!current || !view) { edited(false); return; }
            QPointer<QLineEdit> editor;
            for (auto *candidate : view->findChildren<QLineEdit *>()) if (candidate->isVisible()) { editor = candidate; break; }
            if (!editor) { edited(false); return; }
            bool ok = true;
            if (web) {
                OBSWeb::QtDialogBridge bridge(current); QString error;
                const auto id = nodeFor(bridge, editor).value("id");
                ok = bridge.execute("dialog.input", {{"id", id}, {"value", text}}, error) &&
                     bridge.execute("dialog.finish", {{"id", id}}, error);
            } else {
                editor->selectAll(); editor->insert(text);
                // Match clicking the action after editing: an actual focus
                // change commits the native delegate.
                auto *box = current->findChild<QDialogButtonBox *>();
                QWidget *target = box && box->button(QDialogButtonBox::Ok) ?
                    static_cast<QWidget *>(box->button(QDialogButtonBox::Ok)) : static_cast<QWidget *>(view.data());
                target->setFocus(Qt::OtherFocusReason);
            }
            QTimer::singleShot(0, this, [view, row, column, text, edited, ok] {
                edited(ok && view && view->model()->index(row, column).data().toString() == text);
            });
        });
    }
    void closeTool(std::function<void()> next) {
        if (current) {
            if (!button(QDialogButtonBox::Close)) current->reject();
        }
        current = nullptr;
        QTimer::singleShot(100, this, std::move(next));
    }
    void acknowledgeRemuxCompletion() {
        auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (current && current->inherits("OBSRemux") && message && message->parentWidget() == current &&
            message->windowTitle() == QTStr("Remux.FinishedTitle")) {
            // OBSMessageBox::information uses a custom AcceptRole button, not
            // QMessageBox::Ok. Acknowledge only this tool's exact completion.
            const auto buttons = message->buttons();
            if (buttons.size() == 1 && message->buttonRole(buttons.front()) == QMessageBox::AcceptRole)
                buttons.front()->click();
        }
    }
    void cancelTimedOutRemux() {
        const QPointer<QDialog> remux(current);
        if (!remux || !remux->inherits("OBSRemux")) { finish(); return; }
        acknowledgeRemuxCompletion();
        waitFor([this, remux] {
            // stopRemux() owns a parentless critical Yes/No box. Only answer
            // while our own reject() call is on its stack, with its exact title.
            auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (cancellingRemux && message && !message->parentWidget() &&
                message->windowTitle() == QTStr("Remux.ExitUnfinishedTitle") &&
                message->standardButtons() == (QMessageBox::Yes | QMessageBox::No))
                message->button(QMessageBox::Yes)->click();
            return !remux || !remux->isVisible();
        }, [this] { current = nullptr; finish(); }, 5000,
            "Timed-out fixture Remux stops and closes through its own cancellation controller", [this] { finish(); });
        const QPointer<ToolDataWorkflowChecks> guard(this);
        QTimer::singleShot(0, this, [guard, remux] {
            if (!guard || !remux) return;
            guard->cancellingRemux = true;
            remux->reject();
            if (guard) guard->cancellingRemux = false;
        });
    }
    void remux() {
        if (!QFileInfo(video).isFile() || QFileInfo(video).size() < 1024) {
            check(false, "An explicit disposable video fixture is available for real remux"); importer(); return;
        }
        open("actionRemux", "OBSRemux", [this] {
            auto *table = current->findChild<QTableView *>();
            if (!table) { check(false, "Original remux table exists"); closeTool([this] { importer(); }); return; }
            if (table->model()->rowCount() > 1) button(QDialogButtonBox::RestoreDefaults);
            const bool delivered = dropFile(video);
            check(delivered && table->model()->rowCount() == 2 &&
                QFileInfo(table->model()->index(0, 1).data().toString()) == QFileInfo(video),
                "File drop populates the original Remux queue with the real video");
            if (!delivered) { closeTool([this] { importer(); }); return; }
            editCell(table, 0, 2, output, [this](bool edited) {
                check(edited, "Remux output is changed through the original composite path editor and committed");
                if (!edited) { closeTool([this] { importer(); }); return; }
                const bool started = button(QDialogButtonBox::Ok);
                check(started, "Original Remux button starts the real media worker");
                if (!started) { closeTool([this] { importer(); }); return; }
                waitFor([this] {
                    acknowledgeRemuxCompletion();
                    auto *view = current ? current->findChild<QTableView *>() : nullptr;
                    // Complete is set before the modal completion notice. The
                    // original controller restores drops only after it returns.
                    return view && current->acceptDrops() && view->model()->rowCount() >= 1 &&
                        view->model()->index(0, 0).data(Qt::UserRole).toInt() == 4;
                }, [this] {
                    QFile result(output); const bool readable = result.open(QIODevice::ReadOnly);
                    const auto header = readable ? result.read(12) : QByteArray();
                    check(readable && result.size() > 1024 && header.mid(4, 4) == "ftyp",
                        "Real Remux completes and writes a nonempty MP4 container at the edited destination");
                    closeTool([this] { importer(); });
                }, 30000, "Real remux reaches native Complete state", [this] { cancelTimedOutRemux(); });
            });
        }, [this] { importer(); });
    }
    void importer() {
        auto *config = obs_frontend_get_user_config();
        importFlagsSaved = true;
        hadPrompt = config_has_user_value(config, "General", "AutoSearchPrompt");
        hadSearch = config_has_user_value(config, "General", "AutomaticCollectionSearch");
        oldPrompt = config_get_bool(config, "General", "AutoSearchPrompt");
        oldSearch = config_get_bool(config, "General", "AutomaticCollectionSearch");
        // Avoid discovery of other installed applications' collections in this fixture.
        config_set_bool(config, "General", "AutoSearchPrompt", true);
        config_set_bool(config, "General", "AutomaticCollectionSearch", false);
        open("actionImportSceneCollection", "OBSImporter", [this] {
            auto *emptyTable = current->findChild<QTableView *>();
            auto *box = current->findChild<QDialogButtonBox *>();
            check(box && box->button(QDialogButtonBox::Ok)->isEnabled(),
                "Empty Import dialog preserves the original initial Import action after frontend focus transfer");
            check(unchangedImportPath(emptyTable, 0),
                "Committing the unchanged empty Import path does not emit a model change");
            const bool delivered = dropFile(QDir(directory).filePath("collection.json"));
            auto *table = current->findChild<QTableView *>();
            check(delivered && table && table->model()->rowCount() == 2 &&
                table->model()->index(0, 1).data().toString() == collectionName &&
                table->model()->index(0, 0).data(Qt::CheckStateRole).toInt() == Qt::Checked,
                "File drop is recognized and selected by the original scene-collection importer");
            if (!delivered || !table) { closeTool([this] { finish(); }); return; }
            editCell(table, 0, 1, renamedCollection, [this](bool edited) {
                check(edited, "Importer collection name is changed through its native model editor");
                if (!edited) { closeTool([this] { finish(); }); return; }
                auto *view = current->findChild<QTableView *>();
                check(unchangedImportPath(view, 0) && view->model()->index(0, 1).data().toString() == renamedCollection &&
                    view->model()->index(0, 0).data(Qt::CheckStateRole).toInt() == Qt::Checked,
                    "Leaving an unchanged Import path preserves the edited name and selection without a model change");
                check(button(QDialogButtonBox::Ok), "Original Import button executes the real collection importer");
                waitFor([this] { return main && main->GetSceneCollectionByName(renamedCollection.toStdString()).has_value(); }, [this] {
                    const auto imported = main->GetSceneCollectionByName(renamedCollection.toStdString());
                    QFile saved(QString::fromStdString(imported->getFilePathString()));
                    const bool opened = saved.open(QIODevice::ReadOnly);
                    const auto data = opened ? QJsonDocument::fromJson(saved.readAll()).object() : QJsonObject();
                    check(opened && data.value("name") == renamedCollection && data.value("sources") == fixture.value("sources") &&
                        data.value("scene_order") == fixture.value("scene_order") && data.value("current_scene") == fixture.value("current_scene"),
                        "Imported collection on disk preserves source UUIDs, settings, item transforms, order and edited name");
                    check(QString::fromStdString(main->GetCurrentSceneCollection().getName()) == originalCollection,
                        "Import does not replace the active scene collection");
                    finish();
                }, 10000, "Imported collection appears in the native collection catalog", [this] { finish(); });
            });
        }, [this] { finish(); });
    }
    bool unchangedImportPath(QTableView *table, int row) {
        if (!table || !table->model()) return false;
        const auto index = table->model()->index(row, 2);
        auto *delegate = table->itemDelegateForColumn(2);
        if (!delegate) return false;
        auto *editor = delegate->createEditor(table, QStyleOptionViewItem(), index);
        if (!editor) return false;
        delegate->setEditorData(editor, index);
        int changes = 0;
        const auto connection = connect(table->model(), &QAbstractItemModel::dataChanged,
            this, [&changes] { ++changes; });
        delegate->setModelData(editor, table->model(), index);
        disconnect(connection);
        delete editor;
        return changes == 0;
    }
    void finish() {
        if (finished) return;
        finished = true; timer.stop();
        if (importFlagsSaved) {
            auto *config = obs_frontend_get_user_config();
            if (hadPrompt) config_set_bool(config, "General", "AutoSearchPrompt", oldPrompt); else config_remove_value(config, "General", "AutoSearchPrompt");
            if (hadSearch) config_set_bool(config, "General", "AutomaticCollectionSearch", oldSearch); else config_remove_value(config, "General", "AutomaticCollectionSearch");
        }
        const auto completed = done; deleteLater(); completed();
    }
public:
    ToolDataWorkflowChecks(OBSBasic *window, bool bridge, std::function<void(bool, const char *)> report, std::function<void()> completed)
        : QObject(window), main(window), web(bridge), check(std::move(report)), done(std::move(completed)) {}
    void run() {
        const auto base = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
        if (!main || !QFileInfo(base).isAbsolute() || !QFileInfo(base).isDir()) { check(false, "Tool workflow has an explicit QA artifacts directory"); finish(); return; }
        const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        directory = QDir(base).filePath("tools-" + token); QDir().mkpath(directory);
        video = qEnvironmentVariable("OBS_WEBVIEW2_PARITY_FIXTURE_VIDEO");
        output = QDir(directory).filePath("remux-result.mp4");
        collectionName = "Parity import " + token; renamedCollection = collectionName + " renamed";
        originalCollection = QString::fromStdString(main->GetCurrentSceneCollection().getName());
        const auto sceneId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto colorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject color{{"name", "Imported color"}, {"uuid", colorId}, {"id", "color_source_v3"},
            {"settings", QJsonObject{{"width", 854}, {"height", 480}, {"color", 4281558681.0}}}};
        const QJsonObject item{{"id", 1}, {"name", "Imported color"}, {"source_uuid", colorId}, {"visible", true}, {"locked", true},
            {"pos", QJsonObject{{"x", 12.5}, {"y", 25.0}}}, {"scale", QJsonObject{{"x", 0.75}, {"y", 0.5}}}, {"rot", 15.0}};
        const QJsonObject scene{{"name", "Imported scene"}, {"uuid", sceneId}, {"id", "scene"}, {"settings", QJsonObject{{"items", QJsonArray{item}}}}};
        fixture = {{"name", collectionName}, {"current_scene", "Imported scene"}, {"current_program_scene", "Imported scene"},
            {"sources", QJsonArray{color, scene}}, {"scene_order", QJsonArray{QJsonObject{{"name", "Imported scene"}}}}};
        QSaveFile source(QDir(directory).filePath("collection.json"));
        const bool opened = source.open(QIODevice::WriteOnly);
        if (opened) source.write(QJsonDocument(fixture).toJson());
        if (!opened || !source.commit()) { check(false, "Disposable scene collection JSON is written"); finish(); return; }
        remux();
    }
};

static void RunToolDataWorkflowChecks(OBSBasic *main, bool useBridge,
        const std::function<void(bool, const char *)> &check, const std::function<void()> &done)
{
    (new ToolDataWorkflowChecks(main, useBridge, check, done))->run();
}
} // namespace
