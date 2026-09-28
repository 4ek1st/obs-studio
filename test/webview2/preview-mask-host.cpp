#include "WebView2Widget.hpp"
#include <QApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <Windows.h>
#include <iostream>

static bool checkOrigin(QWidget *widget, const char *stage)
{
  auto *dialog = widget->window();
  POINT native{};
  MapWindowPoints(reinterpret_cast<HWND>(widget->winId()), reinterpret_cast<HWND>(dialog->winId()), &native, 1);
  const QPoint expected = widget->mapTo(dialog, QPoint());
  const qreal scale = widget->devicePixelRatioF();
  const bool ok = qAbs(native.x - qRound(expected.x()*scale)) <= 2 &&
                  qAbs(native.y - qRound(expected.y()*scale)) <= 2;
  std::cout << stage << " QtInDialog=" << expected.x() << ',' << expected.y()
    << " HWNDInDialog=" << native.x << ',' << native.y << " DPR=" << scale << " match=" << ok << '\n';
  return ok;
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  if (argc != 2) return 2;
  QTemporaryDir temporary;
  if (!temporary.isValid()) return 2;
  QDialog dialog;
  dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
  dialog.resize(760,560);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(11,11,11,11);
  auto *splitter = new QSplitter(Qt::Vertical, &dialog);
  auto *frame = new QFrame(splitter);
  auto *nested = new QVBoxLayout(frame);
  nested->setContentsMargins(0,0,0,0);
  auto *nativePreview = new QWidget(frame);
  nativePreview->setObjectName("nativePreview");
  nativePreview->setAttribute(Qt::WA_DontCreateNativeAncestors);
  nativePreview->setAttribute(Qt::WA_NativeWindow);
  nativePreview->setStyleSheet("background:#00a050");
  nativePreview->setMinimumHeight(240);
  nested->addWidget(nativePreview);
  splitter->addWidget(frame);
  layout->addWidget(splitter,1);
  layout->addWidget(new QLabel("Native controls below preview",&dialog));
  dialog.show(); app.processEvents();
  const bool initial = checkOrigin(nativePreview,"before WebView");
  auto *web = new WebView2Widget(&dialog,QString::fromLocal8Bit(argv[1]),temporary.path()+"/profile");
  web->setGeometry(dialog.rect()); web->hide();
  QObject::connect(web,&WebView2Widget::ready,&app,[&] {
    const QRect slot(nativePreview->mapTo(&dialog,QPoint()),nativePreview->size());
    web->setGeometry(dialog.rect());
    web->setMask(QRegion(web->rect())-QRegion(slot));
    web->show(); web->raise(); dialog.activateWindow();
    QTimer::singleShot(500,&app,[&,slot] {
      bool ok = initial && checkOrigin(nativePreview,"after WebView mask") && checkOrigin(web,"WebView host");
      const QPoint videoPoint = nativePreview->mapToGlobal(QPoint(30,30));
      const HWND hitVideo = WindowFromPoint(POINT{videoPoint.x(),videoPoint.y()});
      const HWND previewWindow = reinterpret_cast<HWND>(nativePreview->winId());
      ok &= hitVideo == previewWindow || IsChild(previewWindow,hitVideo);
      const QRect menu(slot.topLeft()+QPoint(60,50),QSize(140,110));
      web->setMask((QRegion(web->rect())-QRegion(slot))|QRegion(menu));
      web->raise();
      QTimer::singleShot(150,&app,[&,menu,ok] {
        const QPoint point=web->mapToGlobal(menu.center());
        const HWND hit=WindowFromPoint(POINT{point.x(),point.y()});
        const HWND webWindow=reinterpret_cast<HWND>(web->winId());
        const bool menuAbove=hit==webWindow || IsChild(webWindow,hit);
        const bool alive=nativePreview->isVisible();
        std::cout << "uncovered video hit=" << ok << " menu hit=" << menuAbove << " video visible=" << alive << '\n';
        app.exit(ok && menuAbove && alive ? 0 : 1);
      });
    });
  });
  QObject::connect(web,&WebView2Widget::failed,&app,[&](const QString &error){std::cerr << error.toStdString();app.exit(1);});
  QTimer::singleShot(15000,&app,[&]{std::cerr << "mask host timed out\n";app.exit(1);});
  return app.exec();
}
