// Dedicated disposable portable probe. Enumeration does not instantiate sources,
// devices or outputs. Plugin initialization itself has already happened in OBS.
#include <QSaveFile>
#include "plugin-audit-dialog.hpp"

namespace {
bool IsPluginAuditRequested()
{
    const auto args = QCoreApplication::arguments();
    return args.contains(QStringLiteral("--webview2-plugin-audit")) &&
           args.contains(QStringLiteral("--webview2-self-test")) &&
           args.contains(QStringLiteral("--portable"));
}

void RunPluginAudit(OBSBasic *main, bool web)
{
    if (!main || !IsPluginAuditRequested() || main->property("webview2PluginAuditStarted").toBool()) return;
    main->setProperty("webview2PluginAuditStarted", true);
    QJsonArray modules;
    obs_enum_modules([](void *data, obs_module_t *module) {
        auto text = [](const char *value) { return QString::fromUtf8(value ? value : ""); };
        static_cast<QJsonArray *>(data)->append(QJsonObject{
            {"file", text(obs_get_module_file_name(module))},
            {"name", text(obs_get_module_name(module))},
            {"path", text(obs_get_module_binary_path(module))}});
    }, &modules);
    auto enumerate = [](bool (*next)(size_t, const char **)) {
        QJsonArray values;
        const char *id = nullptr;
        for (size_t i = 0; next(i, &id); i++) values.append(QString::fromUtf8(id ? id : ""));
        return values;
    };
    const auto args = QCoreApplication::arguments();
    const QJsonObject report{
        {"frontend", web ? "webview2" : "native-qt"},
        {"obsVersion", QString::fromUtf8(obs_get_version_string())},
        {"onlyBundledPlugins", args.contains(QStringLiteral("--only-bundled-plugins"))},
        {"modules", modules}, {"sourceTypes", enumerate(obs_enum_source_types)},
        {"inputTypes", enumerate(obs_enum_input_types)}, {"filterTypes", enumerate(obs_enum_filter_types)},
        {"transitionTypes", enumerate(obs_enum_transition_types)},
        {"scope", "Successful initialization and registered type IDs; no source, output, device or plugin workflow was created by this helper"}};
    BPtr<char> defaultPath = GetAppConfigPathPtr("obs-studio/webview2-plugin-audit.json");
    const QString artifactDirectory = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
    const QString path = artifactDirectory.isEmpty() ? QString::fromUtf8(defaultPath.Get()) :
        QDir(artifactDirectory).filePath(QStringLiteral("plugin-audit.json"));
    QSaveFile output(path);
    const auto bytes = QJsonDocument(report).toJson();
    const bool written = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    blog(written ? LOG_INFO : LOG_ERROR, "[Plugin audit] %s: %s", written ? "Saved inventory" : "Unable to save inventory", path.toUtf8().constData());
    config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
    // OBSApp's startup warning is parentless and can keep its nested event loop
    // alive after the main window closes. Dismiss only that exact warning in
    // this already guarded disposable audit process, preserving other dialogs.
    const auto dismissWarning = [title = QTStr("PluginFailure.Dialog.Title"),
                                 text = QTStr("PluginFailure.Labels.Text"),
                                 continueText = QTStr("PluginFailure.Dialog.Continue"),
                                 openText = QTStr("PluginFailure.Dialog.Open")] {
        const int closed = ClosePluginAuditWarnings(title, text, continueText, openText);
        if (closed) blog(LOG_INFO, "[Plugin audit] Continued past %d native plugin warning(s)", closed);
    };
    const QPointer<OBSBasic> owner = main;
    QTimer::singleShot(100, qApp, [owner, dismissWarning] {
        dismissWarning();
        if (owner) owner->close();
        QTimer::singleShot(100, qApp, dismissWarning);
        QTimer::singleShot(500, qApp, dismissWarning);
    });
}
} // namespace
