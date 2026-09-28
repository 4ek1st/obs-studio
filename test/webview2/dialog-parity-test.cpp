#include "QtDialogBridge.hpp"
#include "ExternalDrop.hpp"
#include "ClickableLabel.hpp"
#include "SpinBox.hpp"
#include "DoubleSpinBox.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QIntValidator>
#include <QImage>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFile>
#include <QJsonDocument>
#include <QHeaderView>
#include <QLabel>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextBrowser>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <QWizard>
#include <iostream>

static QJsonObject named(const QJsonObject &state, const QString &name)
{
	for (const auto entry : state.value("nodes").toArray())
		if (entry.toObject().value("name") == name) return entry.toObject();
	return {};
}

class MandatoryPage : public QWizardPage {
public:
	QLineEdit *edit = new QLineEdit(this);
	MandatoryPage() { auto *layout = new QVBoxLayout(this); layout->addWidget(edit); edit->setObjectName("mandatory"); registerField("value*", edit); }
};

class TabDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	int lastKey = 0;
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::KeyPress) lastKey = static_cast<QKeyEvent *>(event)->key();
		return QStyledItemDelegate::eventFilter(object, event);
	}
};

class DropDialog : public QDialog {
public:
	int received = 0;
	QList<QUrl> urls;
	DropDialog() { setAcceptDrops(true); }
	void dragEnterEvent(QDragEnterEvent *event) override { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
	void dropEvent(QDropEvent *event) override { ++received; urls = event->mimeData()->urls(); }
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	int checks = 0, failed = 0;
	const auto check = [&](bool result, const char *label) {
		++checks; failed += !result;
		std::cout << (result ? "PASS: " : "FAIL: ") << label << std::endl;
	};
	QDialog dialog; dialog.resize(700, 850);
	auto *layout = new QVBoxLayout(&dialog);
	auto *browser = new QTextBrowser; browser->setObjectName("releaseNotes"); browser->setMaximumHeight(70);
	browser->setHtml("<a href='https://example.invalid/details'>Release notes link</a>"); browser->setOpenLinks(false); layout->addWidget(browser);
	auto *date = new QDateTimeEdit(QDateTime(QDate(2026, 9, 28), QTime(12, 0))); date->setObjectName("scheduledTime");
	date->setCalendarPopup(true); layout->addWidget(date);
	auto *label = new ClickableLabel; label->setObjectName("thumbnailPreview"); label->setText("Select thumbnail"); layout->addWidget(label);
	auto *spin = new OBS::SpinBox; spin->setObjectName("transformPosition"); spin->setRange(-10000, 10000); layout->addWidget(spin);
	auto *real = new OBS::DoubleSpinBox; real->setObjectName("transformRotation"); layout->addWidget(real);
	auto *combo = new QComboBox; combo->setObjectName("validatedCombo"); combo->setEditable(true);
	combo->addItems({"10", "20"}); combo->lineEdit()->setValidator(new QIntValidator(1, 99, combo)); layout->addWidget(combo);
	auto *list = new QListWidget; list->setObjectName("triStateModel"); list->setMaximumHeight(60); layout->addWidget(list);
	auto *item = new QListWidgetItem("Optional state", list);
	item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsUserTristate); item->setCheckState(Qt::Unchecked);
	auto *table = new QTableView; table->setObjectName("delegateTable"); layout->addWidget(table);
	QStandardItemModel model(1, 2); model.setData(model.index(0, 0), "old name"); model.setData(model.index(0, 1), "old URL"); table->setModel(&model);
	auto *delegate = new TabDelegate(table); table->setItemDelegate(delegate);
	dialog.show(); app.processEvents();
	OBSWeb::QtDialogBridge bridge(&dialog);
	const auto node = [&](const QString &name) { return named(bridge.snapshot(), name); };
	QString error;
	const auto run = [&](const char *command, QJsonObject args) { return bridge.execute(QString::fromLatin1(command), args, error); };
	check(node("releaseNotes").value("type") == "native", "rich text browsers retain native links instead of becoming plain textareas");
	int opened = 0; QObject::connect(browser, &QTextBrowser::anchorClicked, [&] { ++opened; });
	QPoint linkPoint;
	for (int y = 0; y < browser->viewport()->height() && linkPoint.isNull(); ++y)
		for (int x = 0; x < browser->viewport()->width(); ++x)
			if (!browser->anchorAt({x, y}).isEmpty()) { linkPoint = {x, y}; break; }
	QMouseEvent press(QEvent::MouseButtonPress, linkPoint, browser->viewport()->mapToGlobal(linkPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, linkPoint, browser->viewport()->mapToGlobal(linkPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(browser->viewport(), &press); QApplication::sendEvent(browser->viewport(), &release);
	check(opened == 1, "the original QTextBrowser still emits its link callback without navigating an external site");
	check(node("scheduledTime").value("type") == "native", "date/time editor preserves its complete calendar and spin controls");
	date->setCurrentSection(QDateTimeEdit::DaySection);
	QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier); QApplication::sendEvent(date, &up);
	check(date->date() == QDate(2026, 9, 29), "native date section stepping still changes the scheduled date");
	check(node("thumbnailPreview").value("type") == "native", "ClickableLabel remains clickable even without anchor markup");
	int clicks = 0; QObject::connect(label, &ClickableLabel::clicked, [&] { ++clicks; });
	QMouseEvent labelPress(QEvent::MouseButtonPress, QPointF(4, 4), label->mapToGlobal(QPoint(4, 4)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(label, &labelPress); check(clicks == 1, "native thumbnail/time label executes its original clicked callback");
	check(node("transformPosition").value("type") == "native" && node("transformRotation").value("type") == "native",
	      "custom OBS spinboxes preserve their own keyboard and commit semantics");
	spin->setValue(10); auto *spinEdit = spin->findChild<QLineEdit *>(); spinEdit->selectAll(); spinEdit->insert("42");
	check(spin->value() == 10, "OBS spinbox keeps keyboardTracking=false while typing");
	QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(spin, &enter);
	check(spin->value() == 42 && dialog.isVisible(), "OBS spinbox Return commits without accepting its dialog");
	const auto comboId = node("validatedCombo").value("id");
	check(!run("dialog.input", {{"id", comboId}, {"value", "invalid"}}) && combo->currentText() == "10",
	      "editable combobox rejects text rejected by its native validator");
	combo->setEditText("10");
	check(run("dialog.input", {{"id", comboId}, {"value", "42"}}) && combo->currentText() == "42" && combo->lineEdit()->isUndoAvailable(),
	      "editable combobox uses native user editing and preserves undo history");
	combo->lineEdit()->undo(); check(combo->currentText() == "10", "editable combobox undo restores the prior value");
	combo->lineEdit()->setReadOnly(true);
	check(!run("dialog.input", {{"id", comboId}, {"value", "33"}}) && combo->currentText() == "10", "combobox editor read-only state cannot be bypassed");
	combo->lineEdit()->setReadOnly(false);
	int activated = 0; QObject::connect(combo, &QComboBox::activated, [&](int) { ++activated; });
	run("dialog.input", {{"id", comboId}, {"value", "31"}});
	check(run("dialog.key", {{"id", comboId}, {"key", "Enter"}}) && combo->findText("31") >= 0 && activated > 0,
	      "editable combobox Return reaches its editor and native insertion policy");
	list->setCurrentRow(0);
	QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " "); QApplication::sendEvent(list, &space);
	const auto nativeState = item->checkState(); item->setCheckState(Qt::Unchecked);
	const auto listNode = node("triStateModel"); const auto itemId = listNode.value("items").toArray().at(0).toObject().value("id");
	check(run("dialog.item", {{"id", listNode.value("id")}, {"item", itemId}, {"action", "toggle"}}) && item->checkState() == nativeState && nativeState == Qt::PartiallyChecked,
	      "user-tristate item toggles follow the same cycle as native Qt Space");
	list->addItem("Search target");
	check(run("dialog.key", {{"id", listNode.value("id")}, {"key", "s"}}) && list->currentRow() == 1,
	      "printable item-view key invokes native keyboardSearch and selects its matching row");
	table->setCurrentIndex(model.index(0, 0)); table->edit(model.index(0, 0)); app.processEvents();
	QJsonObject editor;
	for (const auto entry : bridge.snapshot().value("nodes").toArray())
		if (entry.toObject().value("itemView") == node("delegateTable").value("id") && entry.toObject().value("type") == "text") editor = entry.toObject();
	check(!editor.isEmpty(), "native model editor is available to the HTML bridge");
	run("dialog.input", {{"id", editor.value("id")}, {"value", "new name"}});
	check(run("dialog.key", {{"id", editor.value("id")}, {"key", "Tab"}}), "delegate Tab is supported by the bridge");
	app.processEvents();
	check(delegate->lastKey == Qt::Key_Tab && model.index(0, 0).data().toString() == "new name" && table->currentIndex().column() == 1,
	      "delegate Tab commits the value and advances the native table cell");
	QJsonObject blurEditor;
	for (const auto entry : bridge.snapshot().value("nodes").toArray())
		if (entry.toObject().value("itemView") == node("delegateTable").value("id") && entry.toObject().value("type") == "text") blurEditor = entry.toObject();
	run("dialog.input", {{"id", blurEditor.value("id")}, {"value", "committed on blur"}});
	run("dialog.finish", {{"id", blurEditor.value("id")}}); app.processEvents();
	check(model.index(0, 1).data().toString() == "committed on blur", "HTML editor blur commits the standard native item delegate");
	dialog.hide();
	QWizard wizard; wizard.setWizardStyle(QWizard::ModernStyle);
	auto *first = new MandatoryPage; wizard.addPage(first); wizard.addPage(new QWizardPage);
	wizard.button(QWizard::NextButton)->setObjectName("wizardNext"); wizard.button(QWizard::BackButton)->setObjectName("wizardBack");
	wizard.show(); app.processEvents(); OBSWeb::QtDialogBridge wizardBridge(&wizard);
	check(!wizard.button(QWizard::NextButton)->isEnabled(), "native wizard starts with mandatory-field validation");
	check(wizardBridge.execute("dialog.input", {{"id", named(wizardBridge.snapshot(), "mandatory").value("id")}, {"value", "value"}}, error) && wizard.button(QWizard::NextButton)->isEnabled(),
	      "editing a wizard field executes native completeness tracking");
	wizardBridge.execute("dialog.click", {{"id", named(wizardBridge.snapshot(), "wizardNext").value("id")}}, error);
	app.processEvents();
	check(wizard.currentId() == 1, "wizard Next invokes the native page controller");
	wizardBridge.execute("dialog.click", {{"id", named(wizardBridge.snapshot(), "wizardBack").value("id")}}, error);
	app.processEvents();
	check(wizard.currentId() == 0 && first->edit->text() == "value", "wizard Back retains native page state");
	check(wizardBridge.execute("dialog.key", {{"key", "Escape"}}, error) && !wizard.isVisible() && wizard.result() == QDialog::Rejected,
	      "wizard Cancel retains its native rejection path");
	DropDialog drops; drops.show(); app.processEvents(); OBSWeb::QtDialogBridge dropBridge(&drops);
	QTemporaryFile file; if (!file.open()) return 2;
	const auto data = OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}}, {file.fileName()}, error);
	check(data && dropBridge.snapshot().value("acceptDrops").toBool() && dropBridge.drop(*data, error) &&
	      drops.received == 1 && drops.urls == data->urls, "trusted external files reach the original accepting dialog handler");
	drops.setAcceptDrops(false);
	check(!dropBridge.snapshot().value("acceptDrops").toBool() && data && !dropBridge.drop(*data, error) && drops.received == 1,
	      "a running remux can disable drops through its existing Qt acceptDrops state");
	drops.setAcceptDrops(true); drops.setEnabled(false);
	check(data && !dropBridge.drop(*data, error) && drops.received == 1, "disabled dialog cannot import an external drop");
	drops.setEnabled(true);
	QDialog nested; nested.setWindowModality(Qt::ApplicationModal); nested.show(); app.processEvents();
	check(data && !dropBridge.drop(*data, error) && drops.received == 1, "a pending modal confirmation blocks drops to the covered dialog");
	nested.hide(); drops.hide();
	check(data && !dropBridge.drop(*data, error) && drops.received == 1, "closed dialog cannot import an external drop");
	QDialog styledDialog;
	QFontDatabase::addApplicationFont(QString::fromUtf8(OBS_DIALOG_TEST_FONT));
	styledDialog.setFont(QFont(QStringLiteral("Open Sans"), 10));
	auto *styledLayout = new QVBoxLayout(&styledDialog);
	auto *styledEdit = new QLineEdit("10"); styledEdit->setObjectName("styledEdit");
	styledEdit->setReadOnly(true); styledEdit->setAlignment(Qt::AlignCenter); styledEdit->setFixedSize(180, 34);
	styledEdit->setStyleSheet("QLineEdit { background: #272a33; color: white; border: 1px solid #454956; border-radius: 4px; padding: 3px 9px; }"
		"QLineEdit:disabled { background: #31343d; }");
	styledLayout->addWidget(styledEdit); styledDialog.show(); app.processEvents();
	OBSWeb::QtDialogBridge styledBridge(&styledDialog);
	const auto styledNode = named(styledBridge.snapshot(), "styledEdit");
	const auto pixels = [](const QJsonObject &node) {
		return QImage::fromData(QByteArray::fromBase64(node.value("decoration").toString().section(',', 1).toLatin1()));
	};
	const auto styledImage = pixels(styledNode);
	check(!styledImage.isNull() && styledImage.pixelColor(styledImage.width() / 2, styledImage.height() / 2) == QColor("#272a33"),
	      "line-edit native decoration paints its stylesheet background without rasterizing its text");
	check((styledNode.value("alignment").toInt() & Qt::AlignHCenter) && styledNode.value("textRect").toObject().value("x").toInt() >= 10,
	      "line-edit snapshot preserves native center alignment and stylesheet text insets");
	styledEdit->setEnabled(false);
	const auto disabledNode = named(styledBridge.snapshot(), "styledEdit");
	const auto disabledImage = pixels(disabledNode);
	check(!disabledImage.isNull() && disabledImage.pixelColor(disabledImage.width() / 2, disabledImage.height() / 2) == QColor("#31343d") &&
	      disabledNode.value("decoration") != styledNode.value("decoration"), "line-edit raster cache responds to native disabled stylesheet state");
	auto *logo = new QLabel; logo->setObjectName("largeLogo"); logo->setFixedSize(256, 256); logo->setScaledContents(true);
	QPixmap logoPixels(32, 32); logoPixels.fill(QColor("#3578ac")); logo->setPixmap(logoPixels); styledLayout->addWidget(logo);
	auto *emptyCombo = new QComboBox; emptyCombo->setObjectName("placeholderCombo"); emptyCombo->addItems({"First", "Second"});
	emptyCombo->setPlaceholderText("None"); emptyCombo->setCurrentIndex(-1); styledLayout->addWidget(emptyCombo);
	auto *headerTable = new QTableView; headerTable->setObjectName("alignedHeader"); headerTable->setFixedHeight(75);
	auto *headerModel = new QStandardItemModel(1, 2, headerTable); headerTable->setModel(headerModel);
	headerModel->setHeaderData(0, Qt::Horizontal, Qt::AlignRight, Qt::TextAlignmentRole);
	headerTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter); styledLayout->addWidget(headerTable);
	auto *fittedLabel = new QLabel(QString::fromUtf8("Разрешение предпросмотра (холст)")); fittedLabel->setObjectName("nativeWidthLabel");
	fittedLabel->setFixedSize(225, 31); styledLayout->addWidget(fittedLabel);
	auto *fittedCombo = new QComboBox; fittedCombo->setObjectName("nativeWidthCombo");
	fittedCombo->addItem(QString::fromUtf8("Использовать текущее (1920x1080)")); fittedCombo->setFixedSize(261, 36); styledLayout->addWidget(fittedCombo);
	styledDialog.adjustSize(); app.processEvents();
	const auto presentation = styledBridge.snapshot(); const auto logoNode = named(presentation, "largeLogo");
	const auto logoImage = QImage::fromData(QByteArray::fromBase64(logoNode.value("icon").toString().section(',', 1).toLatin1()));
	check(logoNode.value("iconRect").toObject().value("width").toInt() == 256 && !logoImage.isNull() && logoImage.width() >= 256 &&
	      logoImage.pixelColor(logoImage.width() / 2, logoImage.height() / 2) == QColor("#3578ac"),
	      "QLabel pixmap preserves its scaled native size instead of becoming a small button icon");
	const auto emptyComboNode = named(presentation, "placeholderCombo");
	check(emptyComboNode.value("index").toInt() == -1 && emptyComboNode.value("placeholder") == "None" &&
	      emptyComboNode.value("choices").toArray().size() == 2, "empty combo preserves its native placeholder without changing model choices");
	const auto columns = named(presentation, "alignedHeader").value("columns").toArray();
	check(columns.size() == 2 && (columns[0].toObject().value("alignment").toInt() & Qt::AlignRight) &&
	      (columns[1].toObject().value("alignment").toInt() & Qt::AlignHCenter), "header alignment respects model override and native default");
	const auto labelMetrics = named(presentation, "nativeWidthLabel"), comboMetrics = named(presentation, "nativeWidthCombo");
	check(qAbs(labelMetrics.value("nativeTextWidth").toDouble() - QFontMetricsF(fittedLabel->font()).horizontalAdvance(fittedLabel->text())) < .01 &&
	      labelMetrics.value("textRect").toObject().value("width").toInt() == fittedLabel->contentsRect().width(),
	      "single-line label exposes actual Qt text advance and available width for bounded browser fitting");
	check(qAbs(comboMetrics.value("nativeTextWidth").toDouble() - QFontMetricsF(fittedCombo->font()).horizontalAdvance(fittedCombo->currentText())) < .01 &&
	      comboMetrics.value("textRect").toObject().value("width").toInt() < fittedCombo->width(),
	      "combo caption uses the native style edit-field rectangle and actual Qt glyph advance");
	const auto snapshotPath = qEnvironmentVariable("OBS_WEBVIEW2_TEST_STYLE_SNAPSHOT");
	if (!snapshotPath.isEmpty()) { QFile exported(snapshotPath); if (exported.open(QIODevice::WriteOnly)) exported.write(QJsonDocument(presentation).toJson()); }
	std::cout << "RESULT " << checks << " checks, " << failed << " failures\n";
	return failed ? 1 : 0;
}
