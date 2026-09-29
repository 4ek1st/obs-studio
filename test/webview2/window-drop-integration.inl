// Exercise an external window move event onto the OBS preview. Included only in
// the disposable portable integration build.
#include <QComboBox>
#include <QInputDialog>
#include <QProcess>
#include <QTimer>
#include <Windows.h>

namespace {
class WindowDropChecks final : public QObject {
    QPointer<OBSBasic> main;
    QPointer<QWidget> preview;
    std::function<void(bool, const char *)> check;
    std::function<void()> done;
    QProcess fixture;
    OBSSceneAutoRelease scene{obs_scene_create("Window drop integration scene")};
    OBSSourceAutoRelease lower{obs_source_create("color_source_v3", "Window drop lower", nullptr, nullptr)};
    OBSSourceAutoRelease upper{obs_source_create("color_source_v3", "Window drop upper", nullptr, nullptr)};
    OBSSource dropped;
    RECT beforeMove{};
    int dialogWaits = 0;
    bool moveReported = false;
    bool finished = false;

    static HWND findFixture(qint64 pid)
    {
        struct Search { DWORD pid; HWND window = nullptr; } search{DWORD(pid)};
        EnumWindows([](HWND window, LPARAM value) -> BOOL {
            auto &search = *reinterpret_cast<Search *>(value);
            DWORD candidate = 0;
            GetWindowThreadProcessId(window, &candidate);
            if (candidate == search.pid && IsWindowVisible(window)) {
                search.window = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.window;
    }

    void finish()
    {
        if (finished) return;
        finished = true;
        if (fixture.state() != QProcess::NotRunning) {
            fixture.kill();
            fixture.waitForFinished(3000);
        }
        if (dropped) obs_source_remove(dropped);
        if (lower) obs_source_remove(lower);
        if (upper) obs_source_remove(upper);
        if (scene) obs_source_remove(obs_scene_get_source(scene));
        done();
        deleteLater();
    }

    void poll(const std::function<bool()> &condition, const char *assertion, const std::function<void()> &next,
              int remaining = 50)
    {
        if (condition()) { check(true, assertion); next(); return; }
        if (remaining == 0) { check(false, assertion); finish(); return; }
        QTimer::singleShot(200, this, [this, condition, assertion, next, remaining] {
            poll(condition, assertion, next, remaining - 1);
        });
    }

    void afterDrop()
    {
        if (!moveReported) {
            RECT afterMove{}, previewRect{}, overlap{};
            const HWND target = findFixture(fixture.processId());
            check(target && GetWindowRect(target, &afterMove) &&
                  (beforeMove.left != afterMove.left || beforeMove.top != afterMove.top),
                  "External fixture window moves onto OBS preview");
            check(preview && GetWindowRect(reinterpret_cast<HWND>(preview->winId()), &previewRect) &&
                  IntersectRect(&overlap, &afterMove, &previewRect) &&
                  overlap.right - overlap.left >= 48 && overlap.bottom - overlap.top >= 48,
                  "Fixture overlaps the native preview by at least 48 pixels");
            moveReported = true;
        }
        QInputDialog *dialog = nullptr;
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *input = qobject_cast<QInputDialog *>(widget);
                input && input->objectName() == QStringLiteral("obsWindowDropLayerDialog") && input->isVisible() &&
                input->labelText().contains(QStringLiteral("OBS capture QA target"))) {
                dialog = input;
                break;
            }
        }
        if (!dialog && dialogWaits++ < 50) {
            QTimer::singleShot(200, this, [this] { afterDrop(); });
            return;
        }
        check(dialog != nullptr, "Moving an external window onto OBS opens layer selection");
        if (!dialog) { finish(); return; }
        auto *layers = dialog->findChild<QComboBox *>();
        check(layers && layers->count() == 3, "Layer selector offers top and positions below both existing layers");
        if (!layers || layers->count() != 3) { dialog->reject(); finish(); return; }
        layers->setCurrentIndex(1); // below the top layer, above the lower layer
        dialog->accept();

        poll([this] {
            obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *value) {
                auto *self = static_cast<WindowDropChecks *>(value);
                auto *source = obs_sceneitem_get_source(item);
                if (strcmp(obs_source_get_id(source), "window_capture") == 0) self->dropped = source;
                return true;
            }, this);
            return bool(dropped);
        }, "Dropped window creates a real Window Capture source", [this] {
            auto *item = obs_scene_find_source(scene, obs_source_get_name(dropped));
            check(item && obs_sceneitem_get_order_position(item) == 1,
                  "The chosen layer is between the two existing layers");
            const auto settings = obs_source_get_settings(dropped);
            const QString selector = QString::fromUtf8(obs_data_get_string(settings, "window"));
            check(selector.contains(QStringLiteral("obs-capture-fixture.exe"), Qt::CaseInsensitive),
                  "The persistent source targets the external program");
            obs_data_release(settings);
            poll([this] { return obs_source_get_width(dropped) == 640 && obs_source_get_height(dropped) == 360; },
                 "Dropped source renders the real 640x360 fixture", [this] {
                const auto uuid = QByteArray(obs_source_get_uuid(dropped));
                const HWND target = findFixture(fixture.processId());
                if (target) PostMessageW(target, WM_CLOSE, 0, 0);
                poll([this] { return obs_source_get_width(dropped) == 0; },
                     "Source becomes blank after its window closes", [this, uuid] {
                    check(QByteArray(obs_source_get_uuid(dropped)) == uuid &&
                          obs_scene_find_source(scene, obs_source_get_name(dropped)),
                          "Closed window leaves the same source in the scene");
                    fixture.waitForFinished(3000);
                    fixture.start(qEnvironmentVariable("OBS_WEBVIEW2_CAPTURE_FIXTURE_EXE"));
                    check(fixture.waitForStarted(5000), "Fixture restarts");
                    poll([this] { return obs_source_get_width(dropped) == 640 && obs_source_get_height(dropped) == 360; },
                         "The saved source resumes when the program reopens", [this] { finish(); });
                });
            });
        });
    }

public:
    WindowDropChecks(OBSBasic *window, QWidget *surface, std::function<void(bool, const char *)> report,
                     std::function<void()> complete)
        : QObject(window), main(window), preview(surface), check(std::move(report)), done(std::move(complete)) {}

