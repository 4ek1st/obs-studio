// Include inside OBSWebView2, only in the disposable portable integration build.
// The translation unit needs QDateTime, QFileInfo, QJsonParseError, QSaveFile,
// QStringList, <functional>, <vector> and the usual OBS/Qt frontend headers.
#ifndef OBS_WEBVIEW2_INTEGRATION_TESTS
#error "Persistence checks require the disposable OBS integration build"
#endif

// Keep the comparator independent of OBS so its contract can also run in a
// small QtCore-only executable, before the full two-process integration run.
// PERSISTENCE_COMPARATOR_BEGIN
static QString persistencePointerToken(QString value)
{
	return value.replace("~", "~0").replace("/", "~1");
}

static void persistenceDifferences(const QJsonValue &expected, const QJsonValue &actual,
				   const QString &path, QJsonArray &differences)
{
	auto mismatch = [&](const char *reason) {
		QJsonObject difference{{"path", path}, {"reason", QString::fromUtf8(reason)}};
		if (expected.isUndefined())
			difference.insert("expectedMissing", true);
		else
			difference.insert("expected", expected);
		if (actual.isUndefined())
			difference.insert("actualMissing", true);
		else
			difference.insert("actual", actual);
		differences.append(difference);
	};
	if (expected.type() != actual.type()) {
		mismatch(expected.isUndefined() ? "unexpected" : actual.isUndefined() ? "missing" : "type");
		return;
	}
	if (expected.isObject()) {
		const auto left = expected.toObject();
		const auto right = actual.toObject();
		QStringList keys = left.keys();
		for (const auto &key : right.keys())
			if (!left.contains(key))
				keys.append(key);
		keys.sort();
		for (const auto &key : keys)
			persistenceDifferences(left.value(key), right.value(key), path + "/" + persistencePointerToken(key), differences);
		return;
	}
	if (expected.isArray()) {
		const auto left = expected.toArray();
		const auto right = actual.toArray();
		for (qsizetype index = 0; index < std::max(left.size(), right.size()); ++index)
			persistenceDifferences(index < left.size() ? left.at(index) : QJsonValue(QJsonValue::Undefined),
				index < right.size() ? right.at(index) : QJsonValue(QJsonValue::Undefined),
				path + "/" + QString::number(index), differences);
		return;
	}
	if (expected == actual)
		return;
	if (expected.isDouble() && path.contains("/items/") && path.contains("/transform/")) {
		const double left = expected.toDouble();
		const double right = actual.toDouble();
		// OBS saves relative float coordinates and reconstructs absolute values.
		// Preserve sub-pixel accuracy while tolerating that float round trip.
		const double tolerance = std::max(0.0001, std::max(std::abs(left), std::abs(right)) * 0.000001);
		if (std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance)
			return;
	}
	mismatch("value");
}

static void persistenceComparatorChecks(const std::function<void(bool, const char *)> &check)
{
	auto compare = [](const QJsonValue &expected, const QJsonValue &actual, const QString &path = "/state") {
		QJsonArray differences;
		persistenceDifferences(expected, actual, path, differences);
		return differences;
	};
	QJsonObject first{{"z", 2}, {"a", QJsonObject{{"uuid", "source-a"}, {"enabled", true}}}};
	QJsonObject reordered{{"a", QJsonObject{{"enabled", true}, {"uuid", "source-a"}}}, {"z", 2}};
	check(compare(first, reordered).isEmpty(), "Persistence comparison ignores object enumeration order");
	check(!compare(QJsonArray{"source-a", "source-b"}, QJsonArray{"source-b", "source-a"}).isEmpty(),
	      "Persistence comparison detects meaningful scene, item and filter order changes");
	check(!compare(QJsonObject{{"source-a", 1}}, QJsonObject{{"source-b", 1}}).isEmpty(),
	      "Persistence comparison detects replaced source identities");
	check(!compare(QJsonObject{{"visible", false}}, QJsonObject{{"visible", true}}).isEmpty(),
	      "Persistence comparison detects visibility changes");
	check(!compare(QJsonObject{{"locked", true}}, QJsonObject{{"locked", false}}).isEmpty(),
	      "Persistence comparison detects lock changes");
	check(!compare(QJsonObject{{"width", 854}}, QJsonObject{{"width", 855}}).isEmpty(),
	      "Persistence comparison detects source property changes");
	check(!compare(true, 1).isEmpty(), "Persistence comparison preserves JSON value types");
	check(!compare(QJsonObject{{"optional", QJsonValue(QJsonValue::Null)}}, QJsonObject{}).isEmpty(),
	      "Persistence comparison distinguishes missing settings from explicit null");
	check(compare(123.0, 123.00001, "/state/sources/a/items/0/transform/position/x").isEmpty(),
	      "Persistence comparison accepts tiny float round-trip error in transforms");
	check(!compare(123.0, 124.0, "/state/sources/a/items/0/transform/position/x").isEmpty(),
	      "Persistence comparison detects changed transforms");
	check(!compare(0.5, 0.50001, "/state/sources/a/settings/gamma").isEmpty(),
	      "Persistence comparison keeps source settings numerically exact");
	const auto escaped = compare(QJsonObject{{"a/b~c", 1}}, QJsonObject{{"a/b~c", 2}});
	check(escaped.size() == 1 && escaped.at(0).toObject().value("path") == "/state/a~1b~0c",
	      "Persistence differences identify precise escaped JSON paths");
	check(!compare(QJsonArray{1, 2}, QJsonArray{1}).isEmpty(),
	      "Persistence comparison detects missing scene items or filters");
}
// PERSISTENCE_COMPARATOR_END

