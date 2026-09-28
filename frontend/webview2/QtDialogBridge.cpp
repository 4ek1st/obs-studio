#include "QtDialogBridge.hpp"
#include "WebView2Widget.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QCache>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFocusEvent>
#include <QFontDialog>
#include <QFontInfo>
#include <QLibrary>
#include <Windows.h>
#include <dwmapi.h>
#include <QFontMetricsF>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStyleOptionFrame>
#include <QStyleOptionGroupBox>
#include <QStyleOptionSlider>
#include <QTabBar>
#include <QTableView>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr auto surfaceProperty = "_obsWebView2DialogSurface";
constexpr auto installedProperty = "_obsWebView2DialogInstaller";
constexpr auto externalSurfaceProperty = "_obsWebView2ExternalSurface";

QString plainText(const QString &text)
{
	if (!Qt::mightBeRichText(text))
		return text;
	QTextDocument document;
	document.setHtml(text);
	return document.toPlainText();
}

QString buttonText(QString text)
{
	text.replace(QStringLiteral("&&"), QString(QChar(0xf000)));
	text.remove(QLatin1Char('&'));
	text.replace(QChar(0xf000), QLatin1Char('&'));
	return text;
}

QJsonObject rectangle(const QRect &rect)
{
	return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

QJsonObject fontState(const QWidget *widget)
{
	const auto requested = widget->font();
	const QFontInfo resolved(requested);
	const int pixels = requested.pixelSize() > 0 ? requested.pixelSize() : resolved.pixelSize() > 0 ? resolved.pixelSize() :
		std::max(1, qRound(requested.pointSizeF() * widget->logicalDpiY() / 72.0));
	return {{"family", resolved.family().isEmpty() ? requested.family() : resolved.family()}, {"pixelSize", pixels},
		{"weight", int(requested.weight())}, {"italic", requested.italic()},
		{"lineHeight", QFontMetricsF(requested).lineSpacing()}};
}

QJsonObject paletteState(const QWidget *widget)
{
	const auto palette = widget->palette();
	const auto group = !widget->isEnabled() ? QPalette::Disabled : widget->isActiveWindow() ? QPalette::Active : QPalette::Inactive;
	return {{"window", palette.color(group, QPalette::Window).name()}, {"windowText", palette.color(group, QPalette::WindowText).name()},
		{"base", palette.color(group, QPalette::Base).name()}, {"text", palette.color(group, QPalette::Text).name()},
		{"button", palette.color(group, QPalette::Button).name()}, {"buttonText", palette.color(group, QPalette::ButtonText).name()},
		{"mid", palette.color(group, QPalette::Mid).name()}, {"highlight", palette.color(group, QPalette::Highlight).name()},
		{"highlightedText", palette.color(group, QPalette::HighlightedText).name()}};
}

void choiceGeometry(QAbstractButton *button, bool radio, QJsonObject &data)
{
	QStyleOptionButton option;
	option.initFrom(button);
	option.text = button->text();
	option.icon = button->icon();
	option.iconSize = button->iconSize();
	option.state |= button->isChecked() ? QStyle::State_On : QStyle::State_Off;
	if (auto *check = qobject_cast<QCheckBox *>(button); check && check->checkState() == Qt::PartiallyChecked) {
		option.state &= ~(QStyle::State_On | QStyle::State_Off);
		option.state |= QStyle::State_NoChange;
	}
	if (button->isDown()) option.state |= QStyle::State_Sunken;
	data.insert("indicatorRect", rectangle(button->style()->subElementRect(
		radio ? QStyle::SE_RadioButtonIndicator : QStyle::SE_CheckBoxIndicator, &option, button)));
	data.insert("textRect", rectangle(button->style()->subElementRect(
		radio ? QStyle::SE_RadioButtonContents : QStyle::SE_CheckBoxContents, &option, button)));
	data.insert("nativeTextWidth", QFontMetricsF(button->font()).horizontalAdvance(buttonText(button->text())));
}

QRect visibleRectangle(QWidget *widget, QWidget *dialog)
{
	if (widget == dialog) return dialog->rect();
	QRect visible(widget->mapTo(dialog, QPoint()), widget->size());
	for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
		visible &= QRect(parent->mapTo(dialog, QPoint()), parent->size());
		if (parent == dialog)
			break;
	}
	return visible & dialog->rect();
}

Qt::KeyboardModifiers modifiers(const QJsonObject &args)
{
	Qt::KeyboardModifiers result = Qt::NoModifier;
	if (args.value("ctrl").toBool()) result |= Qt::ControlModifier;
	if (args.value("shift").toBool()) result |= Qt::ShiftModifier;
	if (args.value("alt").toBool()) result |= Qt::AltModifier;
	return result;
}

void mouseClick(QWidget *widget, QPoint point, Qt::KeyboardModifiers mods, bool twice = false,
		Qt::MouseButton button = Qt::LeftButton)
{
	const QPointer<QWidget> guard(widget);
	const auto global = widget->mapToGlobal(point);
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), button, button, mods);
	QApplication::sendEvent(widget, &press);
	if (!guard) return;
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), button, Qt::NoButton, mods);
	QApplication::sendEvent(widget, &release);
	if (!guard || !twice) return;
	QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(point), QPointF(global), button, button, mods);
	QApplication::sendEvent(widget, &doubleClick);
	if (guard) QApplication::sendEvent(widget, &release);
}

QAbstractItemView *owningView(QWidget *widget)
{
	for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
		if (auto *view = qobject_cast<QAbstractItemView *>(parent)) return view;
	return nullptr;
}

bool isStandardContainer(QWidget *widget)
{
	const QByteArray type(widget->metaObject()->className());
	return type == "QWidget" || type == "QFrame" || type == "QStackedWidget" || type == "QTabWidget" ||
	       type == "QSplitter" || type == "QDialogButtonBox" || type == "QWizardPage" ||
	       type == "QScrollArea" || type == "QScrollAreaWidget" || type == "QSizeGrip";
}
} // namespace

namespace OBSWeb {
struct QtDialogBridge::Impl {
	QPointer<QWidget> dialog;
	quint64 sequence = 0;
	QHash<QString, QPointer<QWidget>> widgets;
	QHash<QWidget *, QString> widgetIds;
	QHash<QString, QPersistentModelIndex> indexes;
	QHash<QPersistentModelIndex, QString> indexIds;
	QHash<qint64, QString> icons;
	struct Raster { QSize size; qreal dpr; QByteArray fingerprint; QString uri; };
	// Compare freshly styled pixels, then reuse PNG encoding. This also catches
	// inherited QSS and dynamic-property changes without guessing a style cache key.
	// Keep fingerprints rather than raw bitmaps: several nested Settings panels
	// otherwise exhaust the cache and re-encode every image on every poll.
	QCache<QString, Raster> rasters{8 * 1024 * 1024};

	template<typename Paint> QString raster(QWidget *widget, const QString &part, Paint paint, QRect paintRect = {})
	{
		if (paintRect.isEmpty()) paintRect = widget->rect();
		const auto dpr = widget->devicePixelRatioF();
		QImage pixels(paintRect.size() * dpr, QImage::Format_ARGB32_Premultiplied);
		if (pixels.isNull()) return {};
		pixels.setDevicePixelRatio(dpr);
		pixels.fill(Qt::transparent);
		{ QPainter painter(&pixels); painter.translate(-paintRect.topLeft()); paint(painter); }
		const auto fingerprint = QCryptographicHash::hash(
			QByteArrayView(reinterpret_cast<const char *>(pixels.constBits()), pixels.sizeInBytes()), QCryptographicHash::Sha256);
		const auto key = identify(widget) + QLatin1Char(':') + part;
		if (auto *cached = rasters.object(key); cached && cached->size == pixels.size() && cached->dpr == dpr &&
		    cached->fingerprint == fingerprint) return cached->uri;
		QByteArray bytes;
		QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); pixels.save(&buffer, "PNG");
		const auto uri = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
		rasters.insert(key, new Raster{pixels.size(), dpr, fingerprint, uri}, sizeof(Raster) + fingerprint.size() + uri.size() * sizeof(QChar));
		return uri;
	}

