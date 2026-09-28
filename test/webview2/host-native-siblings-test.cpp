#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <Windows.h>
#include <iostream>

class NativeIsland : public QWidget {
	Q_OBJECT
public:
	using QWidget::QWidget;
	int presses = 0;
protected:
	void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), QColor("#316294")); }
	void mousePressEvent(QMouseEvent *) override { ++presses; }
};

static QJsonObject node(const QJsonObject &state, const QString &name)
{
	for (const auto value : state.value("nodes").toArray())
		if (value.toObject().value("name") == name) return value.toObject();
	return {};
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	if (argc != 2 || !temporary.isValid()) return 2;
	const auto assets = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
	// Exercise the Windows default; the host must restore the caller's attribute.
	QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, false);
	int failures = 0, checks = 0;
	const auto check = [&](bool value, const char *description) {
		++checks; failures += !value;
		std::cout << (value ? "PASS: " : "FAIL: ") << description << std::endl;
	};
	QDialog dialog;
	dialog.resize(800, 600);
	auto *layout = new QVBoxLayout(&dialog);
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true); layout->addWidget(scroll);
	auto *content = new QWidget;
	auto *form = new QFormLayout(content); scroll->setWidget(content);
	dialog.show(); app.processEvents();
	const auto nativeCount = [&] {
		int result = 0;
		for (auto *widget : dialog.findChildren<QWidget *>())
			if (!qobject_cast<WebView2Widget *>(widget) &&
			    (widget->internalWinId() || widget->testAttribute(Qt::WA_NativeWindow))) ++result;
		return result;
	};
	check(nativeCount() == 0, "ordinary Qt controls begin without native windows");
	auto *web = new WebView2Widget(&dialog, assets, temporary.filePath("profile"));
	web->setGeometry(dialog.rect());
	check(!QCoreApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings),
	      "creating a WebView restores the caller's application attribute");
	check(nativeCount() == 0, "creating a WebView does not force its ordinary Qt siblings native");
	// Fail before laying out hundreds of forced HWNDs on the buggy implementation.
	if (failures) return 1;
	QObject::connect(web, &WebView2Widget::failed, &app, [&](const QString &error) {
		std::cerr << "FAIL: WebView startup: " << error.toStdString() << std::endl; app.exit(1);
	});
	QObject::connect(web, &WebView2Widget::ready, &app, [&] {
		QTimer::singleShot(0, &app, [&] {
			check(web->internalWinId() && IsWindow(reinterpret_cast<HWND>(web->internalWinId())),
			      "the real WebView controller keeps its required native host window");
			check(nativeCount() == 0, "asynchronous controller creation keeps existing Qt controls alien");
			QElapsedTimer timer; timer.start();
			QLineEdit *first = nullptr;
			for (int index = 0; index < 60; ++index) {
				// OBSHotkeyWidget/OBSHotkeyEdit initially parent to Settings, then the
				// form layout reparents them into the lazy Hotkeys page.
				auto *row = new QWidget(&dialog);
				auto *controls = new QHBoxLayout(row); controls->setContentsMargins(0, 2, 0, 2);
				auto *edit = new QLineEdit(row->parentWidget()); controls->addWidget(edit);
				if (!first) { first = edit; first->setObjectName("lazyHotkey"); }
				for (int button = 0; button < 4; ++button) controls->addWidget(new QPushButton(QString::number(button)));
				form->addRow(new QLabel(QString::number(index)), row);
			}
			form->activate(); content->adjustSize(); app.processEvents();
			const auto elapsed = timer.nsecsElapsed() / 1e6;
			std::cout << "TIMING lazy_60_rows_ms=" << elapsed << " ordinary_native_count=" << nativeCount() << std::endl;
			check(nativeCount() == 0, "lazy hotkey-shaped controls do not inherit nativeChildrenForced");
			check(elapsed < 1500, "lazy controls avoid the measured multi-second native-window cascade");
			OBSWeb::QtDialogBridge bridge(&dialog);
			QString error;
			const auto firstNode = node(bridge.snapshot(), "lazyHotkey");
			const bool edited = bridge.execute("dialog.input", {{"id", firstNode.value("id")}, {"value", "live Qt value"}}, error);
			if (!edited) std::cerr << "Editor fixture: " << error.toStdString() << std::endl;
			check(edited &&
			      first->text() == "live Qt value", "the raster-backed editor remains a live Qt control");
			auto *islandParent = new QWidget(&dialog); islandParent->setGeometry(600, 16, 100, 40); islandParent->show();
			auto *island = new NativeIsland(islandParent); island->setObjectName("explicitNativeIsland"); island->setGeometry(islandParent->rect());
			island->setAttribute(Qt::WA_DontCreateNativeAncestors);
			island->setAttribute(Qt::WA_NativeWindow); island->show();
			web->setMask(QRegion(web->rect()).subtracted(QRegion(islandParent->geometry())));
			app.processEvents();
			const auto handle = reinterpret_cast<HWND>(island->internalWinId());
			check(handle && IsWindow(handle) && node(bridge.snapshot(), "explicitNativeIsland").value("type") == "native",
			      "explicit native custom surfaces keep a real HWND and their bridge island");
			SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
			SendMessageW(handle, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
			app.processEvents();
			check(island->presses == 1, "the explicit native custom surface still handles a native mouse event");
			const auto pixels = island->grab().toImage();
			check(!pixels.isNull() && pixels.pixelColor(pixels.width() / 2, pixels.height() / 2) == QColor("#316294"),
			      "the explicit native custom surface still paints its own pixels");
			QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
			{ WebView2Widget other(nullptr, assets, temporary.filePath("unused")); }
			check(QCoreApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings),
			      "a previously enabled sibling policy is preserved too");
			QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, false);
			std::cout << "RESULT checks=" << checks << " failures=" << failures << std::endl;
			app.exit(failures ? 1 : 0);
		});
	});
	web->show();
	QTimer::singleShot(20000, &app, [&] { std::cerr << "FAIL: real WebView ready timed out\n"; app.exit(1); });
	return app.exec();
}

#include "host-native-siblings-test.moc"
