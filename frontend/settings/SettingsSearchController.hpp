// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// Keep the index in the original Settings dialog: the WebView2 dialog bridge
// mirrors these Qt controls and forwards the same actions as the native UI.
namespace OBSSettingsSearch {
inline QString normalize(QString value)
{
	if (Qt::mightBeRichText(value)) {
		QTextDocument document;
		document.setHtml(value);
		value = document.toPlainText();
	}
	value.remove(QLatin1Char('&'));
	QString result;
	for (const QChar character : value.normalized(QString::NormalizationForm_D).toCaseFolded()) {
		if (character.category() == QChar::Mark_NonSpacing || character.category() == QChar::Mark_SpacingCombining ||
		    character.category() == QChar::Mark_Enclosing)
			continue;
		if (character.isLetterOrNumber()) result.append(character);
		else if (!result.isEmpty() && !result.endsWith(QLatin1Char(' '))) result.append(QLatin1Char(' '));
	}
	return result.trimmed();
}

inline int editDistance(const QString &left, const QString &right, int limit)
{
	if (std::abs(int(left.size() - right.size())) > limit) return limit + 1;
	QVector<int> previous(right.size() + 1), current(right.size() + 1);
	for (int column = 0; column <= right.size(); ++column) previous[column] = column;
	for (int row = 1; row <= left.size(); ++row) {
		current[0] = row;
		int smallest = current[0];
		for (int column = 1; column <= right.size(); ++column) {
			current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
				previous[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1)});
			smallest = std::min(smallest, current[column]);
		}
		if (smallest > limit) return limit + 1;
		previous.swap(current);
	}
	return previous[right.size()];
}

inline int matchScore(const QString &query, const QString &text)
{
	if (query.isEmpty() || text.isEmpty()) return 0;
	if (query == text) return 1000;
	if (text.startsWith(query)) return 850;
	if (text.contains(query)) return 720;
	const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const auto queryWords = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (queryWords.size() > 1) {
		int total = 0;
		for (const auto &part : queryWords) {
			int best = 0;
			for (const auto &word : words) best = std::max(best, matchScore(part, word));
			if (best < 400) return 0;
			total += best;
		}
		return total / int(queryWords.size()) - 80;
	}
	int best = 0;
	const int limit = query.size() >= 7 ? 2 : query.size() >= 3 ? 1 : 0;
	for (const auto &word : words) {
		if (word.startsWith(query)) best = std::max(best, 650);
		if (limit && word.front() == query.front()) {
			const int distance = editDistance(query, word, limit);
			if (distance <= limit) best = std::max(best, 520 - distance * 80);
		}
	}
	return best;
}

struct AliasGroup {
	QStringList terms;
	int preferred;
};

inline const QVector<AliasGroup> &aliases()
{
	static const QVector<AliasGroup> groups{
		{{QStringLiteral("fps"), QStringLiteral("частота кадров"), QStringLiteral("frame rate"),
		  QStringLiteral("framerate"), QStringLiteral("bilder pro sekunde"), QStringLiteral("кадры"),
		  QStringLiteral("кадров")}, 5},
		{{QStringLiteral("битрейт"), QStringLiteral("bitrate"), QStringLiteral("datenrate"),
		  QStringLiteral("скорость передачи"), QStringLiteral("kbps"), QStringLiteral("кбит")}, 3},
		{{QStringLiteral("разрешение"), QStringLiteral("resolution"), QStringLiteral("auflosung"),
		  QStringLiteral("canvas"), QStringLiteral("холст"), QStringLiteral("размер кадра"),
		  QStringLiteral("размер видео")}, 5},
		{{QStringLiteral("кодировщик"), QStringLiteral("энкодер"), QStringLiteral("encoder"),
		  QStringLiteral("кодек"), QStringLiteral("codec"), QStringLiteral("encoding")}, 6},
		{{QStringLiteral("стрим"), QStringLiteral("трансляция"), QStringLiteral("эфир"),
		  QStringLiteral("stream"), QStringLiteral("broadcast"), QStringLiteral("livestream")}, 6},
		{{QStringLiteral("запись"), QStringLiteral("recording"), QStringLiteral("record"),
		  QStringLiteral("aufnahme")}, 4},
		{{QStringLiteral("звук"), QStringLiteral("аудио"), QStringLiteral("audio"),
		  QStringLiteral("sound"), QStringLiteral("microphone"), QStringLiteral("микрофон"),
		  QStringLiteral("mic"), QStringLiteral("mixer"), QStringLiteral("микшер")}, 9},
		{{QStringLiteral("горячие клавиши"), QStringLiteral("hotkeys"), QStringLiteral("shortcuts"),
		  QStringLiteral("шорткаты"), QStringLiteral("tastenkuerzel")}, 5},
		{{QStringLiteral("вывод"), QStringLiteral("output"), QStringLiteral("ausgabe")}, 3},
	};
	return groups;
}