static bool persistenceAllowed()
{
	const auto arguments = QCoreApplication::arguments();
	return arguments.contains("--portable") && arguments.contains("--webview2-self-test") &&
	       arguments.contains("--only-bundled-plugins");
}

static QString persistencePath(const char *name)
{
	BPtr<char> root = GetAppConfigPathPtr("obs-studio");
	return root && *root ? QDir(QString::fromUtf8(root.Get())).filePath(QString::fromUtf8(name)) : QString();
}

static QJsonObject persistenceData(obs_data_t *data, const QString &label, QJsonArray &errors)
{
	if (!data) {
		errors.append(label + ": missing native settings");
		return {};
	}
	const char *json = obs_data_get_json_with_defaults(data);
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(json ? QByteArray(json) : QByteArray(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		errors.append(label + ": " + parseError.errorString());
		return {};
	}
	return document.object();
}

static QJsonObject persistenceTransform(obs_sceneitem_t *item)
{
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_get_crop(item, &crop);
	auto vector = [](const vec2 &value) { return QJsonObject{{"x", value.x}, {"y", value.y}}; };
	return {{"position", vector(info.pos)}, {"rotation", info.rot}, {"scale", vector(info.scale)},
		{"alignment", int(info.alignment)}, {"boundsType", int(info.bounds_type)},
		{"boundsAlignment", int(info.bounds_alignment)}, {"bounds", vector(info.bounds)},
		{"cropToBounds", info.crop_to_bounds},
		{"crop", QJsonObject{{"left", int(crop.left)}, {"top", int(crop.top)},
				     {"right", int(crop.right)}, {"bottom", int(crop.bottom)}}}};
}

struct PersistenceCapture {
	QJsonObject sources;
	QJsonArray errors;
	int itemCount = 0;
	int filterCount = 0;
	int groupCount = 0;
	int hiddenItems = 0;
	int lockedItems = 0;
	int transformedItems = 0;
};

static QJsonObject persistenceSource(obs_source_t *source, PersistenceCapture &capture)
{
	const auto uuid = SourceId(source);
	const bool sceneSource = obs_source_is_scene(source) || obs_source_is_group(source);
	// Refresh source-owned save data. Scene items are read from getters below,
	// avoiding legacy group-backup entries in the scene serialization format.
	obs_source_save(source);
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataAutoRelease privateSettings = obs_source_get_private_settings(source);
	auto data = persistenceData(settings, uuid + "/settings", capture.errors);
	if (sceneSource)
		data.remove("items");
	QJsonObject result{{"uuid", uuid}, {"name", QString::fromUtf8(obs_source_get_name(source))},
		{"id", QString::fromUtf8(obs_source_get_unversioned_id(source))},
		{"versionedId", QString::fromUtf8(obs_source_get_id(source))}, {"type", int(obs_source_get_type(source))},
		{"enabled", obs_source_enabled(source)}, {"flags", double(obs_source_get_flags(source))},
		{"settings", data}, {"privateSettings", persistenceData(privateSettings, uuid + "/privateSettings", capture.errors)}};
	std::vector<OBSSource> filters;
	obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *param) {
		static_cast<std::vector<OBSSource> *>(param)->emplace_back(filter);
	}, &filters);
	QJsonArray filterStates;
	for (const auto &filter : filters) {
		++capture.filterCount;
		filterStates.append(persistenceSource(filter, capture));
	}
	result.insert("filters", filterStates);
	if (sceneSource) {
		capture.groupCount += obs_source_is_group(source);
		auto *scene = obs_source_is_group(source) ? obs_group_from_source(source) : obs_scene_from_source(source);
		std::vector<OBSSceneItem> items;
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		}, &items);
		QJsonArray itemStates;
		for (const auto &item : items) {
			++capture.itemCount;
			capture.hiddenItems += !obs_sceneitem_visible(item);
			capture.lockedItems += obs_sceneitem_locked(item);
			const auto transform = persistenceTransform(item);
			const auto position = transform.value("position").toObject();
			const auto scale = transform.value("scale").toObject();
			capture.transformedItems += transform.value("rotation").toDouble() != 0.0 ||
				position.value("x").toDouble() != 0.0 || position.value("y").toDouble() != 0.0 ||
				scale.value("x").toDouble() != 1.0 || scale.value("y").toDouble() != 1.0;
			OBSDataAutoRelease itemSettings = obs_sceneitem_get_private_settings(item);
			itemStates.append(QJsonObject{{"id", QString::number(obs_sceneitem_get_id(item))},
				{"sourceUuid", SourceId(obs_sceneitem_get_source(item))}, {"order", obs_sceneitem_get_order_position(item)},
				{"visible", obs_sceneitem_visible(item)}, {"locked", obs_sceneitem_locked(item)},
				{"group", obs_sceneitem_is_group(item)}, {"transform", transform},
				{"scaleFilter", int(obs_sceneitem_get_scale_filter(item))},
				{"blendingMethod", int(obs_sceneitem_get_blending_method(item))},
				{"blendingMode", int(obs_sceneitem_get_blending_mode(item))},
				{"privateSettings", persistenceData(itemSettings, uuid + "/itemPrivateSettings", capture.errors)}});
		}
		result.insert("items", itemStates);
	}
	return result;
}