	QJsonObject scrollbarStyle(QScrollBar *scroll)
	{
		QStyleOptionSlider option;
		option.initFrom(scroll);
		option.orientation = scroll->orientation();
		option.minimum = scroll->minimum(); option.maximum = scroll->maximum();
		option.sliderPosition = scroll->sliderPosition(); option.sliderValue = scroll->value();
		option.singleStep = scroll->singleStep(); option.pageStep = scroll->pageStep();
		option.upsideDown = scroll->invertedAppearance();
		option.subControls = QStyle::SC_All;
		option.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_Horizontal);
		if (scroll->orientation() == Qt::Horizontal) option.state |= QStyle::State_Horizontal;
		auto *style = scroll->style();
		auto rect = [&](QStyle::SubControl control) { return style->subControlRect(QStyle::CC_ScrollBar, &option, control, scroll); };
		QJsonObject data{{"thumbRect", rectangle(rect(QStyle::SC_ScrollBarSlider))},
			{"grooveRect", rectangle(rect(QStyle::SC_ScrollBarGroove))},
			{"subLineRect", rectangle(rect(QStyle::SC_ScrollBarSubLine))},
			{"addLineRect", rectangle(rect(QStyle::SC_ScrollBarAddLine))}, {"value", scroll->value()}};
		const int savedPosition = option.sliderPosition;
		option.sliderPosition = option.minimum;
		const auto first = rect(QStyle::SC_ScrollBarSlider);
		option.sliderPosition = option.maximum;
		const auto last = rect(QStyle::SC_ScrollBarSlider);
		option.sliderPosition = savedPosition;
		data.insert("reversed", scroll->orientation() == Qt::Vertical ? last.y() < first.y() : last.x() < first.x());
		for (const auto *state : {"normal", "hover", "pressed"}) {
			auto painted = option;
			painted.activeSubControls = QStyle::SC_None;
			if (strcmp(state, "normal") != 0 && scroll->isEnabled()) {
				painted.state |= QStyle::State_MouseOver;
				painted.activeSubControls = QStyle::SC_ScrollBarSlider;
				if (strcmp(state, "pressed") == 0) painted.state |= QStyle::State_Sunken;
			}
			data.insert(QString::fromLatin1(state), raster(scroll, QString::fromLatin1(state), [&](QPainter &painter) {
				style->drawComplexControl(QStyle::CC_ScrollBar, &painted, &painter, scroll);
			}));
		}
		return data;
	}

	QJsonObject scrollState(QScrollBar *scroll, QWidget *container = nullptr)
	{
		QJsonObject data{{"minimum", scroll->minimum()}, {"maximum", scroll->maximum()}, {"value", scroll->value()},
			{"page", scroll->pageStep()}, {"step", scroll->singleStep()}};
		if (scroll->isVisible()) data.insert("nativeStyle", scrollbarStyle(scroll));
		if (container) data.insert("rect", rectangle(QRect(scroll->mapTo(container, QPoint()), scroll->size())));
		return data;
	}

	void widgetDecoration(QWidget *widget, QJsonObject &data)
	{
		auto *frame = qobject_cast<QFrame *>(widget);
		const bool background = widget->autoFillBackground() || widget->testAttribute(Qt::WA_StyledBackground);
		if (!background && (!frame || !frame->frameWidth())) return;
		if (background) data.insert("background", widget->palette().color(widget->backgroundRole()).name());
		data.insert("frameWidth", frame ? frame->frameWidth() : 0);
		// Paint using the widget's original geometry so native gradients, borders,
		// padding and QSS margins stay exact; transmit only the portion in view.
		const auto paintRect = visibleRectangle(widget, dialog).translated(-widget->mapTo(dialog, QPoint()));
		data.insert("decorationRect", rectangle(paintRect));
		data.insert("decoration", raster(widget, QStringLiteral("decoration"), [&](QPainter &painter) {
			// QSS paints its own background inside its margin/border box. Filling
			// the whole widget here would incorrectly cover transparent margins.
			if (widget->autoFillBackground()) painter.fillRect(widget->rect(), widget->palette().brush(widget->backgroundRole()));
			if (!frame || widget->testAttribute(Qt::WA_StyledBackground)) {
				QStyleOption option; option.initFrom(widget);
				widget->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, widget);
			}
			if (!frame) return;
			QStyleOptionFrame option;
			option.initFrom(frame); option.rect = frame->frameRect();
			option.lineWidth = frame->lineWidth(); option.midLineWidth = frame->midLineWidth();
			option.frameShape = frame->frameShape();
			if (frame->frameShadow() == QFrame::Sunken) option.state |= QStyle::State_Sunken;
			else if (frame->frameShadow() == QFrame::Raised) option.state |= QStyle::State_Raised;
			frame->style()->drawControl(QStyle::CE_ShapedFrame, &option, &painter, frame);
		}, paintRect));
	}

	void lineEditDecoration(QLineEdit *edit, QJsonObject &data)
	{
		// QSS can leave Base black even when PE_PanelLineEdit paints a different
		// background. Paint only the panel: field contents stay editable HTML and
		// password text is never included in a native raster.
		QStyleOptionFrame option;
		option.initFrom(edit); option.rect = edit->contentsRect();
		option.lineWidth = edit->hasFrame() ? edit->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, edit) : 0;
		option.midLineWidth = 0; option.state |= QStyle::State_Sunken;
		if (edit->isReadOnly()) option.state |= QStyle::State_ReadOnly;
		auto contents = edit->style()->subElementRect(QStyle::SE_LineEditContents, &option, edit);
		const auto margins = edit->textMargins();
		// QLineEdit adds two logical horizontal pixels inside the style's content
		// rectangle, in addition to application-provided text margins.
		contents.adjust(margins.left() + 2, margins.top(), -margins.right() - 2, -margins.bottom());
		data.insert("textRect", rectangle(contents));
		data.insert("alignment", int(QStyle::visualAlignment(edit->layoutDirection(), edit->alignment())));
		const auto paintRect = visibleRectangle(edit, dialog).translated(-edit->mapTo(dialog, QPoint()));
		data.insert("decorationRect", rectangle(paintRect));
		data.insert("decoration", raster(edit, QStringLiteral("lineEdit"), [&](QPainter &painter) {
			edit->style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &painter, edit);
		}, paintRect));
	}

	QString identify(QWidget *widget)
	{
		const auto previous = widgetIds.value(widget);
		if (!previous.isEmpty() && widgets.value(previous) == widget) return previous;
		const auto id = QString::number(++sequence);
		widgetIds.insert(widget, id);
		widgets.insert(id, widget);
		return id;
	}

	QString identify(const QModelIndex &index)
	{
		const QPersistentModelIndex persistent(index);
		const auto existing = indexIds.value(persistent);
		if (!existing.isEmpty()) return existing;
		const auto id = QStringLiteral("i%1").arg(++sequence);
		indexes.insert(id, persistent);
		indexIds.insert(persistent, id);
		return id;
	}

	QString icon(const QIcon &source)
	{
		if (source.isNull()) return {};
		const auto key = source.cacheKey();
		if (icons.contains(key)) return icons.value(key);
		QByteArray bytes;
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		source.pixmap(24, 24).save(&buffer, "PNG");
		const auto result = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
		if (icons.size() > 512) icons.clear();
		icons.insert(key, result);
		return result;
	}

	void prune()
	{
		for (auto it = widgets.begin(); it != widgets.end();) {
			if (it.value().isNull()) it = widgets.erase(it); else ++it;
		}
		for (auto it = widgetIds.begin(); it != widgetIds.end();) {
			if (!widgets.contains(it.value())) it = widgetIds.erase(it); else ++it;
		}
		// QPersistentModelIndex's hash changes after model insertions. Rebuild its reverse map.
		indexIds.clear();
		for (auto it = indexes.begin(); it != indexes.end();) {
			if (!it.value().isValid()) it = indexes.erase(it);
			else { indexIds.insert(it.value(), it.key()); ++it; }
		}
	}

	QJsonObject item(QAbstractItemView *view, const QModelIndex &index)
	{
		const auto rect = view->visualRect(index);
		const auto flags = index.flags();
		QJsonObject data{{"id", identify(index)}, {"row", index.row()}, {"column", index.column()},
				 {"text", index.data(Qt::DisplayRole).toString()}, {"rect", rectangle(rect)},
				 {"enabled", bool(flags & Qt::ItemIsEnabled)}, {"selectable", bool(flags & Qt::ItemIsSelectable)},
				 {"editable", bool(flags & Qt::ItemIsEditable)}, {"checkable", bool(flags & Qt::ItemIsUserCheckable) && index.data(Qt::CheckStateRole).isValid()},
				 {"checked", index.data(Qt::CheckStateRole).toInt()},
				 {"selected", view->selectionModel() && view->selectionModel()->isSelected(index)},
				 {"current", view->currentIndex() == index}, {"tooltip", index.data(Qt::ToolTipRole).toString()}};
		data.insert("icon", icon(qvariant_cast<QIcon>(index.data(Qt::DecorationRole))));
		if (auto *tree = qobject_cast<QTreeView *>(view)) {
			data.insert("children", view->model()->hasChildren(index));
			data.insert("expanded", tree->isExpanded(index));
		}
		return data;
	}

	void itemView(QAbstractItemView *view, QJsonObject &data)
	{
		data.insert("type", "items");
		data.insert("viewport", rectangle(QRect(view->viewport()->mapTo(view, QPoint()), view->viewport()->size())));
		data.insert("verticalScroll", scrollState(view->verticalScrollBar(), view));
		data.insert("horizontalScroll", scrollState(view->horizontalScrollBar(), view));
		data.insert("selectionMode", int(view->selectionMode()));
		QJsonArray items;
		const auto bounds = view->viewport()->rect();
		auto *model = view->model();
		if (!model) { data.insert("items", items); return; }
		QHeaderView *header = nullptr;
		if (auto *tree = qobject_cast<QTreeView *>(view)) {
			header = tree->header();
			auto index = tree->indexAt(QPoint(2, 1));
			if (!index.isValid()) index = model->index(0, 0, view->rootIndex());
			for (int count = 0; index.isValid() && count < 512; ++count, index = tree->indexBelow(index)) {
				const auto rect = tree->visualRect(index);
				if (rect.top() > bounds.bottom()) break;
				for (int col = 0; col < model->columnCount(index.parent()); ++col) {
					const auto cell = index.siblingAtColumn(col);
					if (!tree->isColumnHidden(col) && tree->visualRect(cell).intersects(bounds)) items.append(item(view, cell));
				}
			}
		} else if (auto *table = qobject_cast<QTableView *>(view)) {
			header = table->horizontalHeader();
			int first = table->rowAt(0);
			if (first < 0) first = 0;
			for (int row = first, count = 0; row < model->rowCount(view->rootIndex()) && count < 512; ++row, ++count) {
				if (table->isRowHidden(row)) continue;
				if (table->rowViewportPosition(row) > bounds.bottom()) break;
				for (int col = 0; col < model->columnCount(view->rootIndex()); ++col) {
					const auto index = model->index(row, col, view->rootIndex());
					if (!table->isColumnHidden(col) && table->visualRect(index).intersects(bounds)) items.append(item(view, index));
				}
			}
		} else {
			const auto firstIndex = view->indexAt(QPoint(2, 1));
			const int first = firstIndex.isValid() ? std::max(0, firstIndex.row() - 1) : 0;
			for (int row = first, count = 0; row < model->rowCount(view->rootIndex()) && count < 4096; ++row, ++count) {
				const auto index = model->index(row, 0, view->rootIndex());
				const auto rect = view->visualRect(index);
				if (rect.intersects(bounds)) items.append(item(view, index));
				if (rect.top() > bounds.bottom() && !qobject_cast<QListView *>(view)->isWrapping()) break;
			}
		}
		data.insert("items", items);
		QJsonArray columns;
		if (header && header->isVisible()) {
			data.insert("headerRect", rectangle(QRect(header->mapTo(view, QPoint()), header->size())));
			for (int col = 0; col < header->count(); ++col) {
				if (header->isSectionHidden(col)) continue;
				const auto alignment = model->headerData(col, Qt::Horizontal, Qt::TextAlignmentRole);
				columns.append(QJsonObject{{"index", col}, {"text", model->headerData(col, Qt::Horizontal).toString()},
					{"alignment", alignment.isValid() ? alignment.toInt() : int(header->defaultAlignment())},
					{"x", header->sectionViewportPosition(col)}, {"width", header->sectionSize(col)}});
			}
		}
		data.insert("columns", columns);
	}

	void collect(QWidget *parent, QJsonArray &nodes, bool includeParent = false)
	{
		const auto candidates = includeParent ? QList<QWidget *>{parent} : parent->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (auto *widget : candidates) {
			if ((widget != dialog && widget->isWindow()) || widget->property(surfaceProperty).toBool() || !widget->isVisibleTo(dialog)) continue;
			const auto clipped = visibleRectangle(widget, dialog);
			if (clipped.isEmpty()) continue;
			QJsonObject data{{"id", identify(widget)}, {"name", widget->objectName()},
				{"class", QString::fromLatin1(widget->metaObject()->className())},
				{"rect", rectangle(QRect(widget->mapTo(dialog, QPoint()), widget->size()))},
				{"clip", rectangle(clipped)}, {"enabled", widget->isEnabled()},
				{"tooltip", plainText(widget->toolTip())}, {"accessibleName", widget->accessibleName()},
				{"font", fontState(widget)}, {"palette", paletteState(widget)}};
			bool atomic = true;
			if (auto *view = owningView(widget)) data.insert("itemView", identify(view));
			if (widget->inherits("SourceSelectButton") || widget->inherits("OBSHotkeyLabel") || widget->inherits("BalanceSlider") ||
			    widget->inherits("AbsoluteSlider") || widget->inherits("ClickableLabel") ||
			    widget->inherits("OBS::SpinBox") || widget->inherits("OBS::DoubleSpinBox") ||
			    qobject_cast<QTextBrowser *>(widget) ||
			    (qobject_cast<QAbstractSpinBox *>(widget) && !qobject_cast<QSpinBox *>(widget) && !qobject_cast<QDoubleSpinBox *>(widget))) {
				// These controls have custom paint/drag/click contracts beyond their Qt base class.
				data.insert("type", "native");
			} else if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
				const bool check = qobject_cast<QCheckBox *>(button), radio = qobject_cast<QRadioButton *>(button);
				if (check || radio) choiceGeometry(button, radio, data);
				data.insert("type", check ? "check" : radio ? "radio" : "button");
				data.insert("text", buttonText(button->text()));
				data.insert("icon", icon(button->icon()));
				data.insert("checkable", button->isCheckable()); data.insert("checked", button->isChecked());
				if (auto *box = qobject_cast<QCheckBox *>(button)) data.insert("indeterminate", box->checkState() == Qt::PartiallyChecked);
				if (auto *push = qobject_cast<QPushButton *>(button)) { data.insert("default", push->isDefault()); data.insert("menu", push->menu() != nullptr); }
				if (auto *tool = qobject_cast<QToolButton *>(button)) data.insert("menu", tool->menu() != nullptr);
			} else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
				data.insert("type", "combo"); data.insert("index", combo->currentIndex());
				data.insert("editable", combo->isEditable()); data.insert("value", combo->currentText());
				data.insert("placeholder", combo->placeholderText());
				QStyleOptionComboBox option; option.initFrom(combo);
				option.editable = combo->isEditable(); option.frame = combo->hasFrame();
				option.currentText = combo->currentText(); option.currentIcon = combo->itemIcon(combo->currentIndex()); option.iconSize = combo->iconSize();
				data.insert("textRect", rectangle(combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo)));
				data.insert("nativeTextWidth", QFontMetricsF(combo->font()).horizontalAdvance(combo->currentIndex() < 0 ? combo->placeholderText() : combo->currentText()));
				if (const auto *edit = combo->lineEdit()) {
					data.insert("readOnly", edit->isReadOnly()); data.insert("maxLength", edit->maxLength());
					data.insert("placeholder", edit->placeholderText());
				}
				QJsonArray choices;
				for (int i = 0; i < combo->count(); ++i) choices.append(QJsonObject{{"text", combo->itemText(i)},
					{"enabled", bool(combo->model()->index(i, combo->modelColumn(), combo->rootModelIndex()).flags() & Qt::ItemIsEnabled)}});
				data.insert("choices", choices);
			} else if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
				data.insert("type", "number"); data.insert("value", spin->value()); data.insert("minimum", spin->minimum());
				data.insert("maximum", spin->maximum()); data.insert("step", spin->singleStep()); data.insert("readOnly", spin->isReadOnly());
				data.insert("prefix", spin->prefix()); data.insert("suffix", spin->suffix());
			} else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
				data.insert("type", "number"); data.insert("value", spin->value()); data.insert("minimum", spin->minimum());
				data.insert("maximum", spin->maximum()); data.insert("step", spin->singleStep()); data.insert("readOnly", spin->isReadOnly());
				data.insert("prefix", spin->prefix()); data.insert("suffix", spin->suffix()); data.insert("decimals", spin->decimals());
			} else if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
				// Hotkey editors consume native key events and must retain their own surface.
				if (widget->inherits("OBSHotkeyEdit")) data.insert("type", "native");
				else {
					data.insert("type", "text"); data.insert("value", edit->text());
					data.insert("password", edit->echoMode() != QLineEdit::Normal);
					data.insert("placeholder", edit->placeholderText()); data.insert("readOnly", edit->isReadOnly());
					data.insert("maxLength", edit->maxLength());
					data.insert("selectionStart", edit->selectionStart()); data.insert("selectionLength", edit->selectedText().size());
					lineEditDecoration(edit, data);
				}
			} else if (auto *edit = qobject_cast<QTextEdit *>(widget)) {
				data.insert("type", "multiline"); data.insert("value", edit->toPlainText());
				data.insert("readOnly", edit->isReadOnly()); data.insert("placeholder", edit->placeholderText());
			} else if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
				data.insert("type", "multiline"); data.insert("value", edit->toPlainText());
				data.insert("readOnly", edit->isReadOnly()); data.insert("placeholder", edit->placeholderText());
			} else if (auto *label = qobject_cast<QLabel *>(widget)) {
				data.insert("type", label->text().contains(QStringLiteral("<a "), Qt::CaseInsensitive) ? "native" : "label");
				data.insert("text", plainText(label->text()));
				data.insert("wordWrap", label->wordWrap());
				data.insert("alignment", int(label->alignment()));
				if (!label->wordWrap() && !label->text().contains(QLatin1Char('<'))) {
					const int margin = label->margin();
					data.insert("textRect", rectangle(label->contentsRect().adjusted(margin, margin, -margin, -margin)));
					data.insert("nativeTextWidth", QFontMetricsF(label->font()).horizontalAdvance(plainText(label->text())));
				}
				widgetDecoration(label, data);
				if (label->text().isEmpty() && !label->pixmap().isNull()) {
					const auto paintRect = clipped.translated(-label->mapTo(dialog, QPoint()));
					data.insert("iconRect", rectangle(paintRect));
					data.insert("icon", raster(label, QStringLiteral("pixmap"), [&](QPainter &painter) {
						const int margin = label->margin();
						const auto area = label->contentsRect().adjusted(margin, margin, -margin, -margin);
						if (label->hasScaledContents()) painter.drawPixmap(area, label->pixmap());
						else label->style()->drawItemPixmap(&painter, area, int(label->alignment()), label->pixmap());
					}, paintRect));
				}
			} else if (auto *bar = qobject_cast<QTabBar *>(widget)) {
				data.insert("type", "tabs"); data.insert("index", bar->currentIndex());
				QJsonArray tabs;
				for (int i = 0; i < bar->count(); ++i) if (bar->isTabVisible(i))
					tabs.append(QJsonObject{{"index", i}, {"text", buttonText(bar->tabText(i))}, {"enabled", bar->isTabEnabled(i)}, {"rect", rectangle(bar->tabRect(i))}});
				data.insert("tabs", tabs);
				atomic = false;
			} else if (auto *view = qobject_cast<QAbstractItemView *>(widget); view && !qobject_cast<QHeaderView *>(view)) {
				if (view->dragDropMode() == QAbstractItemView::NoDragDrop &&
				    (qobject_cast<QTreeView *>(view) || qobject_cast<QTableView *>(view) || qobject_cast<QListView *>(view))) {
					itemView(view, data);
					nodes.append(data);
					// Persistent index widgets and active delegates are real Qt controls too.
					collect(view->viewport(), nodes);
					continue;
				}
				data.insert("type", "native");
			} else if (auto *area = qobject_cast<QScrollArea *>(widget)) {
				data.insert("type", "scrollArea");
				data.insert("verticalScroll", scrollState(area->verticalScrollBar()));
				data.insert("horizontalScroll", scrollState(area->horizontalScrollBar()));
				atomic = false;
			} else if (auto *scroll = qobject_cast<QScrollBar *>(widget)) {
				data.insert("type", "scroll"); data.insert("minimum", scroll->minimum()); data.insert("maximum", scroll->maximum());
				data.insert("value", scroll->value()); data.insert("step", scroll->singleStep()); data.insert("page", scroll->pageStep());
				data.insert("vertical", scroll->orientation() == Qt::Vertical);
				data.insert("nativeStyle", scrollbarStyle(scroll));
			} else if (auto *slider = qobject_cast<QAbstractSlider *>(widget)) {
				data.insert("type", "slider"); data.insert("minimum", slider->minimum()); data.insert("maximum", slider->maximum());
				data.insert("value", slider->value()); data.insert("step", slider->singleStep()); data.insert("vertical", slider->orientation() == Qt::Vertical);
			} else if (auto *group = qobject_cast<QGroupBox *>(widget)) {
				data.insert("type", "group"); data.insert("text", buttonText(group->title())); data.insert("checkable", group->isCheckable());
				data.insert("checked", group->isChecked()); atomic = false;
				// Preserve QSS group backgrounds, title spacing and the native
				// check indicator. Render only this widget, never its child fields.
				const auto paintRect = visibleRectangle(group, dialog).translated(-group->mapTo(dialog, QPoint()));
				data.insert("decorationRect", rectangle(paintRect));
				data.insert("decoration", raster(group, QStringLiteral("group"), [&](QPainter &painter) {
					group->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
				}, paintRect));
				QStyleOptionGroupBox option; option.initFrom(group); option.text = group->title();
				option.textAlignment = group->alignment(); option.lineWidth = 1;
				option.subControls = QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxLabel;
				if (group->isCheckable()) option.subControls |= QStyle::SC_GroupBoxCheckBox;
				const auto label = group->style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, group);
				const auto indicator = group->isCheckable() ? group->style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, group) : QRect();
				data.insert("titleRect", rectangle(label.united(indicator)));
			} else if (auto *progress = qobject_cast<QProgressBar *>(widget)) {
				data.insert("type", "progress"); data.insert("minimum", progress->minimum()); data.insert("maximum", progress->maximum());
				data.insert("value", progress->value()); data.insert("text", progress->text());
			} else {
				auto children = widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
				children.removeIf([](QWidget *child) { return child->property(surfaceProperty).toBool(); });
				if (widget->inherits("OBSQTDisplay") || widget->inherits("OBSBasicPreview") ||
				    (children.isEmpty() && (!isStandardContainer(widget) || QByteArray(widget->metaObject()->className()) == "QWidget"))) {
					data.insert("type", "native");
				} else {
					atomic = false;
					auto *frame = qobject_cast<QFrame *>(widget);
					const auto background = widget->palette().color(widget->backgroundRole());
					if ((widget->autoFillBackground() || widget->testAttribute(Qt::WA_StyledBackground) ||
					     (frame && frame->frameShape() != QFrame::NoFrame)) && background.alpha() == 255) {
						data.insert("type", "panel");
						data.insert("background", background.name());
						data.insert("frameWidth", frame ? frame->frameWidth() : 0);
						widgetDecoration(widget, data);
					}
				}
			}
			if (data.contains("type")) nodes.append(data);
			if (!atomic) collect(widget, nodes);
		}
	}
};