class Controller final : public QObject {
	struct Entry {
		QString title;
		QString category;
		QString section;
		QString searchableTitle;
		QString searchableCategory;
		QString searchableSection;
		QPointer<QWidget> target;
		int page = -1;
		int choice = -1;
	};

	OBSBasicSettings *dialog;
	Ui::OBSBasicSettings *ui;
	QLineEdit *field;
	QListWidget *results;
	QTimer *updateTimer;
	QVector<Entry> entries;
	QPointer<QWidget> highlighted;
	int highlightGeneration = 0;

	void highlight(QWidget *widget)
	{
		if (highlighted) highlighted->setProperty("_obsSettingsSearchHit", false);
		highlighted = widget;
		if (!highlighted) return;
		highlighted->setProperty("_obsSettingsSearchHit", true);
		const int generation = ++highlightGeneration;
		QTimer::singleShot(1900, this, [this, generation] {
			if (generation != highlightGeneration || !highlighted) return;
			highlighted->setProperty("_obsSettingsSearchHit", false);
			highlighted.clear();
		});
	}

	bool available(QWidget *widget, QWidget *page) const
	{
		if (widget->isHidden()) return false;
		for (auto *child = widget; child && child != page; child = child->parentWidget()) {
			auto *stack = qobject_cast<QStackedWidget *>(child->parentWidget());
			if (!stack || stack == ui->settingsPages || stack == ui->outputModePages || stack == ui->fpsTypes ||
			    qobject_cast<QTabWidget *>(stack->parentWidget()))
				continue;
			if (stack->currentWidget() != child) return false;
		}
		return true;
	}

	QString sectionFor(QWidget *widget, QWidget *page) const
	{
		for (auto *parent = widget->parentWidget(); parent && parent != page; parent = parent->parentWidget()) {
			if (auto *group = qobject_cast<QGroupBox *>(parent); group && !group->title().isEmpty())
				return group->title();
			if (auto *tabs = qobject_cast<QTabWidget *>(parent))
				for (int index = 0; index < tabs->count(); ++index)
					if (tabs->widget(index)->isAncestorOf(widget)) return tabs->tabText(index);
		}
		return {};
	}

	void add(int page, const QString &title, QWidget *target, const QString &section = {}, int choice = -1)
	{
		const QString cleaned = title.simplified();
		if (!target || normalize(cleaned).size() < 2 || cleaned.size() > 140) return;
		for (const auto &entry : entries)
			if (entry.page == page && entry.title == cleaned && entry.target == target) return;
		const QString category = ui->listWidget->item(page)->text();
		entries.push_back({cleaned, category, section, normalize(cleaned), normalize(category), normalize(section),
			target, page, choice});
	}