    void run()
    {
        check(main && preview && scene && lower && upper, "Window drop fixtures are available");
        if (!main || !preview || !scene || !lower || !upper) { finish(); return; }
        obs_scene_add(scene, lower);
        obs_scene_add(scene, upper);
        obs_frontend_set_current_scene(obs_scene_get_source(scene));
        const QString executable = qEnvironmentVariable("OBS_WEBVIEW2_CAPTURE_FIXTURE_EXE");
        check(!executable.isEmpty(), "External capture fixture executable is configured");
        if (executable.isEmpty()) { finish(); return; }
        fixture.start(executable);
        check(fixture.waitForStarted(5000), "External graphics fixture starts");
        if (fixture.state() != QProcess::Running) { finish(); return; }
        poll([this] { return findFixture(fixture.processId()) != nullptr && preview->isVisible(); },
             "Fixture and OBS preview become visible", [this] {
            check(!main->isMinimized() && QApplication::activeModalWidget() == nullptr,
                  "OBS can accept the external window move");
            const HWND target = findFixture(fixture.processId());
            RECT area{};
            GetWindowRect(reinterpret_cast<HWND>(preview->winId()), &area);
            SetWindowPos(target, nullptr, area.left - 200, area.top - 200, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetWindowRect(target, &beforeMove);
            PostMessageW(target, WM_APP + 10, WPARAM(area.left + 80), LPARAM(area.top + 80));
            QTimer::singleShot(700, this, [this] { afterDrop(); });
        });
    }
};
}

static void RunWindowDropChecks(OBSBasic *main, QWidget *preview,
                                const std::function<void(bool, const char *)> &check,
                                const std::function<void()> &done)
{
    (new WindowDropChecks(main, preview, check, done))->run();
}