QtDialogBridge::QtDialogBridge(QWidget *dialog, QObject *parent) : QObject(parent), impl(std::make_unique<Impl>())
{
	impl->dialog = dialog;
}

QtDialogBridge::~QtDialogBridge() = default;

QJsonObject QtDialogBridge::snapshot()
{
	if (!impl->dialog) return {{"closed", true}};
	impl->prune();
	QJsonArray nodes;
	impl->collect(impl->dialog, nodes, !qobject_cast<QDialog *>(impl->dialog));
	const auto palette = impl->dialog->palette();
	const auto font = fontState(impl->dialog);
	const QJsonObject theme{{"window", palette.color(QPalette::Window).name()}, {"windowText", palette.color(QPalette::WindowText).name()},
		{"base", palette.color(QPalette::Base).name()}, {"text", palette.color(QPalette::Text).name()},
		{"button", palette.color(QPalette::Button).name()}, {"buttonText", palette.color(QPalette::ButtonText).name()},
		{"mid", palette.color(QPalette::Mid).name()}, {"highlight", palette.color(QPalette::Highlight).name()},
		{"highlightedText", palette.color(QPalette::HighlightedText).name()}, {"fontFamily", font.value("family")},
		{"fontSize", font.value("pixelSize")}, {"dark", palette.color(QPalette::Window).lightness() < 128}};
	const auto title = impl->dialog->windowTitle().isEmpty() ? impl->dialog->window()->windowTitle() : impl->dialog->windowTitle();
	return {{"title", title}, {"width", impl->dialog->width()}, {"height", impl->dialog->height()},
		{"focus", impl->widgetIds.value(impl->dialog->focusWidget())},
		{"acceptDrops", impl->dialog->acceptDrops()},
		{"enabled", impl->dialog->isEnabled()}, {"nodes", nodes}, {"theme", theme}, {"closed", !impl->dialog->isVisible()}};
}