	void buildIndex()
	{
		entries.clear();
		for (int page = 0; page < ui->settingsPages->count() && page < ui->listWidget->count(); ++page) {
			auto *root = ui->settingsPages->widget(page);
			add(page, ui->listWidget->item(page)->text(), root);
			for (auto *widget : root->findChildren<QWidget *>()) {
				if (!available(widget, root)) continue;
				QString title;
				QWidget *target = widget;
				if (auto *label = qobject_cast<QLabel *>(widget)) {
					title = label->text();
					if (label->buddy()) target = label->buddy();
				} else if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
					title = button->text();
				} else if (auto *group = qobject_cast<QGroupBox *>(widget)) {
					title = group->title();
				} else if (widget == ui->fpsType) {
					for (int index = 0; index < ui->fpsType->count(); ++index)
						add(page, ui->fpsType->itemText(index), widget, sectionFor(widget, root), index);
				}
				if (Qt::mightBeRichText(title)) {
					QTextDocument document;
					document.setHtml(title);
					title = document.toPlainText();
				}
				add(page, title.remove(QLatin1Char('&')), target, sectionFor(widget, root));
			}
		}
	}

	QVector<QPair<QString, int>> expandQuery(const QString &query) const
	{
		QVector<QPair<QString, int>> expanded{{query, 0}};
		for (const auto &group : aliases()) {
			bool related = false;
			for (const auto &alias : group.terms)
				if (matchScore(query, normalize(alias)) >= 400) { related = true; break; }
			if (!related) continue;
			for (int index = 0; index < group.terms.size(); ++index)
				expanded.push_back({normalize(group.terms[index]), index < group.preferred ? 90 : 240});
		}
		return expanded;
	}

	int score(const QString &query, const QVector<QPair<QString, int>> &expanded, const Entry &entry) const
	{
		int best = 0;
		for (const auto &term : expanded)
			best = std::max(best, matchScore(term.first, entry.searchableTitle) - term.second);
		if (best <= 0) {
			best = std::max(matchScore(query, entry.searchableSection) - 180,
				matchScore(query, entry.searchableCategory) - 260);
		}
		return std::max(0, best);
	}

	void placeResults()
	{
		const QPoint below = field->mapTo(dialog, QPoint(0, field->height() + 3));
		const int width = std::min(540, dialog->width() - below.x() - 12);
		const int height = std::min(results->count() * 28 + 4, dialog->height() - below.y() - 45);
		results->setGeometry(below.x(), below.y(), std::max(160, width), std::max(28, height));
	}

	void updateResults()
	{
		const QString query = normalize(field->text());
		if (query.isEmpty()) { results->hide(); return; }
		buildIndex();
		const auto expanded = expandQuery(query);
		QVector<QPair<int, int>> matches;
		for (int index = 0; index < entries.size(); ++index) {
			const int rank = score(query, expanded, entries[index]);
			if (rank > 0) matches.push_back({rank, index});
		}
		std::stable_sort(matches.begin(), matches.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		results->clear();
		for (int index = 0; index < matches.size() && index < 10; ++index) {
			const int entryIndex = matches[index].second;
			const auto &entry = entries[entryIndex];
			QString context = entry.category;
			if (!entry.section.isEmpty() && entry.section != context) context += QStringLiteral(" › ") + entry.section;
			auto *item = new QListWidgetItem(entry.title == context ? entry.title : entry.title + QStringLiteral("  ·  ") + context, results);
			item->setData(Qt::UserRole, entryIndex);
			item->setToolTip(entry.title + QStringLiteral("\n") + context);
		}
		if (matches.isEmpty()) {
			auto *item = new QListWidgetItem(QTStr("Basic.Settings.Search.NoResults"), results);
			item->setFlags(Qt::NoItemFlags);
		} else {
			results->setCurrentRow(0);
		}
		placeResults();
		results->show();
		results->raise();
	}

	void reveal(const Entry &entry)
	{
		QPointer<QWidget> target = entry.target;
		if (!target || entry.page < 0 || entry.page >= ui->listWidget->count()) return;
		ui->listWidget->setCurrentRow(entry.page);
		if (target == ui->fpsType && entry.choice >= 0 && entry.choice < ui->fpsType->count())
			ui->fpsType->setCurrentIndex(entry.choice);
		QVector<QPair<QWidget *, QWidget *>> layers;
		for (auto *child = target.data(); child && child != ui->settingsPages->widget(entry.page); child = child->parentWidget())
			if (auto *parent = child->parentWidget()) layers.push_back({parent, child});
		for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
			auto *container = it->first;
			auto *child = it->second;
			if (auto *tabs = qobject_cast<QTabWidget *>(container)) {
				if (tabs->indexOf(child) >= 0) tabs->setCurrentWidget(child);
			} else if (auto *stack = qobject_cast<QStackedWidget *>(container)) {
				if (auto *tabs = qobject_cast<QTabWidget *>(stack->parentWidget())) {
					if (tabs->indexOf(child) >= 0) tabs->setCurrentWidget(child);
				} else if (stack == ui->outputModePages) {
					ui->outputMode->setCurrentIndex(stack->indexOf(child));
				} else if (stack == ui->fpsTypes) {
					ui->fpsType->setCurrentIndex(stack->indexOf(child));
				} else if (stack != ui->settingsPages && stack->currentWidget() != child) {
					return;
				}
			}
		}
		QTimer::singleShot(0, this, [this, target] {
			if (!target) return;
			QWidget *destination = target;
			if (target == ui->settingsPages->currentWidget()) {
				for (auto *group : target->findChildren<QGroupBox *>()) {
					if (group->isVisibleTo(dialog) && !group->title().isEmpty()) { destination = group; break; }
				}
			}
			for (auto *parent = destination->parentWidget(); parent; parent = parent->parentWidget())
				if (auto *area = qobject_cast<QScrollArea *>(parent)) { area->ensureWidgetVisible(destination, 20, 20); break; }
			if (destination->isVisibleTo(dialog)) highlight(destination);
			if (target->isVisible() && target->isEnabled() && target->focusPolicy() != Qt::NoFocus)
				target->setFocus(Qt::ShortcutFocusReason);
		});
	}

	void activate(QListWidgetItem *item)
	{
		if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;
		bool valid = false;
		const int index = item->data(Qt::UserRole).toInt(&valid);
		if (!valid || index < 0 || index >= entries.size()) return;
		const Entry entry = entries[index];
		results->hide();
		reveal(entry);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (object == dialog && event->type() == QEvent::Resize && results->isVisible()) placeResults();
		if (object == field && event->type() == QEvent::KeyPress) {
			auto *key = static_cast<QKeyEvent *>(event);
			if (key->key() == Qt::Key_Escape && !field->text().isEmpty()) { field->clear(); results->hide(); return true; }
			if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
				if (results->isVisible()) { activate(results->currentItem()); return true; }
			} else if (results->isVisible() && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
				const int direction = key->key() == Qt::Key_Down ? 1 : -1;
				results->setCurrentRow(std::clamp(results->currentRow() + direction, 0, results->count() - 1));
				return true;
			}
		}
		if (event->type() == QEvent::MouseButtonPress && results->isVisible()) {
			auto *widget = qobject_cast<QWidget *>(object);
			if (widget && widget != field && widget != results && !results->isAncestorOf(widget) &&
			    !field->isAncestorOf(widget)) results->hide();
		}
		return QObject::eventFilter(object, event);
	}

