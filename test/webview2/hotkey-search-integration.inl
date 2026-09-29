// Actual Settings > Hotkeys workflow in a disposable portable OBS test run.
#include <settings/OBSBasicSettings.hpp>
#include <QEventLoop>
#include <QFormLayout>
#include <QScrollArea>

namespace {
void RunHotkeySearchChecks(OBSBasic *main, std::function<void(bool, const char *)> check, std::function<void()> done)
{
	struct State {
		QPointer<QDialog> settings;
		QPointer<QTimer> poll;
		QElapsedTimer elapsed;
		int phase = 0;
	};
	auto state = std::make_shared<State>();
	state->elapsed.start();
	const int requestedRows = qEnvironmentVariableIntValue("OBS_WEBVIEW2_HOTKEY_FIXTURE_ROWS");
	const int fixtureRows = requestedRows > 0 ? std::min(requestedRows, 1000) : 160;
	for (int index = 0; index < fixtureRows; ++index) {
		const auto name = QStringLiteral("WebView2.HotkeySearchFixture.%1").arg(index).toUtf8();
		const auto description = QStringLiteral("Search fixture action %1").arg(index).toUtf8();
		obs_hotkey_register_frontend(name.constData(), description.constData(),
			[](void *, obs_hotkey_id, obs_hotkey_t *, bool) {}, nullptr);
	}
	auto *poll = new QTimer(main);
	state->poll = poll;
	poll->setInterval(100);
	QObject::connect(poll, &QTimer::timeout, main, [main, state, fixtureRows, check, done] {
		if (!state->settings) {
			for (auto *widget : QApplication::topLevelWidgets()) {
				if (auto *dialog = qobject_cast<QDialog *>(widget);
				    dialog && dialog->isVisible() && dialog->inherits("OBSBasicSettings")) {
					state->settings = dialog;
					break;
				}
			}
		}
		if (state->elapsed.elapsed() > 30000) {
			check(false, "Hotkey Settings became ready within 30 seconds");
			state->poll->stop();
			if (state->settings) state->settings->close();
			done();
			return;
		}
		if (!state->settings) return;
		if (state->phase == 0) {
			auto *categories = state->settings->findChild<QListWidget *>(QStringLiteral("listWidget"));
			if (!categories || categories->count() < 7) return;
			categories->setCurrentRow(6);
			state->phase = 1;
			return;
		}
		auto *form = state->settings->findChild<QFormLayout *>(QStringLiteral("hotkeyFormLayout"));
		auto *search = state->settings->findChild<QLineEdit *>(QStringLiteral("hotkeyFilterSearch"));
		auto *area = state->settings->findChild<QScrollArea *>(QStringLiteral("hotkeyScrollArea"));
		if (!form || !form->rowCount() || !search || !area) return;
		auto *container = form->itemAt(0)->widget();
		auto *rows = container ? qobject_cast<QFormLayout *>(container->layout()) : nullptr;
		if (!rows || !rows->rowCount()) return;
		state->poll->stop();
		check(rows->rowCount() >= fixtureRows, "Real Settings Hotkeys page loaded all fixture rows");
		OBSWeb::QtDialogBridge bridge(state->settings);
		const auto initial = bridge.snapshot();
		QString searchId;
		for (const auto value : initial.value("nodes").toArray()) {
			const auto node = value.toObject();
			if (node.value("name") == QStringLiteral("hotkeyFilterSearch")) searchId = node.value("id").toString();
		}
		check(!searchId.isEmpty(), "Hotkey search is exposed by the actual dialog bridge");
		QString label;
		for (auto *widget : container->findChildren<QWidget *>()) {
			if (widget->inherits("OBSHotkeyLabel")) {
				label = widget->property("fullName").toString().trimmed();
				if (label.size() >= 4) break;
			}
		}
		check(label.size() >= 4, "Hotkey search has a real label to query");
		const int count = std::min(6, int(label.size()));
		QJsonArray samples;
		QString error;
		bool scrollRangeMatchesContent = true;
		bool inputRemainsResponsive = true;
		auto waitForFilter = [] {
			QEventLoop loop;
			QTimer::singleShot(100, &loop, &QEventLoop::quit);
			loop.exec();
		};
		// Allow the first full Hotkeys page paint to finish before measuring search.
		waitForFilter();
		waitForFilter();
		waitForFilter();
		for (int i = 1; i <= count * 2 + 1; ++i) {
			const int length = i <= count ? i : std::max(0, count * 2 - i);
			const QString query = label.left(length);
			QElapsedTimer clock;
			clock.start();
			const bool accepted = bridge.execute("dialog.input", {{"id", searchId}, {"value", query}}, error);
			const double inputMs = clock.nsecsElapsed() / 1e6;
			if (inputMs >= 50) inputRemainsResponsive = false;
			clock.restart();
			waitForFilter();
			const double settleMs = clock.nsecsElapsed() / 1e6;
			clock.restart();
			const auto snapshot = bridge.snapshot();
			const double snapshotMs = clock.nsecsElapsed() / 1e6;
			const int contentHint = area->widget()->sizeHint().height();
			const int viewportHeight = area->viewport()->height();
			if (contentHint > viewportHeight + 50 && area->verticalScrollBar()->maximum() == 0)
				scrollRangeMatchesContent = false;
			samples.append(QJsonObject{{"query", query}, {"accepted", accepted}, {"inputMs", inputMs},
				{"settleMs", settleMs},
				{"snapshotMs", snapshotMs}, {"nodes", snapshot.value("nodes").toArray().size()},
				{"scrollMaximum", area->verticalScrollBar()->maximum()}, {"contentHint", contentHint},
				{"viewportHeight", viewportHeight}});
			if (!accepted) break;
		}
		check(inputRemainsResponsive, "Typing in Hotkeys search never blocks the UI for 50 ms");
		check(scrollRangeMatchesContent, "Scroll range updates with Hotkeys content after every edit");
		search->setText(QStringLiteral("___No_Matching_Hotkey_90837___"));
		waitForFilter();
		int orphanHeaders = 0;
		for (int row = 0; row < rows->rowCount(); ++row) {
			const auto *item = rows->itemAt(row, QFormLayout::SpanningRole);
			if (item && qobject_cast<QLabel *>(item->widget()) && !item->widget()->isHidden()) ++orphanHeaders;
		}
		check(orphanHeaders == 0, "No unmatched source or scene headings remain in search results");
		search->setText(QString());
		waitForFilter();
		const bool restored = search->text().isEmpty() && rows->rowCount() >= 10 && area->verticalScrollBar()->maximum() > 0;
		check(restored, "Clearing search restores original hotkey rows and scrolling");
		const int middle = area->verticalScrollBar()->maximum() / 2;
		QString areaId;
		for (const auto value : bridge.snapshot().value("nodes").toArray()) {
			const auto node = value.toObject();
			if (node.value("name") == QStringLiteral("hotkeyScrollArea")) areaId = node.value("id").toString();
		}
		check(!areaId.isEmpty() && bridge.execute("dialog.scroll", {{"id", areaId}, {"value", middle}}, error) &&
		      area->verticalScrollBar()->value() == middle, "Hotkey scrollbar reaches the middle through the dialog bridge");
		search->setText(label.left(count));
		waitForFilter();
		check(area->verticalScrollBar()->value() == 0, "Filtering from a scrolled position returns to the top");
		const auto output = qEnvironmentVariable("OBS_WEBVIEW2_TEST_ARTIFACTS");
		QDir().mkpath(output);
		QFile file(QDir(output).filePath("hotkey-search-samples.json"));
		if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(samples).toJson());
		QPointer<QDialog> settings = state->settings;
		QTimer::singleShot(350, main, [main, settings, output, check, done] {
			auto *web = settings ? settings->findChild<WebView2Widget *>("obsWebView2DialogSurface", Qt::FindDirectChildrenOnly) : nullptr;
			check(web && web->isVisible(), "Actual WebView2 surface renders Hotkeys search");
			if (!web) {
				if (settings && !qEnvironmentVariableIsSet("OBS_WEBVIEW2_HOLD_SETTINGS")) settings->close();
				done();
				return;
			}
			OBSWeb::QtDialogBridge finalBridge(settings);
			QFile snapshot(QDir(output).filePath("hotkey-search-state.json"));
			if (snapshot.open(QIODevice::WriteOnly)) snapshot.write(QJsonDocument(finalBridge.snapshot()).toJson());
			web->hide();
			settings->grab().save(QDir(output).filePath("hotkey-search-native.png"));
			web->show();
			web->raise();
			web->capturePreview(QDir(output).filePath("hotkey-search.png"), [settings, output, check, done](bool saved) {
				check(saved, "Hotkey search screenshot captured from WebView2");
				if (settings && qEnvironmentVariableIsSet("OBS_WEBVIEW2_SCROLL_SWEEP")) {
					QJsonArray pages;
					auto *categories = settings->findChild<QListWidget *>(QStringLiteral("listWidget"));
					if (categories) for (int index = 0; index < categories->count(); ++index) {
						categories->setCurrentRow(index);
						QEventLoop loop;
						QTimer::singleShot(100, &loop, &QEventLoop::quit);
						loop.exec();
						OBSWeb::QtDialogBridge probe(settings);
						const auto beforeState = probe.snapshot();
						QJsonArray scrolls;
						for (const auto value : beforeState.value("nodes").toArray()) {
							const auto node = value.toObject();
							if (node.value("type") == QStringLiteral("scroll") ||
							    node.value("type") == QStringLiteral("scrollArea")) scrolls.append(node);
						}
						bool imageChanged = false;
						bool valueChanged = false;
						for (const auto value : scrolls) {
							const auto bar = value.toObject();
							if (bar.value("type") != QStringLiteral("scroll") ||
							    bar.value("maximum").toInt() <= bar.value("minimum").toInt()) continue;
							const int target = bar.value("maximum").toInt();
							QString error;
							const bool accepted = probe.execute("dialog.input", {{"id", bar.value("id")}, {"value", target}}, error);
							QEventLoop updatedLoop;
							QTimer::singleShot(120, &updatedLoop, &QEventLoop::quit);
							updatedLoop.exec();
							for (const auto afterValue : probe.snapshot().value("nodes").toArray()) {
								const auto after = afterValue.toObject();
								if (after.value("id") != bar.value("id")) continue;
								valueChanged = accepted && after.value("value").toInt() == target;
								imageChanged = bar.value("nativeStyle").toObject().value("normal") !=
									       after.value("nativeStyle").toObject().value("normal");
								break;
							}
							const QByteArray name = QStringLiteral("Scrollbar image follows thumb in Settings / %1")
										 .arg(categories->item(index)->text()).toUtf8();
							check(valueChanged && imageChanged, name.constData());
							break;
						}
						pages.append(QJsonObject{{"index", index}, {"category", categories->item(index)->text()},
							{"scrolls", scrolls}, {"valueChanged", valueChanged}, {"imageChanged", imageChanged}});
					}
					QFile sweep(QDir(output).filePath("settings-scroll-sweep.json"));
					if (sweep.open(QIODevice::WriteOnly)) sweep.write(QJsonDocument(pages).toJson());
				}
				if (settings && !qEnvironmentVariableIsSet("OBS_WEBVIEW2_HOLD_SETTINGS")) settings->close();
				done();
			});
		});
	});
	poll->start();
	QMetaObject::invokeMethod(main, "on_action_Settings_triggered", Qt::QueuedConnection);
}
} // namespace