bool QtDialogBridge::drop(const ExternalDropData &data, QString &error)
{
	error.clear();
	const QPointer<QWidget> target(impl->dialog);
	if (!target || !target->isVisible() || !target->isEnabled() || !target->acceptDrops()) {
		error = QStringLiteral("This dialog is not accepting dropped items."); return false;
	}
	if (QApplication::activeModalWidget() && QApplication::activeModalWidget() != target->window()) {
		error = QStringLiteral("Complete the active dialog first."); return false;
	}
	const bool delivered = DispatchExternalDrop(target, data);
	if (!delivered) error = QStringLiteral("This dialog did not accept the dropped items.");
	return delivered;
}

bool QtDialogBridge::execute(const QString &command, const QJsonObject &args, QString &error)
{
	error.clear();
	auto reject = [&](const char *reason) { error = QString::fromLatin1(reason); return false; };
	const QPointer<QWidget> dialog(impl->dialog);
	if (!dialog || !dialog->isVisible() || !dialog->isEnabled()) return reject("Dialog is no longer available");
	if (QApplication::activeModalWidget() && QApplication::activeModalWidget() != dialog->window()) return reject("Complete the active dialog first");
	QPointer<QWidget> widget = impl->widgets.value(args.value("id").toString());
	if (command == "dialog.key" && !args.contains("id")) widget = dialog.data();
	if (!widget || (widget != dialog && !dialog->isAncestorOf(widget)) || !widget->isVisibleTo(dialog) || !widget->isEnabled())
		return reject("Control is no longer available");
	if (command == "dialog.selection" || command == "dialog.context") {
		if (auto *combo = qobject_cast<QComboBox *>(widget); combo && combo->isEditable() && combo->lineEdit())
			widget = combo->lineEdit();
	}
	if (command == "dialog.click") {
		if (auto *button = qobject_cast<QAbstractButton *>(widget)) { button->click(); return true; }
		if (auto *group = qobject_cast<QGroupBox *>(widget); group && group->isCheckable()) {
			group->setChecked(!group->isChecked()); if (widget) emit group->clicked(group->isChecked()); return true;
		}
	} else if (command == "dialog.menu") {
		if (auto *tool = qobject_cast<QToolButton *>(widget); tool && tool->menu()) { tool->showMenu(); return true; }
		if (auto *button = qobject_cast<QPushButton *>(widget); button && button->menu()) { button->showMenu(); return true; }
		return reject("Control has no menu");
	} else if (command == "dialog.input") {
		const auto value = args.value("value");
		auto enterText = [&](QLineEdit *edit) {
			if (edit->isReadOnly() || !value.isString() || value.toString().size() > edit->maxLength()) return reject("Invalid text input");
			QString proposed = value.toString(); int position = proposed.size();
			if (edit->validator() && edit->validator()->validate(proposed, position) == QValidator::Invalid) return reject("Invalid text input");
			if (edit->text() != proposed) { edit->selectAll(); edit->insert(proposed); }
			return true;
		};
		if (auto *edit = qobject_cast<QLineEdit *>(widget)) return enterText(edit);
		if (auto *edit = qobject_cast<QTextEdit *>(widget)) {
			if (edit->isReadOnly() || !value.isString()) return reject("Read only text");
			if (edit->toPlainText() != value.toString()) {
				auto cursor = edit->textCursor(); cursor.select(QTextCursor::Document); cursor.insertText(value.toString());
			}
			return true;
		}
		if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
			if (edit->isReadOnly() || !value.isString()) return reject("Read only text");
			if (edit->toPlainText() != value.toString()) {
				auto cursor = edit->textCursor(); cursor.select(QTextCursor::Document); cursor.insertText(value.toString());
			}
			return true;
		}
		if (auto *combo = qobject_cast<QComboBox *>(widget)) {
			if (!combo->isEditable() || !combo->lineEdit()) return reject("Choice is not editable");
			return enterText(combo->lineEdit());
		}
		if (!value.isDouble() || !std::isfinite(value.toDouble())) return reject("Invalid numeric input");
		const auto number = value.toDouble();
		if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
			if (spin->isReadOnly() || number < spin->minimum() || number > spin->maximum() || std::floor(number) != number) return reject("Number is out of range");
			spin->setValue(int(number)); return true;
		}
		if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
			if (spin->isReadOnly() || number < spin->minimum() || number > spin->maximum()) return reject("Number is out of range");
			spin->setValue(number); return true;
		}
		if (auto *slider = qobject_cast<QAbstractSlider *>(widget)) {
			if (number < slider->minimum() || number > slider->maximum() || std::floor(number) != number) return reject("Number is out of range");
			slider->setSliderDown(true); if (widget) slider->setSliderPosition(int(number));
			if (widget) slider->setValue(int(number));
			return true;
		}
	} else if (command == "dialog.selection") {
		const int start = args.value("start").toInt(-1), end = args.value("end").toInt(-1);
		if (start < 0 || end < start) return reject("Invalid text selection");
		if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
			if (end > edit->text().size()) return reject("Invalid text selection");
			edit->setSelection(start, end - start); return true;
		}
		QTextCursor cursor;
		if (auto *edit = qobject_cast<QTextEdit *>(widget)) cursor = edit->textCursor();
		if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) cursor = edit->textCursor();
		if (cursor.isNull() || end >= cursor.document()->characterCount()) return reject("Invalid text selection");
		cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
		if (auto *edit = qobject_cast<QTextEdit *>(widget)) edit->setTextCursor(cursor);
		if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) edit->setTextCursor(cursor);
		return true;
	} else if (command == "dialog.finish") {
		if (auto *view = owningView(widget)) {
			const QPointer<QWidget> focused(QApplication::focusWidget());
			if (focused && (focused == widget || widget->isAncestorOf(focused))) {
				// A delegate ignores synthetic FocusOut while its editor still owns
				// Qt focus. Move focus to the actual browser (or view in native tests)
				// so Qt performs its original commit/closeEditor path exactly once.
				auto *surface = dialog->findChild<WebView2Widget *>(QStringLiteral("obsWebView2DialogSurface"), Qt::FindDirectChildrenOnly);
				QWidget *target = surface && surface->isVisible() ? static_cast<QWidget *>(surface) : view;
				target->setFocus(Qt::OtherFocusReason);
				if (!widget || QApplication::focusWidget() != focused) return true;
			}
			QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
			QApplication::sendEvent(widget, &event); return true;
		}
		if (auto *edit = qobject_cast<QLineEdit *>(widget)) { if (edit->hasAcceptableInput()) emit edit->editingFinished(); return true; }
		if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) { spin->interpretText(); if (widget) emit spin->editingFinished(); return true; }
		if (auto *combo = qobject_cast<QComboBox *>(widget); combo && combo->lineEdit()) {
			if (combo->lineEdit()->hasAcceptableInput()) emit combo->lineEdit()->editingFinished(); return true;
		}
		if (auto *slider = qobject_cast<QAbstractSlider *>(widget)) { slider->setSliderDown(false); return true; }
		return true;
	} else if (command == "dialog.choose") {
		const auto value = args.value("index");
		if (!value.isDouble() || value.toDouble() != value.toInt(-1)) return reject("Invalid choice");
		const auto index = value.toInt(-1);
		if (auto *combo = qobject_cast<QComboBox *>(widget)) {
			if (index < 0 || index >= combo->count() || !(combo->model()->index(index, combo->modelColumn(), combo->rootModelIndex()).flags() & Qt::ItemIsEnabled)) return reject("Choice is no longer available");
			combo->setCurrentIndex(index);
			if (widget) emit combo->activated(index);
			if (widget) emit combo->textActivated(combo->currentText());
			return true;
		}
		if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
			if (index < 0 || index >= tabs->count() || !tabs->isTabEnabled(index) || !tabs->isTabVisible(index)) return reject("Tab is no longer available");
			tabs->setCurrentIndex(index); return true;
		}
	} else if (command == "dialog.item") {
		auto *view = qobject_cast<QAbstractItemView *>(widget);
		const auto index = impl->indexes.value(args.value("item").toString());
		if (!view || !index.isValid() || index.model() != view->model() || !(index.flags() & Qt::ItemIsEnabled)) return reject("Item is no longer available");
		const auto action = args.value("action").toString();
		if (action == "toggle") {
			if (!(index.flags() & Qt::ItemIsUserCheckable)) return reject("Item is not checkable");
			const int checked = index.data(Qt::CheckStateRole).toInt();
			const int next = index.flags() & Qt::ItemIsUserTristate ? (checked + 1) % 3 : checked == Qt::Checked ? Qt::Unchecked : Qt::Checked;
			return view->model()->setData(index, next, Qt::CheckStateRole);
		}
		if (action == "expand") {
			auto *tree = qobject_cast<QTreeView *>(view);
			if (!tree) return reject("Item has no expansion control");
			tree->setExpanded(index, !tree->isExpanded(index)); return true;
		}
		if (action == "edit") {
			if (!(index.flags() & Qt::ItemIsEditable)) return reject("Item is not editable");
			view->edit(index); return true;
		}
		if (action == "set") {
			if (!(index.flags() & Qt::ItemIsEditable) || !args.value("value").isString()) return reject("Item is not editable");
			return view->model()->setData(index, args.value("value").toString(), Qt::EditRole);
		}
		if (action != "select" && action != "activate" && action != "context") return reject("Unknown item action");
		view->scrollTo(index);
		const auto point = view->visualRect(index).center();
		if (action == "context") {
			mouseClick(view->viewport(), point, modifiers(args), false, Qt::RightButton);
			if (!widget) return true;
			QContextMenuEvent event(QContextMenuEvent::Mouse, point, view->viewport()->mapToGlobal(point), modifiers(args));
			QApplication::sendEvent(view->viewport(), &event);
		} else mouseClick(view->viewport(), point, modifiers(args), action == "activate");
		return true;
	} else if (command == "dialog.scroll") {
		auto *area = qobject_cast<QAbstractScrollArea *>(widget);
		if (!area) return reject("Control cannot scroll");
		auto *bar = args.value("horizontal").toBool() ? area->horizontalScrollBar() : area->verticalScrollBar();
		const auto value = args.value("value");
		if (!value.isDouble() || value.toDouble() < bar->minimum() || value.toDouble() > bar->maximum()) return reject("Invalid scroll position");
		bar->setValue(value.toInt()); return true;
	} else if (command == "dialog.wheel") {
		auto *area = qobject_cast<QAbstractScrollArea *>(widget);
		const auto dx = args.value("deltaX"), dy = args.value("deltaY");
		if (!area || !dx.isDouble() || !dy.isDouble() || !std::isfinite(dx.toDouble()) || !std::isfinite(dy.toDouble()))
			return reject("Invalid wheel input");
		const QPoint delta(qRound(std::clamp(-dx.toDouble(), -1200.0, 1200.0)), qRound(std::clamp(-dy.toDouble(), -1200.0, 1200.0)));
		const auto point = area->viewport()->rect().center();
		QWheelEvent event(QPointF(point), QPointF(area->viewport()->mapToGlobal(point)), QPoint(), delta,
			Qt::NoButton, modifiers(args), Qt::NoScrollPhase, false);
		QApplication::sendEvent(area->viewport(), &event); return true;
	} else if (command == "dialog.context") {
		const auto point = widget->rect().center();
		QContextMenuEvent event(QContextMenuEvent::Mouse, point, widget->mapToGlobal(point), modifiers(args));
		QApplication::sendEvent(widget, &event); return true;
	} else if (command == "dialog.header") {
		QHeaderView *header = nullptr;
		if (auto *tree = qobject_cast<QTreeView *>(widget)) header = tree->header();
		if (auto *table = qobject_cast<QTableView *>(widget)) header = table->horizontalHeader();
		const int column = args.value("column").toInt(-1);
		if (!header || column < 0 || column >= header->count() || header->isSectionHidden(column)) return reject("Column is no longer available");
		mouseClick(header->viewport(), QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2, header->height() / 2), modifiers(args));
		return true;
	} else if (command == "dialog.key") {
		const auto key = args.value("key").toString();
		static const QHash<QString, int> keys{{"Enter", Qt::Key_Return}, {"Escape", Qt::Key_Escape}, {"Delete", Qt::Key_Delete},
			{"Backspace", Qt::Key_Backspace}, {"ArrowUp", Qt::Key_Up}, {"ArrowDown", Qt::Key_Down}, {"ArrowLeft", Qt::Key_Left},
			{"ArrowRight", Qt::Key_Right}, {"Home", Qt::Key_Home}, {"End", Qt::Key_End}, {"PageUp", Qt::Key_PageUp},
			{"PageDown", Qt::Key_PageDown}, {"F2", Qt::Key_F2}, {"Tab", Qt::Key_Tab}, {" ", Qt::Key_Space}};
		int code = keys.value(key, 0);
		if (key == "Tab" && args.value("shift").toBool()) code = Qt::Key_Backtab;
		if (!code && key.size() == 1) code = key.toUpper().at(0).unicode();
		if (!code) return reject("Unsupported key");
		QKeyEvent press(QEvent::KeyPress, code, modifiers(args), key.size() == 1 ? key : QString());
		QApplication::sendEvent(widget, &press);
		if (widget) { QKeyEvent release(QEvent::KeyRelease, code, modifiers(args)); QApplication::sendEvent(widget, &release); }
		return true;
	}
	return reject("Unsupported control action");
}
} // namespace OBSWeb

