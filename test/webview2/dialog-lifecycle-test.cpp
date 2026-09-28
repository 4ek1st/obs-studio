#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (argc != 2) return 2;
	QTemporaryDir profile;
	QMainWindow main; main.resize(800, 550); main.show();
	QObject owner;
	InstallWebView2Dialogs(&owner, QString::fromLocal8Bit(argv[1]), profile.path());
	QDialog dialog(&main); dialog.setWindowTitle("Disposable modeless lifecycle fixture"); dialog.resize(600, 350);
	auto *layout = new QVBoxLayout(&dialog);
	auto *table = new QTableView; QStandardItemModel model(1, 3); model.setData(model.index(0, 0), "Preserved queue entry"); table->setModel(&model);
	table->setEditTriggers(QAbstractItemView::CurrentChanged); layout->addWidget(table);
	auto *button = new QPushButton("Close"); layout->addWidget(button);
	QObject::connect(button, &QPushButton::clicked, &dialog, &QDialog::reject);
	dialog.show();
	QTimer::singleShot(0, table, [table, &model] { table->setCurrentIndex(model.index(0, 1)); });
	int stage = 0;
	QElapsedTimer elapsed; elapsed.start();
	QTimer poll;
	QObject::connect(&poll, &QTimer::timeout, &app, [&] {
		auto *surface = dialog.findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
		if (surface && !surface->property("lifecycleProbeObserved").toBool()) {
			surface->setProperty("lifecycleProbeObserved", true);
			QObject::connect(surface, &WebView2Widget::failed, &app, [](const QString &error) {
				std::cerr << "HOST FAILURE: " << error.toStdString() << std::endl;
			});
		}
		if (surface && surface->isVisible() && surface->property("webview2Presented").toBool() && dialog.windowOpacity() > 0) {
			if (stage == 0) {
				std::cout << "PASS: first modeless surface ready" << std::endl;
				stage = 1; dialog.reject();
				QTimer::singleShot(300, &dialog, [&] { stage = 2; dialog.show(); dialog.raise(); });
			} else if (stage == 2) {
				if (dialog.findChildren<WebView2Widget *>().size() != 1 || model.index(0, 0).data().toString() != "Preserved queue entry") {
					std::cerr << "FAIL: reopened dialog lost model state or duplicated the WebView" << std::endl;
					app.exit(1); return;
				}
				std::cout << "PASS: same modeless dialog reopens with one visible WebView and retains its native model" << std::endl;
				app.exit(0);
			}
		} else if (elapsed.elapsed() > 10000) {
			std::cerr << "FAIL: lifecycle stage=" << stage << " dialogVisible=" << dialog.isVisible()
				<< " surface=" << bool(surface) << " hidden=" << (surface && surface->isHidden())
				<< " fallback=" << dialog.property("webview2NativeFallback").toBool() << std::endl;
			app.exit(1);
		}
	});
	poll.start(80);
	return app.exec();
}
