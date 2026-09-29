#include "FloatingDockGroupChrome.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTabBar>
#include <QWidget>

QWidget *FloatingDockGroupChrome::groupFor(QObject *object) const
{
	auto *widget = qobject_cast<QWidget *>(object);
	auto *group = widget ? widget->window() : nullptr;
	return group && group->inherits("QDockWidgetGroupWindow") && group->parentWidget() == main ? group : nullptr;
}

void FloatingDockGroupChrome::styleGroup(QWidget *group)
{
	if (!group || originalStyles.contains(group)) return;
	originalStyles.insert(group, group->styleSheet());
	connect(group, &QObject::destroyed, this, [this, group] { originalStyles.remove(group); });
	// Keep Qt's edge resize area while replacing its bright beveled group frame.
	group->setStyleSheet(group->styleSheet() + QStringLiteral(
		"\nQDockWidgetGroupWindow { background-color: palette(window); border: 1px solid palette(window); }"));
}

QTabBar *FloatingDockGroupChrome::tabsFor(QWidget *group) const
{
	for (auto *tabs : group->findChildren<QTabBar *>())
		if (tabs->isVisible() && tabs->window() == group) return tabs;
	return nullptr;
}

bool FloatingDockGroupChrome::blankHeader(QWidget *group, QWidget *grip, const QMouseEvent *mouse) const
{
	if (!group || group->findChildren<QDockWidget *>().size() < 2) return false;
	auto *tabs = tabsFor(group);
	if (!tabs) return false;
	if (grip == tabs) return tabs->tabAt(mouse->position().toPoint()) < 0;
	if (grip != group) return false;
	const QPoint local = group->mapFromGlobal(mouse->globalPosition().toPoint());
	const QPoint tabTop = tabs->mapTo(group, QPoint());
	return local.y() >= tabTop.y() - 4 && local.y() <= tabTop.y() + tabs->height() + 4;
}

void FloatingDockGroupChrome::finishMove()
{
	const QPointer<QWidget> grip = movingGrip;
	movingGroup = nullptr;
	movingGrip = nullptr;
	if (!grip) return;
	grip->unsetCursor();
	if (QWidget::mouseGrabber() == grip) grip->releaseMouse();
}

FloatingDockGroupChrome::FloatingDockGroupChrome(QMainWindow *owner, QObject *parent) : QObject(parent), main(owner)
{
	qApp->installEventFilter(this);
	for (auto *widget : QApplication::allWidgets())
		if (auto *group = groupFor(widget); group == widget) styleGroup(group);
}

FloatingDockGroupChrome::~FloatingDockGroupChrome()
{
	finishMove();
	qApp->removeEventFilter(this);
	for (auto it = originalStyles.cbegin(); it != originalStyles.cend(); ++it)
		it.key()->setStyleSheet(it.value());
}

bool FloatingDockGroupChrome::eventFilter(QObject *object, QEvent *event)
{
	if (movingGroup) {
		if (!main || !main->property("webview2NativeDocking").toBool() ||
			event->type() == QEvent::ApplicationDeactivate ||
			(object == movingGrip && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)) ||
			(object == movingGroup && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))) {
			finishMove();
			return false;
		}
		if (object == movingGrip && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
			auto *mouse = static_cast<QMouseEvent *>(event);
			if (event->type() == QEvent::MouseMove) {
				if (!mouse->buttons().testFlag(Qt::LeftButton)) {
					finishMove();
					return false;
				}
				movingGroup->move(pressWindow + mouse->globalPosition().toPoint() - pressGlobal);
				mouse->accept();
				return true;
			}
			if (mouse->button() == Qt::LeftButton) {
				finishMove();
				mouse->accept();
				return true;
			}
		}
	}
	auto *group = groupFor(object);
	if (!group || !main || !main->property("webview2NativeDocking").toBool())
		return false;
	if (object == group && event->type() == QEvent::Show) styleGroup(group);
	if (event->type() != QEvent::MouseButtonPress) return false;
	auto *grip = qobject_cast<QWidget *>(object);
	auto *mouse = static_cast<QMouseEvent *>(event);
	if (movingGroup || mouse->button() != Qt::LeftButton || !blankHeader(group, grip, mouse)) return false;
	auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
	if (lock && lock->isChecked()) return false;
	movingGroup = group;
	movingGrip = grip;
	pressGlobal = mouse->globalPosition().toPoint();
	pressWindow = group->pos();
	grip->setCursor(Qt::ClosedHandCursor);
	grip->grabMouse();
	if (QWidget::mouseGrabber() != grip) {
		finishMove();
		return false;
	}
	mouse->accept();
	return true;
}
