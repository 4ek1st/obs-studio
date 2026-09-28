#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"
#include "AbsoluteSlider.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QAbstractButton>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QFrame>
#include <QDockWidget>
#include <QJsonArray>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QMouseEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QIntValidator>
#include <QImage>
#include <QStandardItemModel>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <iostream>

class SourceSelectButton : public QAbstractButton {
	Q_OBJECT
protected:
	void paintEvent(QPaintEvent *) override {}
};
class OBSHotkeyLabel : public QLabel { Q_OBJECT };
class BalanceSlider : public QSlider { Q_OBJECT };

static int failures = 0;
static void check(bool good, const char *label)
{
	if (!good) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
static QJsonObject node(OBSWeb::QtDialogBridge &bridge, const char *name)
{
	for (const auto entry : bridge.snapshot().value("nodes").toArray()) {
		const auto item = entry.toObject();
		if (item.value("name") == name) return item;
	}
	return {};
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QDialog dialog;
	dialog.resize(600, 700);
	auto *layout = new QVBoxLayout(&dialog);
	auto *edit = new QLineEdit("before", &dialog);
	edit->setObjectName("edit"); layout->addWidget(edit);
	auto *secret = new QLineEdit("secret", &dialog);
	secret->setEchoMode(QLineEdit::Password); secret->setObjectName("secret"); layout->addWidget(secret);
	auto *checkBox = new QCheckBox("Enabled", &dialog);
	checkBox->setObjectName("check"); layout->addWidget(checkBox);
	auto *combo = new QComboBox(&dialog);
	combo->setObjectName("combo"); combo->addItems({"One", "Two"}); layout->addWidget(combo);
	auto *spin = new QSpinBox(&dialog);
	spin->setObjectName("spin"); spin->setRange(1, 20); layout->addWidget(spin);
	auto *list = new QListView(&dialog);
	list->setObjectName("list"); layout->addWidget(list);
	QStandardItemModel model;
	model.appendRow(new QStandardItem("Alpha")); model.appendRow(new QStandardItem("Beta"));
	list->setModel(&model);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	buttons->button(QDialogButtonBox::Ok)->setObjectName("ok");
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.show(); app.processEvents();
	OBSWeb::QtDialogBridge bridge(&dialog);
	QPalette testPalette = dialog.palette();
	testPalette.setColor(QPalette::Window, QColor("#123456"));
	testPalette.setColor(QPalette::Base, QColor("#234567"));
	dialog.setPalette(testPalette);
	QFont testFont = dialog.font(); testFont.setPixelSize(17); dialog.setFont(testFont);
	const auto theme = bridge.snapshot().value("theme").toObject();
	if (theme.value("window") != "#123456" || theme.value("base") != "#234567" || theme.value("fontSize") != 17)
		std::cerr << "Theme values: " << theme.value("window").toString().toStdString() << ' ' << theme.value("base").toString().toStdString() << ' ' << theme.value("fontSize").toInt() << '\n';
	check(theme.value("window") == "#123456" && theme.value("base") == "#234567" && theme.value("fontSize") == 17,
	      "HTML receives the live Qt palette and logical font size");
	checkBox->setStyleSheet("QCheckBox { font-size:11px; font-weight:600; font-style:italic; spacing:2px; }");
	const auto checkNode = node(bridge, "check");
	const auto checkFont = checkNode.value("font").toObject();
	check(checkFont.value("pixelSize") == 11 && checkFont.value("weight") == 600 && checkFont.value("italic").toBool() &&
	      checkFont.value("lineHeight").toDouble() == QFontMetricsF(checkBox->font()).lineSpacing(),
	      "a widget's stylesheet font overrides the dialog font in HTML metrics");
	QStyleOptionButton checkStyle; checkStyle.initFrom(checkBox); checkStyle.text = checkBox->text();
	const auto expectedIndicator = checkBox->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &checkStyle, checkBox);
	const auto expectedText = checkBox->style()->subElementRect(QStyle::SE_CheckBoxContents, &checkStyle, checkBox);
	auto asRect = [](const QJsonValue &value) { const auto r = value.toObject(); return QRect(r.value("x").toInt(), r.value("y").toInt(), r.value("width").toInt(), r.value("height").toInt()); };
	check(asRect(checkNode.value("indicatorRect")) == expectedIndicator && asRect(checkNode.value("textRect")) == expectedText,
	      "checkbox layout follows native style indicator and text rectangles instead of fixed browser gaps");
	check(checkNode.value("nativeTextWidth").toDouble() == QFontMetricsF(checkBox->font()).horizontalAdvance(checkBox->text()),
	      "checkbox text advance permits exact fitting across browser font metrics");
	checkBox->setText("&Capture && monitor");
	check(node(bridge, "check").value("nativeTextWidth").toDouble() ==
	      QFontMetricsF(checkBox->font()).horizontalAdvance("Capture & monitor"),
	      "checkbox text advance excludes Qt mnemonics but retains literal ampersands");
	QString error;
	auto run = [&](const char *command, QJsonObject args) { return bridge.execute(QString::fromLatin1(command), args, error); };
	const auto editId = node(bridge, "edit").value("id");
	check(editId.isString(), "visible controls receive stable IDs");
	check(node(bridge, "secret").value("password").toBool(), "password editor remains masked");
	int edits = 0, finished = 0, activated = 0;
	QObject::connect(edit, &QLineEdit::textEdited, [&] { ++edits; });
	QObject::connect(edit, &QLineEdit::editingFinished, [&] { ++finished; });
	QObject::connect(combo, &QComboBox::activated, [&](int index) { activated = index + 1; });
	check(run("dialog.input", {{"id", editId}, {"value", "after"}}) && edit->text() == "after" && edits == 1,
	      "text input invokes native textEdited listeners");
	check(run("dialog.finish", {{"id", editId}}) && finished == 1, "blur invokes editingFinished exactly once");
	check(edit->isUndoAvailable(), "web edits preserve native text undo history");
	edit->undo();
	check(edit->text() == "before", "native undo restores the previous web edited value");
	run("dialog.input", {{"id", editId}, {"value", "after"}});
	check(run("dialog.selection", {{"id", editId}, {"start", 1}, {"end", 4}}) && edit->selectedText() == "fte",
	      "native context actions see browser text selection");
	check(run("dialog.click", {{"id", node(bridge, "check").value("id")}}) && checkBox->isChecked(), "checkbox click invokes native toggle");
	check(run("dialog.choose", {{"id", node(bridge, "combo").value("id")}, {"index", 1}}) && combo->currentIndex() == 1 && activated == 2,
	      "combobox user choice invokes activated");
	check(!run("dialog.choose", {{"id", node(bridge, "combo").value("id")}, {"index", 99}}), "invalid combo index rejected");
	check(run("dialog.input", {{"id", node(bridge, "spin").value("id")}, {"value", 12}}) && spin->value() == 12,
	      "number input updates live native value");
	check(!run("dialog.input", {{"id", node(bridge, "spin").value("id")}, {"value", 21}}) && spin->value() == 12,
	      "out of range input cannot mutate live control");
	auto *validated = new QLineEdit("10", &dialog);
	validated->setValidator(new QIntValidator(10, 99, validated)); validated->setObjectName("validated");
	layout->insertWidget(1, validated); validated->show(); app.processEvents();
	const auto validatedId = node(bridge, "validated").value("id");
	check(!run("dialog.input", {{"id", validatedId}, {"value", "letters"}}) && validated->text() == "10", "invalid native validator input is rejected");
	QDialog other;
	auto *moved = new QLineEdit("owned", &dialog); moved->setObjectName("moved"); layout->addWidget(moved); moved->show(); app.processEvents();
	const auto movedId = node(bridge, "moved").value("id");
	moved->setParent(&other); moved->show(); other.show(); app.processEvents();
	check(!run("dialog.input", {{"id", movedId}, {"value", "wrong owner"}}) && moved->text() == "owned", "reparented controls cannot be modified by their old dialog");
	other.hide();
	const auto listNode = node(bridge, "list");
	const auto items = listNode.value("items").toArray();
	check(items.size() == 2, "visible model rows serialized");
	check(!items.isEmpty() && !items.at(0).toObject().value("checkable").toBool(), "rows without check state do not gain artificial checkboxes");
	const auto betaId = items.size() > 1 ? items.at(1).toObject().value("id") : QJsonValue();
	check(run("dialog.item", {{"id", listNode.value("id")}, {"item", betaId}, {"action", "select"}}) && list->currentIndex().row() == 1,
	      "model click updates actual native selection");
	check(run("dialog.item", {{"id", listNode.value("id")}, {"item", betaId}, {"action", "edit"}}), "native delegate editing opens");
	app.processEvents();
	QJsonObject delegate;
	for (const auto entry : bridge.snapshot().value("nodes").toArray()) if (entry.toObject().value("itemView") == listNode.value("id")) delegate = entry.toObject();
	check(!delegate.isEmpty(), "native delegate editor is identified separately from dialog inputs");
	check(run("dialog.input", {{"id", delegate.value("id")}, {"value", "Beta renamed"}}) &&
	      run("dialog.key", {{"id", delegate.value("id")}, {"key", "Enter"}}), "delegate Enter commits through the Qt event filter");
	app.processEvents();
	check(model.index(1, 0).data() == "Beta renamed" && dialog.isVisible(), "delegate Enter updates model without accepting its dialog");
	list->setDragDropMode(QAbstractItemView::InternalMove);
	check(node(bridge, "list").value("type") == "native", "drag-enabled model views preserve native reorder and drop semantics");
	list->setDragDropMode(QAbstractItemView::NoDragDrop);
	model.insertRow(0, new QStandardItem("Inserted"));
	check(run("dialog.item", {{"id", listNode.value("id")}, {"item", betaId}, {"action", "select"}}) && list->currentIndex().data() == "Beta renamed",
	      "persistent index targets the same item after insertion");
	model.clear();
	check(!run("dialog.item", {{"id", listNode.value("id")}, {"item", betaId}, {"action", "select"}}), "model reset invalidates stale row IDs");
	edit->setDisabled(true);
	check(!run("dialog.input", {{"id", editId}, {"value", "blocked"}}) && edit->text() == "after", "disabled controls reject mutations");
	delete edit;
	check(!run("dialog.input", {{"id", editId}, {"value", "stale"}}), "deleted controls reject stale IDs");
	check(run("dialog.click", {{"id", node(bridge, "ok").value("id")}}) && dialog.result() == QDialog::Accepted && !dialog.isVisible(),
	      "native OK retains QDialog acceptance and callbacks");
	dialog.show(); app.processEvents();
	int returned = 0;
	QObject::connect(validated, &QLineEdit::returnPressed, [&] { ++returned; });
	buttons->button(QDialogButtonBox::Ok)->setDefault(true);
	check(run("dialog.key", {{"id", validatedId}, {"key", "Enter"}}) && returned == 1 &&
	      dialog.result() == QDialog::Accepted && !dialog.isVisible(), "Enter reaches the line edit and then the native default dialog action");
	dialog.show(); app.processEvents();
	check(run("dialog.key", {{"key", "Escape"}}) && dialog.result() == QDialog::Rejected && !dialog.isVisible(), "Escape preserves native dialog rejection");
	auto *shortDialog = new QDialog;
	OBSWeb::QtDialogBridge shortBridge(shortDialog);
	delete shortDialog;
	check(shortBridge.snapshot().value("closed").toBool() && !shortBridge.execute("dialog.key", {{"key", "Enter"}}, error),
	      "dialog deletion leaves no usable dangling targets");
	QDialog customDialog;
	auto *customLayout = new QVBoxLayout(&customDialog);
	auto *sourceTile = new SourceSelectButton; sourceTile->setObjectName("sourceTile"); sourceTile->setFixedSize(160, 100);
	auto *tileLayout = new QVBoxLayout(sourceTile); tileLayout->addWidget(new QLabel("Existing OBS source", sourceTile));
	customLayout->addWidget(sourceTile);
	auto *hotkeyLabel = new OBSHotkeyLabel; hotkeyLabel->setObjectName("hotkeyLabel"); hotkeyLabel->setText("Hotkey conflict link"); customLayout->addWidget(hotkeyLabel);
	auto *balance = new BalanceSlider; balance->setObjectName("balance"); balance->setOrientation(Qt::Horizontal); customLayout->addWidget(balance);
	auto *seek = new AbsoluteSlider(Qt::Horizontal); seek->setObjectName("seek"); seek->setRange(0, 1000); customLayout->addWidget(seek);
	customDialog.show(); app.processEvents();
	OBSWeb::QtDialogBridge customBridge(&customDialog);
	check(node(customBridge, "sourceTile").value("type") == "native", "source thumbnail tile preserves its native drawing and drag interaction");
	check(node(customBridge, "hotkeyLabel").value("type") == "native", "hotkey label retains its custom click handling");
	check(node(customBridge, "balance").value("type") == "native", "audio balance slider retains its double click reset");
	check(node(customBridge, "seek").value("type") == "native", "media AbsoluteSlider remains a native interactive island");
	int pressed = 0, seekMoved = 0, released = 0, hovered = 0;
	QObject::connect(seek, &QAbstractSlider::sliderPressed, [&] { ++pressed; });
	QObject::connect(seek, &QAbstractSlider::sliderMoved, [&] { ++seekMoved; });
	QObject::connect(seek, &QAbstractSlider::sliderReleased, [&] { ++released; });
	QObject::connect(seek, &AbsoluteSlider::absoluteSliderHovered, [&] { ++hovered; });
	const QPoint start(seek->width() / 4, seek->height() / 2), end(seek->width() * 3 / 4, seek->height() / 2);
	QMouseEvent seekPress(QEvent::MouseButtonPress, QPointF(start), QPointF(seek->mapToGlobal(start)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(seek, &seekPress);
	const auto startedAt = seek->value();
	QMouseEvent seekMove(QEvent::MouseMove, QPointF(end), QPointF(seek->mapToGlobal(end)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(seek, &seekMove);
	QMouseEvent seekRelease(QEvent::MouseButtonRelease, QPointF(end), QPointF(seek->mapToGlobal(end)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(seek, &seekRelease);
	check(pressed == 1 && seekMoved >= 2 && released == 1 && hovered == 1 && seek->value() > startedAt && !seek->isSliderDown(),
	      "real AbsoluteSlider native drag retains seek start, movement, hover, and release callbacks");
	customDialog.hide();
	QDialog menuDialog;
	auto *menuLayout = new QVBoxLayout(&menuDialog);
	auto *tool = new QToolButton; tool->setObjectName("tools"); tool->setText("Tools");
	auto *menu = new QMenu(tool); menu->addAction("Native command"); tool->setMenu(menu); tool->setPopupMode(QToolButton::InstantPopup);
	menuLayout->addWidget(tool); menuDialog.show(); app.processEvents();
	OBSWeb::QtDialogBridge menuBridge(&menuDialog);
	const auto menuNode = node(menuBridge, "tools");
	check(menuNode.value("menu").toBool(), "toolbutton popup affordance is exposed to HTML");
	bool popupShown = false;
	QTimer::singleShot(0, menu, [&] { popupShown = menu->isVisible(); menu->close(); });
	check(menuBridge.execute("dialog.click", {{"id", menuNode.value("id")}}, error) && popupShown,
	      "instant-popup toolbutton opens its real native menu");
	tool->setPopupMode(QToolButton::MenuButtonPopup); popupShown = false;
	QTimer::singleShot(0, menu, [&] { popupShown = menu->isVisible(); menu->close(); });
	check(menuBridge.execute("dialog.menu", {{"id", menuNode.value("id")}}, error) && popupShown,
	      "split toolbutton menu can be opened independently of its main action");
	menuDialog.hide();
	QDialog scrolling;
	scrolling.resize(320, 180);
	auto *scrollLayout = new QVBoxLayout(&scrolling);
	auto *area = new QScrollArea; area->setObjectName("settingsScroll"); area->setWidgetResizable(true);
	area->verticalScrollBar()->setObjectName("propertiesScrollBar");
	auto *content = new QWidget; content->setMinimumSize(250, 1600); area->setWidget(content); scrollLayout->addWidget(area);
	scrolling.show(); app.processEvents();
	OBSWeb::QtDialogBridge scrollBridge(&scrolling);
	const auto scrollNode = node(scrollBridge, "settingsScroll");
	check(scrollNode.value("type") == "scrollArea", "settings scroll area is represented as a wheel target");
	check(node(scrollBridge, "propertiesScrollBar").value("page").toInt() == area->verticalScrollBar()->pageStep(),
	      "scrollbar exposes native page size for proportional browser thumb geometry");
	auto *styledScroll = area->verticalScrollBar();
	styledScroll->setStyleSheet("QScrollBar:vertical { background:#123456; width:18px; margin:3px; border:0; }"
		"QScrollBar::handle:vertical { background:#2468ac; min-height:40px; }"
		"QScrollBar::handle:vertical:hover { background:#468ace; }"
		"QScrollBar::handle:vertical:pressed { background:#689cee; }"
		"QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; border:0; background:none; }"
		"QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background:none; }");
	app.processEvents();
	const auto nativeScroll = node(scrollBridge, "propertiesScrollBar").value("nativeStyle").toObject();
	const auto thumbRect = asRect(nativeScroll.value("thumbRect"));
	check(thumbRect.height() >= 40 && asRect(nativeScroll.value("grooveRect")).height() < styledScroll->height(),
	      "scrollbar geometry preserves the native QSS minimum handle and groove margins");
	auto imageFromUri = [](const QJsonValue &value) { return QImage::fromData(QByteArray::fromBase64(value.toString().section(',', 1).toLatin1())); };
	const auto normalScroll = imageFromUri(nativeScroll.value("normal"));
	const auto hoverScroll = imageFromUri(nativeScroll.value("hover"));
	const auto pressedScroll = imageFromUri(nativeScroll.value("pressed"));
	const auto thumbPixel = thumbRect.center() * styledScroll->devicePixelRatioF();
	check(!normalScroll.isNull() && normalScroll.pixelColor(thumbPixel) == QColor("#2468ac") &&
	      !hoverScroll.isNull() && hoverScroll.pixelColor(thumbPixel) == QColor("#468ace") &&
	      !pressedScroll.isNull() && pressedScroll.pixelColor(thumbPixel) == QColor("#689cee"),
	      "scrollbar normal hover and pressed visuals come from real QSS subcontrol painting");
	check(!normalScroll.isNull() && normalScroll.pixelColor(thumbPixel) == styledScroll->grab().toImage().pixelColor(thumbPixel),
	      "serialized scrollbar paint matches the actual native widget at the handle");
	check(scrollBridge.execute("dialog.wheel", {{"id", scrollNode.value("id")}, {"deltaY", 120}, {"deltaX", 0}}, error) &&
	      area->verticalScrollBar()->value() > 0, "wheel over a settings form moves its real Qt scroll position");
	scrolling.hide();
	QObject installerOwner;
	InstallWebView2Dialogs(&installerOwner, "missing-test-assets", "unused-test-profile");
	QMainWindow dockingHost;
	auto *dock = new QDockWidget("Statistics", &dockingHost);
	auto *dockContent = new QWidget;
	auto *dockLayout = new QVBoxLayout(dockContent);
	auto *dockButton = new QPushButton("Reset statistics", dockContent); dockButton->setObjectName("dockReset"); dockLayout->addWidget(dockButton);
	dock->setWidget(dockContent); dockingHost.addDockWidget(Qt::LeftDockWidgetArea, dock);
	dockingHost.show(); app.processEvents();
	check(dockContent->findChildren<WebView2Widget *>().isEmpty(), "docked panels retain their existing native content");
	dock->setFloating(true); app.processEvents(); app.processEvents();
	QPointer<WebView2Widget> floatingSurface = dockContent->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
	check(floatingSurface && floatingSurface->parentWidget() == dockContent && floatingSurface->geometry() == dockContent->rect(),
	      "floating dock HTML overlay covers content while preserving native titlebar ownership");
	OBSWeb::QtDialogBridge dockBridge(dockContent);
	int dockResetCount = 0;
	QObject::connect(dockButton, &QPushButton::clicked, [&] { ++dockResetCount; });
	check(dockBridge.snapshot().value("title") == "Statistics" && dockBridge.execute("dialog.click", {{"id", node(dockBridge, "dockReset").value("id")}}, error) && dockResetCount == 1,
	      "floating QWidget content uses the same bridge and invokes its original native controller");
	dock->setFloating(false); app.processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	check(!floatingSurface && dockContent->findChildren<WebView2Widget *>().isEmpty() && dock->widget() == dockContent && dockButton->parentWidget() == dockContent,
	      "redocking retires the web overlay without reparenting native controllers");
	dock->setFloating(true); app.processEvents(); app.processEvents();
	check(dockContent->findChildren<WebView2Widget *>().size() == 1, "floating again installs exactly one content renderer");
	QPointer<WebView2Widget> replacedSurface = dockContent->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
	dockContent->setProperty("_obsWebView2ExternalSurface", true);
	auto *externalSurface = new WebView2Widget(dockContent, "missing-test-assets", "unused-test-profile");
	externalSurface->setObjectName("coreDockWebView");
	app.processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	check(!replacedSurface && dockContent->findChildren<WebView2Widget *>().size() == 1 && externalSurface->parentWidget() == dockContent,
	      "claiming a floating dock for a dedicated renderer retires the generic overlay");
	dock->setFloating(false); app.processEvents(); dock->setFloating(true); app.processEvents(); app.processEvents();
	check(dockContent->findChildren<WebView2Widget *>().size() == 1 &&
	      !dockContent->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly),
	      "floating a core dock with a dedicated renderer never adds a second WebView");
	dockingHost.hide();
	QLineEdit rootEditor("Dock leaf"); rootEditor.setObjectName("rootEditor"); rootEditor.show();
	OBSWeb::QtDialogBridge rootEditorBridge(&rootEditor);
	check(node(rootEditorBridge, "rootEditor").value("type") == "text" &&
	      rootEditorBridge.execute("dialog.input", {{"id", node(rootEditorBridge, "rootEditor").value("id")}, {"value", "Leaf updated"}}, error) && rootEditor.text() == "Leaf updated",
	      "a dock content widget that is itself an editor remains represented and operable");
	rootEditor.hide();
	QDialog opaqueDialog; opaqueDialog.resize(320, 180);
	auto *opaque = new QWidget(&opaqueDialog); opaque->setGeometry(opaqueDialog.rect()); opaqueDialog.show();
	app.processEvents(); app.processEvents(); app.processEvents();
	auto *opaqueWeb = opaqueDialog.findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly);
	if (opaqueWeb) emit opaqueWeb->ready();
	check(opaqueWeb && !opaqueWeb->isVisible() && opaque->isVisible(),
	      "an entirely native content area hides HTML instead of clearing its empty mask and covering controls");
	opaqueDialog.hide();
	QDialog panels;
	auto *panelsLayout = new QVBoxLayout(&panels);
	auto *styledPanel = new QFrame; styledPanel->setObjectName("styledPanel"); styledPanel->setMinimumSize(220, 50);
	styledPanel->setStyleSheet("QFrame#styledPanel { background-color:#252833; border:2px solid #434650; }");
	panelsLayout->addWidget(styledPanel);
	auto *paddedPanel = new QFrame; paddedPanel->setObjectName("paddedPanel"); paddedPanel->setFrameShape(QFrame::NoFrame);
	paddedPanel->setMinimumSize(220, 70);
	paddedPanel->setStyleSheet("QFrame#paddedPanel { background:#252833; border:1px solid #556677; padding:8px; margin:3px; border-radius:4px; }");
	panelsLayout->addWidget(paddedPanel);
	auto *swatch = new QLabel("#ffd1d1d1"); swatch->setObjectName("colorSwatch");
	swatch->setFrameStyle(QFrame::Sunken | QFrame::Panel);
	swatch->setPalette(QPalette(QColor("#d1d1d1")));
	swatch->setStyleSheet("background-color:#d1d1d1; color:#000000;");
	swatch->setAutoFillBackground(true); swatch->setAlignment(Qt::AlignCenter);
	panelsLayout->addWidget(swatch);
	auto *transparentPanel = new QFrame; transparentPanel->setObjectName("transparentPanel"); transparentPanel->setFrameShape(QFrame::NoFrame);
	auto *transparentLayout = new QVBoxLayout(transparentPanel);
	auto *disabledLabel = new QLabel("Disabled caption"); disabledLabel->setObjectName("disabledCaption");
	QPalette labelPalette = disabledLabel->palette(); labelPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#818293"));
	disabledLabel->setPalette(labelPalette); disabledLabel->setDisabled(true); transparentLayout->addWidget(disabledLabel);
	panelsLayout->addWidget(transparentPanel); panels.show(); app.processEvents();
	OBSWeb::QtDialogBridge panelBridge(&panels);
	const auto panelNode = node(panelBridge, "styledPanel");
	check(panelNode.value("type") == "panel" && panelNode.value("background") == "#252833" && panelNode.value("frameWidth") == 2,
	      "styled container preserves its own background and native frame width");
	const auto paddedNode = node(panelBridge, "paddedPanel");
	const auto paddedPaint = imageFromUri(paddedNode.value("decoration"));
	check(paddedNode.value("frameWidth").toInt() > 1 && !paddedPaint.isNull() &&
	      paddedPaint.pixelColor(paddedPaint.width() / 2, 3) == QColor("#556677") &&
	      paddedPaint.pixelColor(paddedPaint.width() / 2, 5) == QColor("#252833") && paddedPaint.pixelColor(0, 0).alpha() == 0,
	      "styled panel padding and margins are not painted as a thick border");
	check(node(panelBridge, "transparentPanel").isEmpty(), "transparent grouping container does not gain an opaque HTML panel");
	check(node(panelBridge, "disabledCaption").value("palette").toObject().value("windowText") == "#818293",
	      "disabled controls publish their own disabled text palette");
	const auto swatchNode = node(panelBridge, "colorSwatch");
	const auto swatchPaint = imageFromUri(swatchNode.value("decoration"));
	check(swatchNode.value("background") == "#d1d1d1" && swatchNode.value("frameWidth").toInt() == swatch->frameWidth() &&
	      !swatchPaint.isNull() && swatchPaint.pixelColor(swatchPaint.width() / 2, swatchPaint.height() / 2) == QColor("#d1d1d1"),
	      "color property QLabel retains its visible swatch background and native frame");
	check(!node(panelBridge, "disabledCaption").contains("decoration"),
	      "plain transparent labels do not acquire a painted background");
	panels.hide();
	if (!failures) std::cout << "PASS: native dialog edits, activation, model identity, deletion and acceptance\n";
	return failures ? 1 : 0;
}

#include "dialog-bridge-test.moc"