public:
	Controller(OBSBasicSettings *owner, Ui::OBSBasicSettings *settingsUi)
		: QObject(owner), dialog(owner), ui(settingsUi)
	{
		auto *sidebar = new QWidget(dialog);
		sidebar->setObjectName(QStringLiteral("settingsSidebar"));
		sidebar->setMaximumWidth(180);
		auto *column = new QVBoxLayout(sidebar);
		column->setContentsMargins(0, 0, 0, 0);
		column->setSpacing(5);
		field = new QLineEdit(sidebar);
		field->setObjectName(QStringLiteral("settingsSearch"));
		field->setPlaceholderText(QTStr("Basic.Settings.Search.Placeholder"));
		field->setAccessibleName(QTStr("Basic.Settings.Search.Placeholder"));
		field->setMaxLength(120);
		field->installEventFilter(this);
		auto *findShortcut = new QShortcut(QKeySequence::Find, dialog);
		connect(findShortcut, &QShortcut::activated, this, [this] {
			field->setFocus(Qt::ShortcutFocusReason);
			field->selectAll();
		});
		column->addWidget(field);
		auto *old = ui->horizontalLayout->takeAt(0);
		delete old;
		ui->listWidget->setParent(sidebar);
		column->addWidget(ui->listWidget, 1);
		ui->horizontalLayout->insertWidget(0, sidebar);
		results = new QListWidget(dialog);
		results->setObjectName(QStringLiteral("settingsSearchResults"));
		results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		results->hide();
		updateTimer = new QTimer(this);
		updateTimer->setSingleShot(true);
		updateTimer->setInterval(70);
		connect(updateTimer, &QTimer::timeout, this, [this] { updateResults(); });
		connect(field, &QLineEdit::textChanged, this, [this](const QString &text) {
			if (text.trimmed().isEmpty()) { updateTimer->stop(); results->hide(); }
			else updateTimer->start();
		});
		connect(results, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { activate(item); });
		connect(results, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { activate(item); });
		connect(ui->listWidget, &QListWidget::currentRowChanged, this, [this] { results->hide(); });
		dialog->installEventFilter(this);
		qApp->installEventFilter(this);
	}

	~Controller() override { if (qApp) qApp->removeEventFilter(this); }
};
} // namespace OBSSettingsSearch
