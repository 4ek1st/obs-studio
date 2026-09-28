// Opt-in private fixtures for the two OBS 33 compatibility builds. The original
// OBS ScreenshotObj performs the actual GPU render/readback. No device, output,
// public source or personal scene is created or changed.
#include <utility/ScreenshotObj.hpp>
#include <QImage>
#include <QSaveFile>

namespace {
class PluginRenderChecks final : public QObject {
    QString directory;
    bool web;
    QJsonArray checks;
    OBSSourceAutoRelease source;
    OBSSourceAutoRelease filter;
    QImage baseline, blurred;
    int stepIndex = 0;
    std::function<void()> done;
    void check(bool passed, const QString &name) {
        checks.append(QJsonObject{{"name", name}, {"passed", passed}});
        blog(passed ? LOG_INFO : LOG_ERROR, "[Plugin render] %s: %s", passed ? "PASS" : "FAIL", name.toUtf8().constData());
    }
    QString path(const char *name) const { return QDir(directory).filePath(QString::fromUtf8(name)); }
    void setPath(const char *key, const char *file) {
        OBSDataAutoRelease settings = obs_source_get_settings(filter);
        obs_data_set_string(settings, key, path(file).toUtf8().constData());
        obs_source_update(filter, settings);
    }
    void removeFilter() {
        if (filter) { obs_source_filter_remove(source, filter); filter = nullptr; }
    }
    bool createFilter(bool shader) {
        OBSDataAutoRelease settings = obs_data_create();
        if (shader) {
            obs_data_set_string(settings, "shader_text",
                "uniform texture2d qa_texture;\nfloat4 mainImage(VertData v_in) : TARGET { return qa_texture.Sample(textureSampler, v_in.uv); }\n");
            obs_data_set_string(settings, "qa_texture", path("green.png").toUtf8().constData());
        } else {
            obs_data_set_int(settings, "blur_algorithm", 1);
            obs_data_set_int(settings, "blur_type", 1);
            obs_data_set_double(settings, "radius", 8.0);
            obs_data_set_int(settings, "effect_mask", 5);
            obs_data_set_string(settings, "effect_mask_source_file", path("opaque-mask.png").toUtf8().constData());
        }
        const char *id = shader ? "shader_filter" : "obs_composite_blur";
        filter = obs_source_create_private(id, "Compatibility fixture filter", settings);
        const bool valid = filter && obs_source_get_display_name(id);
        check(valid, QStringLiteral("Create original filter: ") + id);
        if (valid) obs_source_filter_add(source, filter);
        return valid;
    }
    bool reloadFilter() {
        OBSDataAutoRelease saved = obs_save_source(filter);
        OBSDataAutoRelease oldSettings = obs_source_get_settings(filter);
        const auto expected = QByteArray(obs_data_get_json(oldSettings));
        const auto id = QByteArray(obs_source_get_id(filter));
        removeFilter();
        filter = obs_load_private_source(saved);
        OBSDataAutoRelease restored = filter ? obs_source_get_settings(filter) : nullptr;
        const bool same = filter && id == obs_source_get_id(filter) &&
            QJsonDocument::fromJson(expected) == QJsonDocument::fromJson(obs_data_get_json(restored));
        check(same, "Save/reload preserves filter ID and complete settings");
        if (filter) obs_source_filter_add(source, filter);
        return !!filter;
    }
    static double difference(const QImage &a, const QImage &b) {
        if (a.isNull() || a.size() != b.size()) return 1000;
        double sum = 0;
        for (int y = 8; y < a.height() - 8; ++y)
            for (int x = 8; x < a.width() - 8; ++x) {
                const auto ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
                sum += std::abs(ca.red() - cb.red()) + std::abs(ca.green() - cb.green()) + std::abs(ca.blue() - cb.blue());
            }
        return sum / (3.0 * (a.width() - 16) * (a.height() - 16));
    }
    void render() {
        QTimer::singleShot(250, this, [this] {
            if (!source || obs_source_get_width(source) != 64 || obs_source_get_height(source) != 64) {
                check(false, "Fixture image source has its original 64x64 size"); finish(); return;
            }
            ScreenshotObj::Options options; options.outputToFile = false;
            auto *capture = new ScreenshotObj(source, options);
            auto *deadline = new QTimer(this); deadline->setSingleShot(true);
            connect(deadline, &QTimer::timeout, this, [this, capture, deadline] {
                delete capture; deadline->deleteLater(); check(false, "GPU readback completes within 5 seconds"); finish();
            });
            connect(capture, &ScreenshotObj::imageReady, this, [this, deadline](QImage image) {
                deadline->stop(); deadline->deleteLater();
                check(!image.isNull() && image.size() == QSize(64, 64), QString("GPU frame %1 has valid dimensions").arg(stepIndex));
                image.save(path(QString("frame-%1.png").arg(stepIndex).toUtf8().constData()));
                const auto center = image.isNull() ? QColor() : image.pixelColor(32, 32);
                switch (stepIndex) {
                case 0:
                    baseline = image;
                    check(!image.isNull() && image.pixelColor(12, 12).red() < 5 && image.pixelColor(20, 12).red() > 250,
                        "Unfiltered checkerboard reaches GPU readback with distinct black and white pixels"); break;
                case 1:
                    blurred = image;
                    check(difference(image, baseline) > 20 && center.red() > 20 && center.red() < 240,
                        "Composite Blur applies the opaque image mask and changes checkerboard pixels"); break;
                case 2: case 6: case 12:
                    check(difference(image, baseline) < 2, "Transparent mask or disabled filter restores the original pixels"); break;
                case 4: case 5:
                    check(difference(image, blurred) < 2, "Composite Blur recovers identical output after replacing/reloading its image mask"); break;
                case 7: case 10: case 11:
                    check(center.green() > 245 && center.red() < 10 && center.blue() < 10,
                        "Shaderfilter loads/reloads the green file texture and renders it on the GPU"); break;
                case 8:
                    check(center.red() > 245 && center.blue() > 245 && center.green() < 10,
                        "Shaderfilter replaces its file texture with magenta"); break;
                default: break; // Missing images must stay alive and recover in the next step.
                }
                ++stepIndex;
                QTimer::singleShot(0, this, [this] { advance(); });
            });
            deadline->start(5000);
        });
    }
    void advance() {
        switch (stepIndex) {
        case 0: break;
        case 1: if (!createFilter(false)) { finish(); return; } break;
        case 2: setPath("effect_mask_source_file", "transparent-mask.png"); break;
        case 3: setPath("effect_mask_source_file", "missing.png"); break;
        case 4: setPath("effect_mask_source_file", "opaque-mask.png"); break;
        case 5: if (!reloadFilter()) { finish(); return; } break;
        case 6: obs_source_set_enabled(filter, false); break;
        case 7: removeFilter(); if (!createFilter(true)) { finish(); return; } break;
        case 8: setPath("qa_texture", "magenta.png"); break;
        case 9: setPath("qa_texture", "missing.png"); break;
        case 10: setPath("qa_texture", "green.png"); break;
        case 11: if (!reloadFilter()) { finish(); return; } break;
        case 12: obs_source_set_enabled(filter, false); break;
        default: finish(); return;
        }
        render();
    }
    void finish() {
        removeFilter();
        if (source) { obs_source_dec_showing(source); source = nullptr; }
        bool passed = stepIndex == 13;
        for (const auto &entry : checks) passed &= entry.toObject().value("passed").toBool();
        QSaveFile file(path("plugin-render.json"));
        const auto bytes = QJsonDocument(QJsonObject{{"frontend", web ? "webview2" : "native-qt"},
            {"passed", passed}, {"checks", checks}, {"scope", "Private PNG source; real GPU filter output, image replacement, missing-file recovery, save/reload, disable and release; no devices or outputs"}}).toJson();
        const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        if (!written) blog(LOG_ERROR, "[Plugin render] Unable to write report");
        const auto completed = done; deleteLater(); completed();
    }
public:
    PluginRenderChecks(OBSBasic *parent, bool useWeb, const QString &artifacts, std::function<void()> completed)
        : QObject(parent), directory(QDir(artifacts).filePath("render")), web(useWeb), done(std::move(completed)) {}
    void run() {
        if (!QFileInfo(directory).isAbsolute() || !QDir().mkpath(directory)) { check(false, "Explicit QA artifact path is writable"); finish(); return; }
        QImage image(64, 64, QImage::Format_RGBA8888);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            image.setPixelColor(x, y, ((x / 8 + y / 8) % 2) ? Qt::white : Qt::black);
        bool saved = image.save(path("checker.png"));
        image.fill(Qt::white); saved &= image.save(path("opaque-mask.png"));
        image.fill(Qt::transparent); saved &= image.save(path("transparent-mask.png"));
        image.fill(Qt::green); saved &= image.save(path("green.png"));
        image.fill(Qt::magenta); saved &= image.save(path("magenta.png"));
        check(saved, "Disposable checker, mask and texture fixtures are written");
        if (!saved) { finish(); return; }
        OBSDataAutoRelease settings = obs_data_create();
        obs_data_set_string(settings, "file", path("checker.png").toUtf8().constData());
        source = obs_source_create_private("image_source", "Plugin compatibility checker", settings);
        if (!source) { check(false, "Private image source is created"); finish(); return; }
        obs_source_inc_showing(source);
        advance();
    }
};
} // namespace