QJsonObject persistenceSnapshot(QJsonArray &errors, QJsonObject &coverage) const
{
	PersistenceCapture capture;
	std::vector<OBSSource> sources;
	auto collect = [](void *param, obs_source_t *source) {
		if (!obs_source_removed(source))
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		return true;
	};
	// Hold references while inspecting, outside libobs enumeration locks.
	obs_enum_sources(collect, &sources);
	obs_enum_scenes(collect, &sources);
	for (const auto &source : sources) {
		const auto uuid = SourceId(source);
		if (uuid.isEmpty()) {
			capture.errors.append("A native source has no persistent UUID");
			continue;
		}
		if (!capture.sources.contains(uuid))
			capture.sources.insert(uuid, persistenceSource(source, capture));
	}
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	QJsonArray sceneOrder;
	for (size_t index = 0; index < scenes.sources.num; ++index) {
		auto *source = scenes.sources.array[index];
		const auto uuid = SourceId(source);
		sceneOrder.append(uuid);
		if (!capture.sources.contains(uuid))
			capture.sources.insert(uuid, persistenceSource(source, capture));
	}
	obs_frontend_source_list_free(&scenes);
	BPtr<char> collection = obs_frontend_get_current_scene_collection();
	BPtr<char> profile = obs_frontend_get_current_profile();
	BPtr<char> profilePath = obs_frontend_get_current_profile_path();
	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	OBSSourceAutoRelease previewScene = obs_frontend_get_current_preview_scene();
	auto *config = static_cast<OBSBasic *>(main)->Config();
	QJsonObject profileSettings;
	const std::pair<const char *, const char *> keys[] = {
		{"Output", "Mode"}, {"Output", "FilenameFormatting"}, {"Output", "OverwriteIfExists"},
		{"SimpleOutput", "StreamEncoder"}, {"SimpleOutput", "RecEncoder"}, {"SimpleOutput", "RecQuality"},
		{"SimpleOutput", "RecFormat2"}, {"SimpleOutput", "RecTracks"}, {"SimpleOutput", "FilePath"},
		{"Video", "BaseCX"}, {"Video", "BaseCY"}, {"Video", "OutputCX"}, {"Video", "OutputCY"},
		{"Video", "FPSType"}, {"Video", "FPSCommon"}, {"Video", "FPSInt"}, {"Video", "FPSNum"}, {"Video", "FPSDen"},
		{"Video", "ScaleType"}, {"Video", "ColorFormat"}, {"Video", "ColorSpace"}, {"Video", "ColorRange"},
		{"Audio", "SampleRate"}, {"Audio", "ChannelSetup"}};
	for (const auto &key : keys) {
		auto section = profileSettings.value(key.first).toObject();
		const char *value = config_get_string(config, key.first, key.second);
		section.insert(key.second, value ? QJsonValue(QString::fromUtf8(value)) : QJsonValue(QJsonValue::Null));
		profileSettings.insert(key.first, section);
	}
	QJsonObject context{{"collection", QString::fromUtf8(collection.Get() ? collection.Get() : "")},
		{"collectionFile", QString::fromUtf8(config_get_string(obs_frontend_get_user_config(), "Basic", "SceneCollectionFile"))},
		{"profile", QString::fromUtf8(profile.Get() ? profile.Get() : "")},
		{"profileDirectory", QFileInfo(QString::fromUtf8(profilePath.Get() ? profilePath.Get() : "")).fileName()},
		{"currentScene", SourceId(currentScene)}, {"previewScene", SourceId(previewScene)},
		{"studioMode", obs_frontend_preview_program_mode_active()}};
	if (context.value("collection").toString().isEmpty() || context.value("profile").toString().isEmpty() ||
	    sceneOrder.isEmpty() || capture.sources.isEmpty())
		capture.errors.append("Native persistence state must contain a selected collection, profile, scenes and sources");
	errors = capture.errors;
	coverage = {{"scenes", sceneOrder.size()}, {"sources", capture.sources.size()}, {"sceneItems", capture.itemCount},
		{"groups", capture.groupCount}, {"filters", capture.filterCount}, {"hiddenItems", capture.hiddenItems},
		{"lockedItems", capture.lockedItems}, {"transformedItems", capture.transformedItems}};
	return {{"context", context}, {"profileSettings", profileSettings}, {"sceneOrder", sceneOrder}, {"sources", capture.sources}};
}

