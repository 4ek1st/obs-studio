#pragma once

#include "UiGeometry.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QHash>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>

namespace OBSWeb {
inline QJsonObject ControlPresentation(QAbstractButton *button, const QString &id, const QString &group,
				      QHash<qint64, QString> &icons)
{
	const auto classes = button->property("class").toStringList().join(' ').split(' ', Qt::SkipEmptyParts);
	const auto *push = qobject_cast<QPushButton *>(button);
	const auto icon = button->icon();
	QString image;
	if (!icon.isNull()) {
		if (!icons.contains(icon.cacheKey())) {
			if (icons.size() > 256) icons.clear();
			QByteArray bytes;
			QBuffer buffer(&bytes);
			if (buffer.open(QIODevice::WriteOnly) && icon.pixmap(32, 32).save(&buffer, "PNG"))
				icons.insert(icon.cacheKey(), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
		}
		image = icons.value(icon.cacheKey());
	}
	return {{"id", id}, {"text", ControlLabel(button->text(), button->accessibleName(), button->toolTip())},
		{"tooltip", button->toolTip()}, {"enabled", button->isEnabled()}, {"checked", button->isChecked()},
		{"checkable", button->isCheckable()}, {"active", classes.contains("state-active")},
		{"icon", image}, {"iconOnly", button->text().isEmpty() && !icon.isNull()}, {"group", group},
		{"menu", push && push->menu()}, {"background", button->palette().color(QPalette::Button).name()},
		{"foreground", button->palette().color(QPalette::ButtonText).name()}};
}

inline void ActivateControlButton(QAbstractButton *button)
{
	if (button->inherits("MenuButton")) {
		QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(button, &enter);
	} else if (auto *push = qobject_cast<QPushButton *>(button); push && push->menu()) {
		// QAbstractButton::click emits clicked even when the menu is cancelled.
		// Qt's mouse path only invokes the selected QAction for a menu button.
		push->menu()->popup(QCursor::pos());
	} else {
		button->click();
	}
}
} // namespace OBSWeb
