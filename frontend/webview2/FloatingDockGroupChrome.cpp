#include "FloatingDockGroupChrome.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTabBar>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

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

QDockWidget *FloatingDockGroupChrome::activeDockForTabs(QTabBar *tabs) const
{
	if (!main || !tabs || tabs->window() != main || tabs->count() < 2) return nullptr;
	const QRect tabRect(tabs->mapToGlobal(QPoint()), tabs->size());
	QDockWidget *nearest = nullptr;
	int bestDistance = 72;
	for (auto *dock : main->findChildren<QDockWidget *>()) {
		if (dock->parentWidget() != main || !dock->isVisible() ||
		    main->tabifiedDockWidgets(dock).isEmpty()) continue;
		const QRect dockRect(dock->mapToGlobal(QPoint()), dock->size());
		const int overlap = std::min(dockRect.right(), tabRect.right()) -
		                    std::max(dockRect.left(), tabRect.left()) + 1;
		if (overlap < 20) continue;
		const int distance = std::min(std::abs(dockRect.top() - tabRect.bottom()),
		                              std::abs(dockRect.bottom() - tabRect.top()));
		if (distance < bestDistance) {
			bestDistance = distance;
			nearest = dock;
		}
	}
	return nearest;
}

void FloatingDockGroupChrome::sendDockMouse(QDockWidget *dock, QEvent::Type type, const QPoint &global,
	                                         Qt::MouseButton button, Qt::MouseButtons buttons,
	                                         Qt::KeyboardModifiers modifiers)
{
	if (!dock) return;
	QMouseEvent forwarded(type, QPointF(dock->mapFromGlobal(global)), QPointF(global), button, buttons, modifiers);
	dispatchingForwardedEvent = true;
	QApplication::sendEvent(dock, &forwarded);
	dispatchingForwardedEvent = false;
}