static bool writePersistenceJson(const QString &path, const QJsonObject &value)
{
	if (path.isEmpty())
		return false;
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	const auto json = QJsonDocument(value).toJson();
	return file.write(json) == json.size() && file.commit();
}

void savePersistenceFixture(const std::function<void(bool, const char *)> &check)
{
	if (!persistenceAllowed() || QCoreApplication::arguments().contains("--webview2-persistence-test")) {
		check(false, "Persistence fixture capture requires the original disposable portable self-test process");
		return;
	}
	persistenceComparatorChecks(check);
	auto *basic = static_cast<OBSBasic *>(main);
	const bool maySave = !basic->SavingDisabled() && !basic->Active() && !obs_frontend_recording_active();
	check(maySave, "Persistence capture starts after native outputs stop and scene saving is enabled");
	if (!maySave)
		return;
	basic->SaveProject();
	basic->SaveProjectDeferred();
	const bool savedProfile = config_save_safe(basic->Config(), "tmp", "bak") == CONFIG_SUCCESS;
	const bool savedSelection = config_save_safe(obs_frontend_get_user_config(), "tmp", "bak") == CONFIG_SUCCESS;
	check(savedProfile && savedSelection, "Native profile and active collection selection are saved before restart");
	QJsonArray errors;
	QJsonObject coverage;
	const auto state = persistenceSnapshot(errors, coverage);
	check(errors.isEmpty(), "Native source, scene, filter and transform state can be captured for restart");
	const auto context = state.value("context").toObject();
	const auto sceneFile = persistencePath(("basic/scenes/" + context.value("collectionFile").toString()).toUtf8().constData());
	QFile collectionFile(sceneFile);
	QJsonParseError parseError;
	const bool readable = collectionFile.open(QIODevice::ReadOnly);
	const auto savedCollection = readable ? QJsonDocument::fromJson(collectionFile.readAll(), &parseError) : QJsonDocument();
	const bool savedScene = readable && parseError.error == QJsonParseError::NoError && savedCollection.isObject() &&
		savedCollection.object().value("name") == context.value("collection");
	check(savedScene, "Native scene collection exists as valid JSON in the disposable portable profile");
	const QJsonObject manifest{{"schema", 1}, {"valid", savedProfile && savedSelection && savedScene && errors.isEmpty()},
		{"createdAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
		{"capturePid", QString::number(QCoreApplication::applicationPid())},
		{"coverage", coverage}, {"captureErrors", errors}, {"state", state},
		{"transformTolerance", QJsonObject{{"absolute", 0.0001}, {"relative", 0.000001}}}};
	check(writePersistenceJson(persistencePath("webview2-persistence-fixture.json"), manifest),
	      "Persistence fixture manifest is saved under the disposable portable config");
}

void runPersistenceChecks()
{
	if (!persistenceAllowed() || !QCoreApplication::arguments().contains("--webview2-persistence-test")) {
		blog(LOG_ERROR, "[WebView2 persistence test] Explicit disposable restart flags are required");
		return;
	}
	QTimer::singleShot(500, this, [this] {
		QJsonArray checks, differences, captureErrors;
		QJsonObject coverage;
		bool passed = true;
		auto check = [&checks, &passed](bool value, const char *name) {
			passed &= value;
			checks.append(QJsonObject{{"name", QString::fromUtf8(name)}, {"passed", value}});
			blog(value ? LOG_INFO : LOG_ERROR, "[WebView2 persistence test] %s: %s", value ? "PASS" : "FAIL", name);
		};
		persistenceComparatorChecks(check);
		check(isVisible() && !main->isVisible() && static_cast<OBSBasic *>(main)->FrontendWindow() == this,
		      "Restart restores WebView2 as the visible owning frontend");
		check(!static_cast<OBSBasic *>(main)->Active() && !obs_frontend_recording_active(),
		      "Persistence restart does not start native outputs");
		QFile fixtureFile(persistencePath("webview2-persistence-fixture.json"));
		const bool readable = fixtureFile.open(QIODevice::ReadOnly);
		QJsonParseError parseError;
		const auto document = readable ? QJsonDocument::fromJson(fixtureFile.readAll(), &parseError) : QJsonDocument();
		const auto fixture = document.object();
		const bool valid = readable && parseError.error == QJsonParseError::NoError && document.isObject() &&
			fixture.value("schema") == 1 && fixture.value("valid").toBool() && fixture.value("state").isObject();
		check(valid, "Restart reads a valid persistence manifest from the completed original process");
		check(valid && fixture.value("capturePid").toString() != QString::number(QCoreApplication::applicationPid()),
		      "Persistence validation runs in a separate OBS process");
		const auto actual = persistenceSnapshot(captureErrors, coverage);
		check(captureErrors.isEmpty(), "Reconstructed native persistence state is readable after restart");
		const auto expected = fixture.value("state").toObject();
		if (valid) {
			persistenceDifferences(expected, actual, "/state", differences);
			for (const auto &key : QStringList{"context", "profileSettings", "sceneOrder", "sources"}) {
				QJsonArray category;
				persistenceDifferences(expected.value(key), actual.value(key), "/state/" + key, category);
				const auto label = ("Native " + key + " persists across a full OBS restart").toUtf8();
				check(category.isEmpty(), label.constData());
			}
			check(differences.isEmpty(), "Scene/source identities, settings, filters, order, visibility, locks and transforms persist");
		}
		QJsonObject report{{"schema", 1}, {"passed", passed}, {"checks", checks}, {"differences", differences},
			{"captureErrors", captureErrors}, {"coverage", coverage}, {"expectedCoverage", fixture.value("coverage")},
			{"expected", expected}, {"actual", actual}, {"capturePid", fixture.value("capturePid")},
			{"restartPid", QString::number(QCoreApplication::applicationPid())},
			{"checkedAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
		const bool saved = writePersistenceJson(persistencePath("webview2-persistence.json"), report);
		check(saved, "Detailed persistence report is saved under the disposable portable config");
		blog(passed ? LOG_INFO : LOG_ERROR, "[WebView2 persistence test] %s, differences=%lld",
		     passed ? "PASS" : "FAIL", static_cast<long long>(differences.size()));
		config_set_bool(obs_frontend_get_user_config(), "General", "ConfirmOnExit", false);
		QTimer::singleShot(0, this, &QWidget::close);
	});
}
