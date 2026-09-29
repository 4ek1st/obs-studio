#include "../../frontend/webview2/FloatingDockGroupChrome.hpp"

#include <QApplication>
#include <QAction>
#include <QCursor>
#include <QDockWidget>
#include <QEventLoop>
#include <QMainWindow>
#include <QMouseEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>
#include <iostream>
#include <Windows.h>

class IgnoringTitleBar final : public QWidget {
public:
	explicit IgnoringTitleBar(QWidget *parent) : QWidget(parent)
	{
		setObjectName(QStringLiteral("obsWebView2DockTitleBar"));
		setFixedHeight(28);
	}
	QSize sizeHint() const override { return {150, 28}; }
	QSize minimumSizeHint() const override { return sizeHint(); }
protected:
	void mousePressEvent(QMouseEvent *event) override { event->ignore(); }
	void mouseMoveEvent(QMouseEvent *event) override { event->ignore(); }
	void mouseReleaseEvent(QMouseEvent *event) override { event->ignore(); }
};

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
	QMainWindow real;
	real.resize(800, 500);
	real.setDockOptions(real.dockOptions() | QMainWindow::GroupedDragging);
	real.setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
	real.setProperty("webview2NativeDocking", true);
	QDockWidget first(QStringLiteral("First"), &real);
	QDockWidget second(QStringLiteral("Second"), &real);
	QDockWidget target(QStringLiteral("Target"), &real);
	first.setObjectName(QStringLiteral("first"));
	second.setObjectName(QStringLiteral("second"));
	target.setObjectName(QStringLiteral("target"));
	real.addDockWidget(Qt::LeftDockWidgetArea, &first);
	real.addDockWidget(Qt::LeftDockWidgetArea, &second);
	real.addDockWidget(Qt::RightDockWidgetArea, &target);
	real.tabifyDockWidget(&first, &second);
	real.show();
	app.processEvents();
	FloatingDockGroupChrome realChrome(&real, &app);
	QTabBar *realTabs = nullptr;
	for (auto *bar : real.findChildren<QTabBar *>()) {
		if (bar->isVisible() && bar->count() == 2) { realTabs = bar; break; }
	}
	if (!realTabs || realTabs->tabAt(QPoint(realTabs->width() - 10, realTabs->height() / 2)) >= 0) {
		std::cerr << "FAIL: docked tab group has no blank tab strip\n";
		return 1;
	}
	const QPoint blankTab(realTabs->width() - 10, realTabs->height() / 2);
	QAction lock(&real);
	lock.setObjectName(QStringLiteral("lockDocks"));
	lock.setCheckable(true);
	lock.setChecked(true);
	sendMouse(realTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, blankTab);
	if (QWidget::mouseGrabber()) {
		std::cerr << "FAIL: locked docks can still be dragged by their blank tab strip\n";
		return 1;
	}
	sendMouse(realTabs, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, blankTab);
	lock.setChecked(false);
	const QPoint firstTab = realTabs->tabRect(0).center();
	sendMouse(realTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, firstTab);
	sendMouse(realTabs, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, firstTab);
	if (realTabs->currentIndex() != 0 || first.parentWidget() != &real) {
		std::cerr << "FAIL: ordinary tab selection was intercepted\n";
		return 1;
	}
	const QPoint secondTab = realTabs->tabRect(1).center();
	sendMouse(realTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, secondTab);
	sendMouse(realTabs, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, secondTab);
	app.processEvents();
	sendMouse(realTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, blankTab);
	if (QWidget::mouseGrabber() != realTabs) {
		std::cerr << "FAIL: blank docked tab strip did not capture the mouse\n";
		return 1;
	}
	const QPoint dropGlobal = realTabs->mapToGlobal(blankTab) + QPoint(200, 100);
	sendMouse(realTabs, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, realTabs->mapFromGlobal(dropGlobal));
	if (QWidget::mouseGrabber() != &second) {
		std::cerr << "FAIL: blank strip did not start native grouped dragging\n";
		return 1;
	}
	sendMouse(&second, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
	          second.mapFromGlobal(dropGlobal));
	app.processEvents();
	if (QWidget::mouseGrabber() || !second.parentWidget() ||
	    !second.parentWidget()->inherits("QDockWidgetGroupWindow") ||
	    first.parentWidget() != second.parentWidget()) {
		std::cerr << "FAIL: blank tab drag did not release a floating group containing both panels\n";
		return 1;
	}
	QTabBar *floatingTabs = nullptr;
	for (auto *bar : second.parentWidget()->findChildren<QTabBar *>()) {
		if (bar->isVisible() && bar->count() == 2) { floatingTabs = bar; break; }
	}
	if (!floatingTabs) {
		std::cerr << "FAIL: floating group has no tab strip\n";
		return 1;
	}
	const QPoint floatingBlank(floatingTabs->width() - 10, floatingTabs->height() / 2);
	if (floatingTabs->tabAt(floatingBlank) >= 0) {
		std::cerr << "FAIL: floating group has no blank tab strip\n";
		return 1;
	}
	const QPoint dockTarget = target.mapToGlobal(target.rect().center());
	sendMouse(floatingTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, floatingBlank);
	sendMouse(floatingTabs, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton,
	          floatingTabs->mapFromGlobal(dockTarget));
	QWidget *releaseReceiver = QWidget::mouseGrabber();
	if (!releaseReceiver) releaseReceiver = floatingTabs;
	sendMouse(releaseReceiver, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
	          releaseReceiver->mapFromGlobal(dockTarget));
	app.processEvents();
	QEventLoop redockSettle;
	QTimer::singleShot(500, &redockSettle, &QEventLoop::quit);
	redockSettle.exec();
	if (first.parentWidget() != &real || second.parentWidget() != &real ||
	    !real.tabifiedDockWidgets(&first).contains(&second) ||
	    real.dockWidgetArea(&first) != Qt::RightDockWidgetArea) {
		std::cerr << "FAIL: floating group dropped on a dock did not insert both tabs into that area\n";
		return 1;
	}

	QMainWindow stuck;
	stuck.resize(800, 500);
	stuck.setDockOptions(stuck.dockOptions() | QMainWindow::GroupedDragging);
	stuck.setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
	stuck.setProperty("webview2NativeDocking", true);
	QDockWidget stuckFirst(QStringLiteral("First"), &stuck);
	QDockWidget stuckSecond(QStringLiteral("Second"), &stuck);
	stuckFirst.setObjectName(QStringLiteral("stuckFirst"));
	stuckSecond.setObjectName(QStringLiteral("stuckSecond"));
	auto *title = new IgnoringTitleBar(&stuckSecond);
	stuckSecond.setTitleBarWidget(title);
	stuck.addDockWidget(Qt::LeftDockWidgetArea, &stuckFirst);
	stuck.addDockWidget(Qt::LeftDockWidgetArea, &stuckSecond);
	stuck.tabifyDockWidget(&stuckFirst, &stuckSecond);
	stuck.show();
	stuckSecond.raise();
	app.processEvents();
	QEventLoop settle;
	QTimer::singleShot(250, &settle, &QEventLoop::quit);
	settle.exec();
	bool buttonDown = true;
	FloatingDockGroupChrome stuckChrome(&stuck, &app, [&] { return buttonDown; });
	sendMouse(title, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
	          QPoint(30, title->height() / 2));
	const QPoint dockTitle = stuckSecond.mapFromGlobal(title->mapToGlobal(QPoint(30, title->height() / 2)));
	sendMouse(&stuckSecond, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, dockTitle + QPoint(180, 100));
	if (QWidget::mouseGrabber() != &stuckSecond) {
		std::cerr << "FAIL: title drag fixture did not grab the dock\n";
		return 1;
	}
	buttonDown = false;
	QEventLoop loop;
	QTimer::singleShot(220, &loop, &QEventLoop::quit);
	loop.exec();
	if (QWidget::mouseGrabber()) {
		std::cerr << "FAIL: missed title release left a dock attached to the cursor\n";
		return 1;
	}
	QMainWindow lost;
	lost.resize(800, 500);
	lost.setDockOptions(lost.dockOptions() | QMainWindow::GroupedDragging);
	lost.setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
	lost.setProperty("webview2NativeDocking", true);
	QDockWidget lostFirst(QStringLiteral("First"), &lost);
	QDockWidget lostSecond(QStringLiteral("Second"), &lost);
	lostFirst.setObjectName(QStringLiteral("lostFirst"));
	lostSecond.setObjectName(QStringLiteral("lostSecond"));
	lost.addDockWidget(Qt::LeftDockWidgetArea, &lostFirst);
	lost.addDockWidget(Qt::LeftDockWidgetArea, &lostSecond);
	lost.tabifyDockWidget(&lostFirst, &lostSecond);
	lost.show();
	app.processEvents();
	bool lostButtonDown = true;
	FloatingDockGroupChrome lostChrome(&lost, &app, [&] { return lostButtonDown; });
	QTabBar *lostTabs = nullptr;
	for (auto *bar : lost.findChildren<QTabBar *>())
		if (bar->isVisible() && bar->count() == 2) { lostTabs = bar; break; }
	if (!lostTabs) {
		std::cerr << "FAIL: missed-release tab bar fixture missing\n";
		return 1;
	}
	const QPoint lostBlank(lostTabs->width() - 10, lostTabs->height() / 2);
	sendMouse(lostTabs, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, lostBlank);
	sendMouse(lostTabs, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, lostBlank + QPoint(200, 100));
	if (QWidget::mouseGrabber() != &lostSecond) {
		std::cerr << "FAIL: missed-release blank tab drag did not start\n";
		return 1;
	}
	lostButtonDown = false;
	QEventLoop lostLoop;
	QTimer::singleShot(220, &lostLoop, &QEventLoop::quit);
	lostLoop.exec();
	if (QWidget::mouseGrabber() || lostSecond.parentWidget() != lostFirst.parentWidget()) {
		std::cerr << "FAIL: missed blank tab release left the grouped panels dragging\n";
		return 1;
	}
	if (argc > 1 && QByteArray(argv[1]) == "--physical") {
		QMainWindow physical;
		physical.setProperty("webview2NativeDocking", true);
		physical.setDockOptions(physical.dockOptions() | QMainWindow::GroupedDragging);
		physical.setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
		physical.setWindowFlag(Qt::WindowStaysOnTopHint);
		physical.setGeometry(120, 120, 800, 500);
		QDockWidget a(QStringLiteral("Physical A"), &physical);
		QDockWidget b(QStringLiteral("Physical B"), &physical);
		QDockWidget target(QStringLiteral("Physical target"), &physical);
		a.setObjectName(QStringLiteral("physicalA"));
		b.setObjectName(QStringLiteral("physicalB"));
		target.setObjectName(QStringLiteral("physicalTarget"));
		auto *physicalTitle = new IgnoringTitleBar(&b);
		b.setTitleBarWidget(physicalTitle);
		physical.addDockWidget(Qt::LeftDockWidgetArea, &a);
		physical.addDockWidget(Qt::LeftDockWidgetArea, &b);
		physical.addDockWidget(Qt::RightDockWidgetArea, &target);
		physical.tabifyDockWidget(&a, &b);
		physical.show();
		physical.raise();
		physical.activateWindow();
		app.processEvents();
		FloatingDockGroupChrome physicalChrome(&physical, &app);
		QTabBar *physicalTabs = nullptr;
		for (auto *bar : physical.findChildren<QTabBar *>())
			if (bar->isVisible() && bar->count() == 2) { physicalTabs = bar; break; }
		if (!physicalTabs) {
			std::cerr << "FAIL: physical tab bar missing\n";
			return 1;
		}
		const QPoint start = physicalTabs->mapToGlobal(QPoint(physicalTabs->width() - 10,
		                                                     physicalTabs->height() / 2));
		if (QApplication::widgetAt(start) != physicalTabs) {
			std::cerr << "FAIL: physical tab strip is covered by another window\n";
			return 1;
		}
		const QPoint oldCursor = QCursor::pos();
		QEventLoop physicalLoop;
		auto sendButton = [](DWORD flags) {
			INPUT input{};
			input.type = INPUT_MOUSE;
			input.mi.dwFlags = flags;
			SendInput(1, &input, sizeof(input));
		};
		QTimer::singleShot(60, &physicalLoop, [&] { QCursor::setPos(start); sendButton(MOUSEEVENTF_LEFTDOWN); });
		QTimer::singleShot(160, &physicalLoop, [&] { QCursor::setPos(start + QPoint(100, 600)); });
		QTimer::singleShot(260, &physicalLoop, [&] { sendButton(MOUSEEVENTF_LEFTUP); });
		QTimer::singleShot(500, &physicalLoop, &QEventLoop::quit);
		physicalLoop.exec();
		QCursor::setPos(oldCursor);
		if (QWidget::mouseGrabber() || !b.parentWidget() ||
		    !b.parentWidget()->inherits("QDockWidgetGroupWindow") ||
		    a.parentWidget() != b.parentWidget()) {
			std::cerr << "FAIL: physical blank-tab drag did not move and release the group\n";
			return 1;
		}
		b.parentWidget()->setWindowFlag(Qt::WindowStaysOnTopHint);
		b.parentWidget()->show();
		b.parentWidget()->raise();
		b.parentWidget()->activateWindow();
		app.processEvents();
		const QPoint titleStart = physicalTitle->mapToGlobal(QPoint(30, physicalTitle->height() / 2));
		if (QApplication::widgetAt(titleStart) != physicalTitle) {
			std::cerr << "FAIL: physical title strip is covered by another window\n";
			return 1;
		}
		const QPoint outside = physical.mapToGlobal(QPoint(physical.width() + 180,
		                                                  physical.height() / 2));
		const QPoint groupBefore = b.parentWidget()->pos();
		QEventLoop titleLoop;
		QTimer::singleShot(60, &titleLoop, [&] { QCursor::setPos(titleStart); sendButton(MOUSEEVENTF_LEFTDOWN); });
		QTimer::singleShot(160, &titleLoop, [&] { QCursor::setPos(outside); });
		QTimer::singleShot(260, &titleLoop, [&] { sendButton(MOUSEEVENTF_LEFTUP); });
		QTimer::singleShot(500, &titleLoop, &QEventLoop::quit);
		titleLoop.exec();
		QCursor::setPos(oldCursor);
		const bool floatingMoved = b.parentWidget() &&
		                           b.parentWidget()->inherits("QDockWidgetGroupWindow") &&
		                           a.parentWidget() == b.parentWidget() &&
		                           b.parentWidget()->pos() != groupBefore;
		if (QWidget::mouseGrabber() || !floatingMoved) {
			std::cerr << "FAIL: physical title drag did not move and release the group\n";
			return 1;
		}
		QTabBar *floatingTabs = nullptr;
		for (auto *bar : b.parentWidget()->findChildren<QTabBar *>()) {
			if (bar->isVisible() && bar->count() == 2) { floatingTabs = bar; break; }
		}
		if (!floatingTabs) {
			std::cerr << "FAIL: physical floating group has no tab strip\n";
			return 1;
		}
		const QPoint floatingStart = floatingTabs->mapToGlobal(
			QPoint(floatingTabs->width() - 10, floatingTabs->height() / 2));
		if (floatingTabs->tabAt(floatingTabs->mapFromGlobal(floatingStart)) >= 0 ||
		    QApplication::widgetAt(floatingStart) != floatingTabs) {
			std::cerr << "FAIL: physical floating group's blank strip is unavailable\n";
			return 1;
		}
		const QPoint targetPoint = target.mapToGlobal(target.rect().center());
		QEventLoop redockLoop;
		QTimer::singleShot(60, &redockLoop, [&] { QCursor::setPos(floatingStart); sendButton(MOUSEEVENTF_LEFTDOWN); });
		QTimer::singleShot(160, &redockLoop, [&] { QCursor::setPos(targetPoint); });
		QTimer::singleShot(260, &redockLoop, [&] { sendButton(MOUSEEVENTF_LEFTUP); });
		QTimer::singleShot(850, &redockLoop, &QEventLoop::quit);
		redockLoop.exec();
		QCursor::setPos(oldCursor);
		if (QWidget::mouseGrabber() || a.parentWidget() != &physical ||
		    b.parentWidget() != &physical ||
		    !physical.tabifiedDockWidgets(&a).contains(&b) ||
		    physical.dockWidgetArea(&a) != Qt::RightDockWidgetArea) {
			std::cerr << "FAIL: physical blank-tab drag did not dock the floating group together\n";
			return 1;
		}
		std::cout << "PASS: real mouse input detaches, moves, and docks the whole group from blank tabs\n";
	}
	std::cout << "PASS: blank docked tab strip drags both panels and missed title release recovers\n";
	return 0;
}

#include "dock-group-drag-test.moc"
