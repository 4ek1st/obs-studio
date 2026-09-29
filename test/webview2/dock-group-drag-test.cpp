#include "../../frontend/webview2/FloatingDockGroupChrome.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTabBar>
#include <QWidget>
#include <iostream>

// Qt's internal floating tab group has this metaobject name. The production
// filter deliberately limits itself to these groups; this fixture supplies its
// actual parent, tabs and child docks without starting all of OBS.
class QDockWidgetGroupWindow final : public QWidget {
	Q_OBJECT
public:
	explicit QDockWidgetGroupWindow(QWidget *parent) : QWidget(parent, Qt::Window) {}
};

static void sendMouse(QWidget *target, QEvent::Type type, Qt::MouseButton button,
			      Qt::MouseButtons buttons, const QPoint &point)
{
	const QPoint global = target->mapToGlobal(point);
	QMouseEvent event(type, QPointF(point), QPointF(global), button, buttons, Qt::NoModifier);
	QApplication::sendEvent(target, &event);
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QMainWindow main;
	main.setProperty("webview2NativeDocking", true);
	main.resize(600, 400);
	main.show();

	QDockWidgetGroupWindow group(&main);
	group.resize(450, 300);
	QDockWidget sources(QStringLiteral("Sources"), &group);
	QDockWidget scenes(QStringLiteral("Scenes"), &group);
	sources.show();
	scenes.show();
	QTabBar tabs(&group);
	tabs.setExpanding(false);
	tabs.addTab(QStringLiteral("Sources"));
	tabs.addTab(QStringLiteral("Scenes"));
	tabs.setGeometry(0, 0, 420, 30);
	group.show();
	tabs.show();
	app.processEvents();

	const QPoint blank(400, 15);
	if (tabs.tabAt(blank) >= 0 || !group.isWindow() || group.parentWidget() != &main) {
		std::cerr << "FAIL: floating tab group fixture is invalid\n";
		return 1;
	}
	FloatingDockGroupChrome chrome(&main, &app);
	sendMouse(&tabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, blank);
	if (QWidget::mouseGrabber() != &tabs) {
		std::cerr << "FAIL: dragging the group did not capture the mouse\n";
		return 1;
	}
	sendMouse(&tabs, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, blank);
	if (QWidget::mouseGrabber()) {
		std::cerr << "FAIL: releasing the button retained the mouse grab\n";
		return 1;
	}
	sendMouse(&tabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, blank);
	QEvent deactivated(QEvent::WindowDeactivate);
	QApplication::sendEvent(&group, &deactivated);
	if (QWidget::mouseGrabber()) {
		std::cerr << "FAIL: deactivating the group retained the mouse grab\n";
		return 1;
	}
	sendMouse(&tabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, blank);
	sendMouse(&tabs, QEvent::MouseMove, Qt::NoButton, Qt::NoButton, blank);
	if (QWidget::mouseGrabber()) {
		std::cerr << "FAIL: movement without a pressed button retained the mouse grab\n";
		return 1;
	}
	std::cout << "PASS: floating group drag captures and releases the mouse on all exit paths\n";
	return 0;
}

#include "dock-group-drag-test.moc"