namespace {
bool CloakOpeningWindow(QWidget *window, bool hidden)
{
	using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
	static auto setAttribute = reinterpret_cast<SetAttribute>(QLibrary::resolve(QStringLiteral("dwmapi"), "DwmSetWindowAttribute"));
	const BOOL cloak = hidden;
	const auto id = hidden ? window->winId() : window->internalWinId();
	return id && setAttribute && SUCCEEDED(setAttribute(reinterpret_cast<HWND>(id), DWMWA_CLOAK, &cloak, sizeof(cloak)));
}

class DialogOpening : public QObject {
	QPointer<QDialog> dialog;
	qreal opacity;
	QTimer deadline;
	bool pending = true;
	bool cloaked = false;

public:
	DialogOpening(QDialog *target, QObject *owner, std::function<void()> fallback)
		: QObject(owner), dialog(target), opacity(target->windowOpacity())
	{
		// Show events arrive before Qt maps the top-level window. Keep its normal
		// visibility/modality/layout lifecycle and DWM composition running. Opacity
		// changes recreate the layered-window backing store and briefly expose Qt
		// controls; DWM cloaking reveals the already-composed WebView in one step.
		cloaked = CloakOpeningWindow(target, true);
		if (!cloaked) target->setWindowOpacity(0);
		target->setProperty("webview2Opening", true);
		deadline.setSingleShot(true);
		connect(&deadline, &QTimer::timeout, this, [this, fallback = std::move(fallback)] {
			fallback();
			finish();
		});
		deadline.start(3000);
	}
	~DialogOpening() override { finish(); }
	void finish()
	{
		if (!pending) return;
		pending = false;
		deadline.stop();
		if (dialog) {
			dialog->setProperty("webview2Opening", false);
			if (cloaked) CloakOpeningWindow(dialog, false);
			else dialog->setWindowOpacity(opacity);
		}
	}
};

class DialogSurface : public QObject {
	QPointer<QWidget> dialog;
	QPointer<WebView2Widget> web;
	QPointer<DialogOpening> opening;
	OBSWeb::QtDialogBridge bridge;
	QTimer timer;
	QByteArray lastSnapshot;
	bool ready = false;
	bool failed = false;
	bool geometryPending = false;

public:
	DialogSurface(QWidget *target, const QString &assets, const QString &profile, QObject *owner, DialogOpening *pendingOpening)
		: QObject(owner), dialog(target), opening(pendingOpening), bridge(target, this)
	{
		target->setProperty("webview2NativeFallback", false);
		target->installEventFilter(this);
		web = new WebView2Widget(target, assets, profile, QStringLiteral("dialog.html"));
		web->setProperty(surfaceProperty, true);
		web->setObjectName(QStringLiteral("obsWebView2DialogSurface"));
		web->setGeometry(target->rect());
		// The controller must render to produce its first-frame acknowledgement.
		// Top-level dialogs stay cloaked until that acknowledgement arrives.
		web->hide();
		connect(target, &QObject::destroyed, this, [this] { dialog = nullptr; web = nullptr; deleteLater(); });
		connect(web, &WebView2Widget::ready, this, [this] {
			if (!dialog || !web || failed) return;
			ready = true; failed = false;
			web->setGeometry(dialog->rect()); web->show(); web->raise();
			publish(true);
			web->setFocus(Qt::OtherFocusReason);
		});
		connect(web, &WebView2Widget::presented, this, [this] {
			if (!failed && opening) opening->finish();
		});
		connect(web, &WebView2Widget::failed, this, [this](const QString &) {
			fallback();
		});
		connect(web, &WebView2Widget::messageReceived, this, [this](const QJsonObject &request) {
			const QPointer<DialogSurface> guard(this);
			if (!dialog || !web) return;
			const auto command = request.value("command").toString();
			QString error;
			const bool ok = command == "dialog.state" || bridge.execute(command, request.value("args").toObject(), error);
			if (!guard || !dialog || !web) return;
			QJsonObject response{{"version", 1}, {"id", request.value("id")}, {"ok", ok}};
			if (ok) response.insert("result", QJsonObject{});
			else response.insert("error", QJsonObject{{"code", "DialogActionRejected"}, {"message", error}});
			web->postMessage(response);
			publish(true);
		});
		connect(web, &WebView2Widget::externalDrop, this, [this](const QString &id, const OBSWeb::ExternalDropData &data) {
			const QPointer<DialogSurface> guard(this);
			if (!dialog || !web) return;
			QString error;
			const bool ok = bridge.drop(data, error);
			if (!guard || !dialog || !web) return;
			QJsonObject response{{"version", 1}, {"id", id}, {"ok", ok}};
			if (ok) response.insert("result", QJsonObject{{"delivered", true}});
			else response.insert("error", QJsonObject{{"code", "DialogDropRejected"}, {"message", error}});
			web->postMessage(response);
			publish(true);
		});
		timer.setInterval(120);
		connect(&timer, &QTimer::timeout, this, [this] { publish(false); });
		timer.start();
	}