void FloatingDockGroupChrome::finishForwardedDrag(bool release, const QPoint &actual)
{
	const QPointer<QDockWidget> dock = forwardedDock;
	const QPointer<QWidget> grip = forwardedGrip;
	forwardedDock = nullptr;
	forwardedGrip = nullptr;
	guardedDock = nullptr;
	++guardSequence;
	releaseWatchdog.stop();
	if (release && dock)
		sendDockMouse(dock, QEvent::MouseButtonRelease, virtualPress + actual - actualPress,
		              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	if (grip && QWidget::mouseGrabber() == grip) grip->releaseMouse();
}

void FloatingDockGroupChrome::checkMissedRelease()
{
	if (!forwardedDock && !guardedDock) {
		releaseWatchdog.stop();
		return;
	}
	if (leftButtonDown ? leftButtonDown() : bool(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
		releasedTicks = 0;
		return;
	}
	// Native WebView2 children or a reparented Qt floating group can consume the
	// release event. The OS button state is authoritative after two quiet ticks.
	if (++releasedTicks < 2) return;
	const QPoint cursor = QCursor::pos();
	if (forwardedDock) {
		finishForwardedDrag(true, cursor);
	} else {
		const QPointer<QDockWidget> dock = guardedDock;
		guardedDock = nullptr;
		releaseWatchdog.stop();
		if (dock) sendDockMouse(dock, QEvent::MouseButtonRelease, cursor, Qt::LeftButton,
		                        Qt::NoButton, Qt::NoModifier);
	}
}

FloatingDockGroupChrome::FloatingDockGroupChrome(QMainWindow *owner, QObject *parent,
                                               std::function<bool()> buttonDown)
	: QObject(parent), main(owner), leftButtonDown(std::move(buttonDown))
{
	releaseWatchdog.setInterval(50);
	connect(&releaseWatchdog, &QTimer::timeout, this, &FloatingDockGroupChrome::checkMissedRelease);
	qApp->installEventFilter(this);
	for (auto *widget : QApplication::allWidgets())
		if (auto *group = groupFor(widget); group == widget) styleGroup(group);
}

FloatingDockGroupChrome::~FloatingDockGroupChrome()
{
	if (forwardedDock) finishForwardedDrag(true, QCursor::pos());
	releaseWatchdog.stop();
	finishMove();
	qApp->removeEventFilter(this);
	for (auto it = originalStyles.cbegin(); it != originalStyles.cend(); ++it)
		it.key()->setStyleSheet(it.value());
}

bool FloatingDockGroupChrome::eventFilter(QObject *object, QEvent *event)
{
	if (dispatchingForwardedEvent) return false;
	if (forwardedDock) {
		if (!main || !main->property("webview2NativeDocking").toBool() ||
		    event->type() == QEvent::ApplicationDeactivate) {
			finishForwardedDrag(true, QCursor::pos());
			return false;
		}
		if (object == forwardedGrip || object == forwardedDock) {
			if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
				auto *mouse = static_cast<QMouseEvent *>(event);
				if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) {
					finishForwardedDrag(true, mouse->globalPosition().toPoint());
					mouse->accept();
					return true;
				}
				if (event->type() == QEvent::MouseMove) {
					if (!mouse->buttons().testFlag(Qt::LeftButton))
						finishForwardedDrag(true, mouse->globalPosition().toPoint());
					else
						sendDockMouse(forwardedDock, QEvent::MouseMove,
						              virtualPress + mouse->globalPosition().toPoint() - actualPress,
						              Qt::NoButton, mouse->buttons(), mouse->modifiers());
					mouse->accept();
					return true;
				}
			}
		}
	}
	if (event->type() == QEvent::MouseButtonPress) {
		auto *mouse = static_cast<QMouseEvent *>(event);
		auto *tabs = qobject_cast<QTabBar *>(object);
		if (tabs && mouse->button() == Qt::LeftButton && main &&
		    main->property("webview2NativeDocking").toBool() &&
		    tabs->tabAt(mouse->position().toPoint()) < 0) {
			auto *lock = main->findChild<QAction *>(QStringLiteral("lockDocks"));
			auto *dock = lock && lock->isChecked() ? nullptr : activeDockForTabs(tabs);
			if (dock && dock->features().testFlag(QDockWidget::DockWidgetMovable)) {
				const QRect title = dock->titleBarWidget() ? dock->titleBarWidget()->geometry() :
				                    QRect(0, 0, dock->width(), 24);
				const int left = title.left() + 2;
				const int right = std::max(left, title.right() - 2);
				const QPoint anchor(std::clamp(dock->mapFromGlobal(mouse->globalPosition().toPoint()).x(),
				                               left, right), title.center().y());
				forwardedDock = dock;
				forwardedGrip = tabs;
				guardedDock = dock;
				++guardSequence;
				actualPress = mouse->globalPosition().toPoint();
				virtualPress = dock->mapToGlobal(anchor);
				releasedTicks = 0;
				sendDockMouse(dock, QEvent::MouseButtonPress, virtualPress,
				              Qt::LeftButton, Qt::LeftButton, mouse->modifiers());
				tabs->grabMouse();
				if (QWidget::mouseGrabber() != tabs) {
					finishForwardedDrag(true, actualPress);
					return false;
				}
				releaseWatchdog.start();
				mouse->accept();
				return true;
			}
		}
		auto *widget = qobject_cast<QWidget *>(object);
		if (widget && widget->objectName() == QStringLiteral("obsWebView2DockTitleBar") &&
		    mouse->button() == Qt::LeftButton && main &&
		    main->property("webview2NativeDocking").toBool()) {
			auto *dock = qobject_cast<QDockWidget *>(widget->parentWidget());
			auto *parent = dock ? dock->parentWidget() : nullptr;
			if (parent != main && (!parent || !parent->inherits("QDockWidgetGroupWindow") ||
			                       parent->parentWidget() != main)) return false;
			guardedDock = dock;
			++guardSequence;
			releasedTicks = 0;
			if (guardedDock) releaseWatchdog.start();
		}
	}
	if (guardedDock && !forwardedDock && event->type() == QEvent::MouseButtonRelease) {
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::LeftButton &&
		    (object == guardedDock || object == guardedDock->titleBarWidget())) {
			const QPointer<QDockWidget> dock = guardedDock;
			const auto sequence = guardSequence;
			QTimer::singleShot(0, this, [this, dock, sequence] {
				if (guardSequence == sequence && guardedDock == dock) {
					guardedDock = nullptr;
					releaseWatchdog.stop();
				}
			});
		}
	}
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
