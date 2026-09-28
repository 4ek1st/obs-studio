#include "plugin-audit-dialog.hpp"

#include <QPushButton>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    int failed = 0;
    auto check = [&](bool pass, const char *label) {
        std::printf("%s %s\n", pass ? "PASS" : "FAIL", label);
        failed += !pass;
    };
    auto setup = [](QMessageBox &box) {
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("Plugin Errors");
        box.setText("One or more plugins failed to load.");
        box.addButton("Continue", QMessageBox::RejectRole);
        box.addButton("Open Plugin Manager", QMessageBox::AcceptRole);
        box.show();
    };
    QMessageBox plugin, unrelated, changed, extra;
    setup(plugin); setup(unrelated); setup(changed); setup(extra);
    unrelated.setWindowTitle("Output Error");
    changed.setText("Another warning");
    extra.addButton("Retry", QMessageBox::ActionRole);
    app.processEvents();
    const int closed = ClosePluginAuditWarnings("Plugin Errors", "One or more plugins failed to load.",
                                                "Continue", "Open Plugin Manager");
    check(closed == 1 && !plugin.isVisible(), "only the exact parentless native plugin warning closes");
    check(plugin.clickedButton() && plugin.buttonRole(plugin.clickedButton()) == QMessageBox::RejectRole,
          "cleanup chooses Continue instead of opening Plugin Manager");
    check(unrelated.isVisible() && changed.isVisible() && extra.isVisible(),
          "different titles, text, and button sets are preserved");
    check(ClosePluginAuditWarnings("Plugin Errors", "One or more plugins failed to load.",
                                  "Continue", "Open Plugin Manager") == 0,
          "repeated cleanup is harmless");
    return failed ? 1 : 0;
}