	~DialogSurface() override { if (web) delete web.data(); }

	void fallback()
	{
		failed = true; ready = false;
		if (web) web->hide();
		if (dialog) dialog->setProperty("webview2NativeFallback", true);
		if (opening) opening->finish();
	}

	void retire()
	{
		timer.stop();
		if (web) web->hide();
		if (dialog) dialog->removeEventFilter(this);
		dialog = nullptr;
		deleteLater();
	}

	void publish(bool force)
	{
		if (!ready || failed || !dialog || !dialog->isVisible() || !web) return;
		const auto timingFile = qEnvironmentVariable("OBS_WEBVIEW2_TRACE_PERFORMANCE");
		QElapsedTimer timing;
		if (!timingFile.isEmpty()) timing.start();
		const auto state = bridge.snapshot();
		const auto snapshotMs = timing.isValid() ? timing.nsecsElapsed() / 1e6 : 0.0;
		const auto json = QJsonDocument(state).toJson(QJsonDocument::Compact);
		if (timing.isValid()) {
			QFile trace(timingFile);
			if (trace.open(QIODevice::WriteOnly | QIODevice::Append))
				trace.write(QJsonDocument(QJsonObject{{"component", "dialog"}, {"class", dialog->metaObject()->className()},
					{"stage", "snapshot"}, {"snapshotMs", snapshotMs}, {"elapsedMs", timing.nsecsElapsed() / 1e6},
					{"bytes", json.size()}, {"nodes", state.value("nodes").toArray().size()}, {"forced", force},
					{"epochMs", QDateTime::currentMSecsSinceEpoch()}}).toJson(QJsonDocument::Compact) + '\n');
		}
		if (!force && json == lastSnapshot) return;
		lastSnapshot = json;
		QRegion region(dialog->rect());
		bool hasNative = false;
		for (const auto entry : state.value("nodes").toArray()) {
			const auto node = entry.toObject();
			if (node.value("type") != "native") continue;
			const auto rect = node.value("clip").toObject();
			region -= QRect(rect.value("x").toInt(), rect.value("y").toInt(), rect.value("width").toInt(), rect.value("height").toInt());
			hasNative = true;
		}
		if (hasNative && region.isEmpty()) {
			// Qt treats an empty mask as no mask, which would cover the native control.
			web->hide();
			if (opening) opening->finish();
		} else {
			if (hasNative) web->setMask(region); else web->clearMask();
			if (!web->isVisible()) { web->show(); web->raise(); }
		}
		web->postMessage({{"version", 1}, {"event", "dialog.state"}, {"data", state}});
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == dialog && web) {
			if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest) {
				web->setGeometry(dialog->rect());
				if (!geometryPending) {
					geometryPending = true;
					// Publish after Qt finishes this layout, rather than waiting for
					// the 120ms state poll while the browser stretches an old frame.
					QTimer::singleShot(0, this, [this] { geometryPending = false; publish(false); });
				}
			}
			if (event->type() == QEvent::Show && ready && !failed) { web->show(); web->raise(); publish(true); }
		}
		return QObject::eventFilter(watched, event);
	}
};

