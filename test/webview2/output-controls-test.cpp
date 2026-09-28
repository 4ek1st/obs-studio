#include <ControlBridge.hpp>
#include <QApplication>
#include <QMenu>
#include <QPushButton>
#include <QPixmap>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	int failures = 0, checks = 0;
	auto check = [&](bool pass, const char *label) {
		++checks;
		if (!pass) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
	};
	QPushButton button;
	button.setAccessibleName("Pause recording");
	button.setToolTip("Pause recording");
	button.setProperty("class", "icon-media-pause state-active");
	QPixmap icon(16, 16);
	icon.fill(Qt::red);
	button.setIcon(QIcon(icon));
	QHash<qint64, QString> cache;
	const auto state = OBSWeb::ControlPresentation(&button, "pauseRecordButton", "recordingLayout", cache);
	check(state.value("active").toBool() && !state.value("checkable").toBool(),
	      "native state-active is preserved independently of checkable");
	check(state.value("iconOnly").toBool() && state.value("icon").toString().startsWith("data:image/png;base64,"),
	      "native side buttons retain their actual icon and compact presentation");
	check(state.value("group").toString() == "recordingLayout" && state.value("text").toString() == "Pause recording",
	      "button group and accessible native label are retained");
	button.setProperty("class", QStringList{"icon-media-pause", "state-active"});
	check(OBSWeb::ControlPresentation(&button, "pauseRecordButton", "recordingLayout", cache).value("active").toBool(),
	      "both Qt string and string-list class formats preserve the active state");
	int clicked = 0, chosen = 0;
	QObject::connect(&button, &QPushButton::clicked, [&] { ++clicked; });
	QMenu menu;
	auto *stop = menu.addAction("Stop delayed stream");
	auto *forceStop = menu.addAction("Force stop");
	QObject::connect(forceStop, &QAction::triggered, [&] { ++chosen; });
	button.setMenu(&menu);
	OBSWeb::ActivateControlButton(&button);
	check(menu.isVisible() && clicked == 0, "opening a delayed-stream menu does not invoke ordinary start or stop");
	menu.close();
	check(clicked == 0 && chosen == 0, "cancelling the delayed-stream menu performs no output action");
	OBSWeb::ActivateControlButton(&button);
	forceStop->trigger();
	menu.close();
	check(chosen == 1 && clicked == 0 && stop->isEnabled(), "only the selected native menu action is invoked");
	button.setMenu(nullptr);
	OBSWeb::ActivateControlButton(&button);
	check(clicked == 1, "a control without an attached menu retains its ordinary click");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
