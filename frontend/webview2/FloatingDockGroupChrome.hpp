#pragma once

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>

class QMainWindow;
class QMouseEvent;
class QTabBar;
class QWidget;

// Applies the OBS theme to Qt's floating tab group and lets its blank tab
// header move the group. Qt owns docking, tab tear-out and drop targets.
class FloatingDockGroupChrome final : public QObject {
	QPointer<QMainWindow> main;
	QPointer<QWidget> movingGroup;
	QPointer<QWidget> movingGrip;
	QPoint pressGlobal;
	QPoint pressWindow;
	QHash<QWidget *, QString> originalStyles;

	QWidget *groupFor(QObject *object) const;
	void styleGroup(QWidget *group);
	QTabBar *tabsFor(QWidget *group) const;
	bool blankHeader(QWidget *group, QWidget *grip, const QMouseEvent *mouse) const;
	void finishMove();

public:
	FloatingDockGroupChrome(QMainWindow *owner, QObject *parent);
	~FloatingDockGroupChrome() override;

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
};