class DialogInstaller : public QObject {
	QString assets;
	QString profile;
	QHash<QWidget *, QPointer<DialogSurface>> surfaces;
	QHash<QWidget *, QPointer<DialogOpening>> openings;
	QHash<QDockWidget *, QPointer<QWidget>> dockContents;
	QSet<QDockWidget *> queuedDocks;

	void ensureSurface(QWidget *content)
	{
		if (content && content->property(externalSurfaceProperty).toBool()) {
			retireSurface(content);
			return;
		}
		if (!content || !content->isVisible() || content->property(surfaceProperty).toBool() || surfaces.value(content)) return;
		auto *surface = new DialogSurface(content, assets, profile, this, openings.value(content));
		surfaces.insert(content, surface);
		connect(content, &QObject::destroyed, this, [this, content] { surfaces.remove(content); });
	}

	void retireSurface(QWidget *content)
	{
		if (auto surface = surfaces.take(content)) surface->retire();
	}

	void refreshDock(QDockWidget *dock)
	{
		auto *content = dock->isFloating() && dock->isVisible() ? dock->widget() : nullptr;
		if (content && content->property(externalSurfaceProperty).toBool()) content = nullptr;
		const auto previous = dockContents.value(dock);
		if (previous && previous != content) retireSurface(previous);
		dockContents.insert(dock, content);
		// The Qt dock titlebar keeps its native move, dock, float, and close behavior.
		if (content) ensureSurface(content);
	}

