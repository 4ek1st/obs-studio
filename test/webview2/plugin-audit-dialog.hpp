#pragma once

#include <QApplication>
#include <QAbstractButton>
#include <QMessageBox>
#include <QPointer>

// Test-process cleanup for OBSApp::handlePluginFailure's exact message box.
// The caller must first verify the disposable portable audit command flags.
inline int ClosePluginAuditWarnings(const QString &title, const QString &text,
                                   const QString &continueText, const QString &openText)
{
    QList<QPointer<QMessageBox>> warnings;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *box = qobject_cast<QMessageBox *>(widget)) warnings.append(box);
    int closed = 0;
    for (const auto &box : warnings) {
        if (!box || !box->isVisible() || box->parentWidget() || box->icon() != QMessageBox::Warning ||
            box->windowTitle() != title || box->text() != text || box->buttons().size() != 2) continue;
        QAbstractButton *continueButton = nullptr;
        bool hasOpen = false;
        for (auto *button : box->buttons()) {
            if (box->buttonRole(button) == QMessageBox::RejectRole && button->text() == continueText)
                continueButton = button;
            if (box->buttonRole(button) == QMessageBox::AcceptRole && button->text() == openText)
                hasOpen = true;
        }
        if (continueButton && hasOpen) {
            continueButton->click();
            ++closed;
        }
    }
    return closed;
}
