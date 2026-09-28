#include "QtDialogBridge.hpp"
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <iostream>
#include <vector>

static QJsonObject node(const QJsonObject &state, const char *name)
{
 for (const auto value : state.value("nodes").toArray()) {
  const auto result = value.toObject();
  if (result.value("name") == name) return result;
 }
 return {};
}
static QRect rect(const QJsonValue &value)
{
 const auto data = value.toObject();
 return {data.value("x").toInt(), data.value("y").toInt(), data.value("width").toInt(), data.value("height").toInt()};
}
static QImage image(const QJsonObject &data)
{
 const auto uri = data.value("decoration").toString();
 return QImage::fromData(QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1()));
}
int main(int argc, char **argv)
{
 QApplication app(argc, argv);
 int failures = 0;
 const auto check = [&](bool condition, const char *name) {
  std::cout << (condition ? "PASS: " : "FAIL: ") << name << '\n';
  failures += !condition;
 };
 QDialog dialog; dialog.resize(760, 510);
 auto *layout = new QVBoxLayout(&dialog);
 auto *area = new QScrollArea; area->setWidgetResizable(false); layout->addWidget(area);
 auto *outer = new QFrame; outer->setObjectName("longSettingsPage"); outer->resize(1200, 3600);
 outer->setStyleSheet("QFrame#longSettingsPage { border:2px solid #667788; background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #123456,stop:1 #abcdef); }");
 auto *inner = new QFrame(outer); inner->setObjectName("nestedSettingsPanel"); inner->setGeometry(8, 8, 1184, 3584);
 inner->setStyleSheet("QFrame#nestedSettingsPanel { background:#252833; border:1px solid #445566; padding:8px; margin:3px; }");
 auto *edit = new QLineEdit("Live native settings", inner); edit->setObjectName("liveSetting"); edit->setGeometry(12, 180, 250, 28);
 area->setWidget(outer); dialog.show(); app.processEvents();
 area->horizontalScrollBar()->setValue(60); area->verticalScrollBar()->setValue(140); app.processEvents();
 OBSWeb::QtDialogBridge bridge(&dialog);
 QElapsedTimer timer; timer.start();
 auto state = bridge.snapshot();
 const auto first = timer.nsecsElapsed() / 1e6;
 const auto page = node(state, "longSettingsPage");
 const auto localClip = rect(page.value("clip")).translated(-rect(page.value("rect")).topLeft());
 const auto pagePixels = image(page);
 check(!pagePixels.isNull() && pagePixels.size() == localClip.size() * outer->devicePixelRatioF(),
       "long settings pages send only the visible decoration pixels");
 check(rect(page.value("decorationRect")) == localClip,
       "scrolled decorations retain their native widget coordinate origin");
 const auto innerNode = node(state, "nestedSettingsPanel");
 const auto innerClip = rect(innerNode.value("clip")).translated(-rect(innerNode.value("rect")).topLeft());
 check(image(innerNode).size() == innerClip.size() * inner->devicePixelRatioF(),
       "nested panels are clipped too, keeping the raster working set bounded by the viewport");
 QImage nativePage(outer->size() * outer->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
 nativePage.setDevicePixelRatio(outer->devicePixelRatioF()); nativePage.fill(Qt::transparent);
 outer->render(&nativePage, QPoint(), QRegion(), QWidget::DrawWindowBackground);
 const auto sample = QPoint(pagePixels.width()/2, pagePixels.height()/2);
 const auto nativeSample = sample + localClip.topLeft() * outer->devicePixelRatioF();
 check(!pagePixels.isNull() && pagePixels.pixelColor(sample) == nativePage.pixelColor(nativeSample),
       "clipping keeps the native gradient position instead of resizing the entire painted page");
 std::vector<double> samples;
 for (int i = 0; i < 7; ++i) {
  timer.restart();
  const auto current = bridge.snapshot();
  samples.push_back(timer.nsecsElapsed() / 1e6);
  check(current == state, "unchanged native state produces identical snapshots");
 }
 std::sort(samples.begin(), samples.end());
 std::cout << "TIMING first_ms=" << first << " steady_median_ms=" << samples[samples.size()/2]
           << " steady_max_ms=" << samples.back() << " payload_bytes=" << QJsonDocument(state).toJson(QJsonDocument::Compact).size() << '\n';
 check(samples[samples.size()/2] < 120, "unchanged snapshots finish within the existing polling interval");
 const auto previous = innerNode.value("decoration");
 inner->setStyleSheet("QFrame#nestedSettingsPanel { background:#314159; border:1px solid #445566; padding:8px; margin:3px; }");
 state = bridge.snapshot();
 const auto changed = node(state, "nestedSettingsPanel");
 const auto changedPixels = image(changed);
 check(changed.value("decoration") != previous && !changedPixels.isNull() &&
       changedPixels.pixelColor(changedPixels.width()/2, changedPixels.height()/2) == QColor("#314159"),
       "a live stylesheet change invalidates cached decoration pixels");
 QString error;
 check(bridge.execute("dialog.input", {{"id", node(state, "liveSetting").value("id")}, {"value", "Edited through bridge"}}, error) &&
       edit->text() == "Edited through bridge", "clipping preserves the actual native setting editor");
 area->verticalScrollBar()->setValue(260); app.processEvents();
 state = bridge.snapshot();
 const auto scrolled = node(state, "longSettingsPage");
 check(rect(scrolled.value("decorationRect")).top() > localClip.top(), "scrolling immediately moves the sampled decoration origin");
 dialog.resize(650, 430); app.processEvents();
 const auto resized = node(bridge.snapshot(), "longSettingsPage");
 check(image(resized).size() == rect(resized.value("clip")).size() * outer->devicePixelRatioF(),
       "resizing regenerates decoration pixels for the new visible area");
 return failures ? 1 : 0;
}