	void queueDock(QDockWidget *dock)
	{
		if (queuedDocks.contains(dock)) return;
		queuedDocks.insert(dock);
		const QPointer<QDockWidget> guard(dock);
		QTimer::singleShot(0, this, [this, guard, dock] {
			queuedDocks.remove(dock);
			if (guard) refreshDock(guard);
		});
	}

	void watchDock(QDockWidget *dock)
	{
		if (dockContents.contains(dock)) return;
		dockContents.insert(dock, nullptr);
		connect(dock, &QDockWidget::topLevelChanged, this, [this, dock](bool floating) {
			if (!floating) {
				retireSurface(dockContents.value(dock));
				dockContents[dock] = nullptr;
			}
			queueDock(dock);
		});
		connect(dock, &QObject::destroyed, this, [this, dock] {
			retireSurface(dockContents.take(dock));
			queuedDocks.remove(dock);
		});
		queueDock(dock);
	}

public:
	DialogInstaller(QObject *owner, QString assetsPath, QString profilePath)
		: QObject(owner), assets(std::move(assetsPath)), profile(std::move(profilePath))
	{
		qApp->installEventFilter(this);
		for (auto *widget : QApplication::allWidgets())
			if (auto *dock = qobject_cast<QDockWidget *>(widget)) watchDock(dock);
	}

	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::Hide && !event->spontaneous()) {
			// QDialog::done/reject can destroy its platform window while retaining
			// the QWidget and model (Remux, Scripts, browser docks). That closes the
			// child WebView controller. Recreate only the renderer on the next Show.
			if (auto *dialog = qobject_cast<QDialog *>(object)) {
				retireSurface(dialog);
				if (auto opening = openings.take(dialog)) { opening->finish(); opening->deleteLater(); }
			}
		}
		if (event->type() == QEvent::DynamicPropertyChange &&
		    static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == externalSurfaceProperty) {
			if (auto *content = qobject_cast<QWidget *>(object)) {
				if (content->property(externalSurfaceProperty).toBool()) retireSurface(content);
				if (auto *dock = qobject_cast<QDockWidget *>(content->parentWidget()); dock && dock->widget() == content) {
					watchDock(dock);
					queueDock(dock);
				}
			}
		}
		if (auto *dock = qobject_cast<QDockWidget *>(object)) {
			if (event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildRemoved) {
				watchDock(dock);
				queueDock(dock);
			}
		}
		if (event->type() != QEvent::Show) return false;
		auto *dialog = qobject_cast<QDialog *>(object);
		if (!dialog || dialog->property(surfaceProperty).toBool() || qobject_cast<QFileDialog *>(dialog) ||
		    qobject_cast<QColorDialog *>(dialog) || qobject_cast<QFontDialog *>(dialog) || surfaces.value(dialog) ||
		    dialog->property(externalSurfaceProperty).toBool() || openings.value(dialog)) return false;
		// Queue outside QWidget::show and outside any WebView2 COM callback.
		const QPointer<QDialog> guard(dialog);
		// Spontaneous Show also occurs on restore/unminimize. Do not restart an
		// existing window's presentation or interfere with that native transition.
		if (!event->spontaneous()) {
			auto *opening = new DialogOpening(dialog, this, [this, guard] {
				if (!guard) return;
				if (auto surface = surfaces.value(guard)) surface->fallback();
				else guard->setProperty("webview2NativeFallback", true);
			});
			openings.insert(dialog, opening);
			connect(dialog, &QObject::destroyed, this, [this, dialog] {
				if (auto opening = openings.take(dialog)) opening->deleteLater();
			});
		}
		QTimer::singleShot(0, this, [this, guard] {
			if (guard) ensureSurface(guard);
		});
		return false;
	}
};
} // namespace

void InstallWebView2Dialogs(QObject *owner, const QString &assets, const QString &profile)
{
	if (!owner || !qApp || owner->property(installedProperty).toBool()) return;
	owner->setProperty(installedProperty, true);
	new DialogInstaller(owner, assets, profile);
}
