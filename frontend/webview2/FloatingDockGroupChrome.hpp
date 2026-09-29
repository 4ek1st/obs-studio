#pragma once

#include <QHash>
#include <QEvent>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtGlobal>
#include <functional>

class QDockWidget;
class QMainWindow;
class QMouseEvent;
class QTabBar;
class QWidget;

// Styles floating Qt tab groups and makes the unused part of their tab row
// draggable, whether the group is docked or floating. Qt still owns docking,
// tab tear-out and drop targets.
class FloatingDockGroupChrome final : public QObject {
	QPointer<QMainWindow> main;
	QPointer<QWidget> movingGroup;
	QPointer<QWidget> movingGrip;
	QPoint pressGlobal;
	QPoint pressWindow;
	QHash<QWidget *, QString> originalStyles;
	QPointer<QDockWidget> forwardedDock;
	QPointer<QWidget> forwardedGrip;
	QPointer<QDockWidget> guardedDock;
	QPoint actualPress;
	QPoint virtualPress;
	QTimer releaseWatchdog;
	std::function<bool()> leftButtonDown;
	quint64 guardSequence = 0;
	int releasedTicks = 0;
	bool dispatchingForwardedEvent = false;

	QWidget *groupFor(QObject *object) const;
	void styleGroup(QWidget *group);
	QTabBar *tabsFor(QWidget *group) const;
	bool blankHeader(QWidget *group, QWidget *grip, const QMouseEvent *mouse) const;
	void finishMove();
	QDockWidget *activeDockForTabs(QTabBar *tabs) const;
	void sendDockMouse(QDockWidget *dock, QEvent::Type type, const QPoint &global,
	                   Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
	void finishForwardedDrag(bool release, const QPoint &actual);
	void checkMissedRelease();

public:
	FloatingDockGroupChrome(QMainWindow *owner, QObject *parent,
	                        std::function<bool()> buttonDown = {});
	~FloatingDockGroupChrome() override;

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
};
