/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QMimeData>
#ifdef _WIN32
#include <QApplication>
#include <QInputDialog>
#include <QSettings>
#include <QTimer>
#include <util/windows/window-helpers.h>
#include <algorithm>
#include <functional>
#endif
#include <QUrlQuery>

using namespace std;

#ifdef _WIN32
namespace {
class MovedWindowDrop final : public QObject {
	static inline MovedWindowDrop *instance = nullptr;
	QPointer<OBSBasic> main;
	std::function<void(HWND)> onDrop;
	HWINEVENTHOOK hook = nullptr;
	HWND moving = nullptr;
	RECT start{};
	bool pending = false;

	static void CALLBACK event(HWINEVENTHOOK, DWORD type, HWND window, LONG object, LONG child, DWORD, DWORD)
	{
		if (!instance || !window || object != OBJID_WINDOW || child != CHILDID_SELF)
			return;
		instance->handle(type, window);
	}

	void handle(DWORD type, HWND window)
	{
		if (!main || pending || QApplication::activeModalWidget())
			return;
		if (type == EVENT_SYSTEM_MOVESIZESTART) {
			moving = window;
			GetWindowRect(window, &start);
			return;
		}
		if (type != EVENT_SYSTEM_MOVESIZEEND || moving != window)
			return;
		moving = nullptr;
		RECT end{};
		if (!GetWindowRect(window, &end) || (start.left == end.left && start.top == end.top) ||
		    main->isMinimized() || !main->isVisible() || !IsWindowVisible(window) || IsIconic(window) ||
		    GetAncestor(window, GA_ROOT) != window || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
			return;
		DWORD owner = 0;
		GetWindowThreadProcessId(window, &owner);
		if (!owner || owner == GetCurrentProcessId())
			return;
		auto *preview = main->findChild<QWidget *>(QStringLiteral("preview"));
		RECT previewRect{}, overlap{};
		if (!preview || !preview->isVisible() ||
		    !GetWindowRect(reinterpret_cast<HWND>(preview->winId()), &previewRect) ||
		    !IntersectRect(&overlap, &end, &previewRect) ||
		    overlap.right - overlap.left < 48 || overlap.bottom - overlap.top < 48)
			return;
		pending = true;
		QTimer::singleShot(0, this, [this, window] {
			if (main && IsWindow(window)) onDrop(window);
			pending = false;
		});
	}

public:
	MovedWindowDrop(OBSBasic *owner, std::function<void(HWND)> callback)
		: QObject(owner), main(owner), onDrop(std::move(callback))
	{
		setObjectName(QStringLiteral("obsMovedWindowDrop"));
		instance = this;
		hook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, &event,
				       0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
		if (!hook)
			blog(LOG_WARNING, "[Window Drop] Could not watch external window movement: %lu", GetLastError());
	}

	~MovedWindowDrop() override
	{
		if (hook) UnhookWinEvent(hook);
		if (instance == this) instance = nullptr;
	}
};

QString windowDropCaption(HWND window)
{
	dstr title{}, executable{};
	ms_get_window_title(&title, window);
	ms_get_window_exe(&executable, window);
	QString caption = QString::fromUtf8(title.array ? title.array : "");
	if (caption.isEmpty()) caption = QString::fromUtf8(executable.array ? executable.array : "");
	if (caption.isEmpty()) caption = QTStr("WindowDrop.Untitled");
	dstr_free(&title);
	dstr_free(&executable);
	return caption;
}

bool bindMovedWindow(obs_source_t *source, HWND window)
{
	auto *handler = obs_source_get_proc_handler(source);
	if (!handler) return false;
	calldata_t call{};
	calldata_set_ptr(&call, "window", window);
	const bool called = proc_handler_call(handler, "bind_window", &call);
	const bool accepted = called && calldata_bool(&call, "accepted");
	calldata_free(&call);
	return accepted;
}

void fitMovedWindow(obs_sceneitem_t *item)
{
	obs_video_info video{};
	if (!obs_get_video_info(&video)) return;
	obs_transform_info transform{};
	vec2_set(&transform.pos, 0.0f, 0.0f);
	vec2_set(&transform.scale, 1.0f, 1.0f);
	vec2_set(&transform.bounds, float(video.base_width), float(video.base_height));
	transform.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	transform.bounds_type = OBS_BOUNDS_SCALE_INNER;
	transform.bounds_alignment = OBS_ALIGN_CENTER;
	obs_sceneitem_set_info2(item, &transform);
}
} // namespace

void OBSBasic::InstallWindowMoveCapture()
{
	new MovedWindowDrop(this, [this](HWND window) { AddMovedWindowCapture(window); });
}

void OBSBasic::AddMovedWindowCapture(void *nativeWindow)
{
	HWND window = static_cast<HWND>(nativeWindow);
	OBSScene scene = GetCurrentScene();
	if (!scene || !IsWindow(window)) return;
	obs_source_t *existing = nullptr;
	obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *value) {
		auto **found = static_cast<obs_source_t **>(value);
		auto *source = obs_sceneitem_get_source(item);
		if (strcmp(obs_source_get_id(source), "temporary_window_capture") != 0) return true;
		*found = source;
		return false;
	}, &existing);
	if (existing) {
		bindMovedWindow(existing, window);
		return;
	}

	const QString caption = windowDropCaption(window);

	QStringList layers;
	obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *value) {
		auto *names = static_cast<QStringList *>(value);
		names->append(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))));
		return true;
	}, &layers);
	std::reverse(layers.begin(), layers.end());
	QStringList choices{QTStr("WindowDrop.Top")};
	for (const QString &name : layers) choices.append(QTStr("WindowDrop.Below").arg(name));

	QInputDialog dialog(this);
	dialog.setObjectName(QStringLiteral("obsWindowDropLayerDialog"));
	dialog.setWindowTitle(QTStr("WindowDrop.Title"));
	dialog.setLabelText(QTStr("WindowDrop.Label").arg(caption));
	dialog.setComboBoxItems(choices);
	dialog.setComboBoxEditable(false);
	dialog.setOkButtonText(QTStr("AddSource"));
	dialog.setCancelButtonText(QTStr("Cancel"));
	if (dialog.exec() != QDialog::Accepted) return;
	auto *combo = dialog.findChild<QComboBox *>();
	const int position = layers.size() - (combo ? combo->currentIndex() : 0);
	const char *type = obs_get_latest_input_type_id("temporary_window_capture");
	if (!type) return;
	QString base = QTStr("WindowDrop.Source");
	QString name = base;
	for (int n = 2; ; ++n) {
		const QByteArray candidate = name.toUtf8();
		OBSSourceAutoRelease existing = obs_get_source_by_name(candidate.constData());
		if (!existing) break;
		name = base + QStringLiteral(" (%1)").arg(n);
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "cursor", true);
	obs_data_set_bool(settings, "client_area", true);
	const QByteArray nameUtf8 = name.toUtf8();
	OBSSourceAutoRelease source = obs_source_create(type, nameUtf8.constData(), settings, nullptr);
	if (!source) return;
	OBSSceneItem item = obs_scene_add(scene, source);
	if (!item) return;
	obs_sceneitem_set_order_position(item, position);
	fitMovedWindow(item);
	bindMovedWindow(source, window);
	const std::string sceneUuid = obs_source_get_uuid(obs_scene_get_source(scene));
	const std::string sourceUuid = obs_source_get_uuid(source);
	OBSDataAutoRelease saved = obs_save_source(source);
	const std::string savedJson = obs_data_get_json(saved);
	auto undo = [sceneUuid, sourceUuid](const std::string &) {
		OBSSourceAutoRelease created = obs_get_source_by_uuid(sourceUuid.c_str());
		if (created) obs_source_remove(created);
		OBSSourceAutoRelease sceneSource = obs_get_source_by_uuid(sceneUuid.c_str());
		if (sceneSource) OBSBasic::Get()->SetCurrentScene(sceneSource.Get(), true);
	};
	auto redo = [sceneUuid, position](const std::string &data) {
		OBSSourceAutoRelease sceneSource = obs_get_source_by_uuid(sceneUuid.c_str());
		if (!sceneSource) return;
		OBSScene target = obs_scene_from_source(sceneSource);
		OBSDataAutoRelease savedSource = obs_data_create_from_json(data.c_str());
		OBSSourceAutoRelease created = obs_load_source(savedSource);
		if (!created) return;
		OBSSceneItem added = obs_scene_add(target, created);
		if (!added) return;
		obs_sceneitem_set_order_position(added, position);
		fitMovedWindow(added);
		OBSBasic::Get()->SetCurrentScene(sceneSource.Get(), true);
	};
	undo_s.add_action(QTStr("Undo.Add").arg(name), undo, redo, sourceUuid, savedJson);
}
#endif

static const char *textExtensions[] = {"txt", "log", nullptr};

static const char *imageExtensions[] = {"bmp", "gif", "jpeg", "jpg",
#ifdef _WIN32
					"jxr",
#endif
					"png", "tga", "webp", nullptr};

static const char *htmlExtensions[] = {"htm", "html", nullptr};

static const char *mediaExtensions[] = {
	"3ga", "669",  "a52", "aac",  "ac3",   "adt",   "adts",  "aif",  "aifc", "aiff", "amb",  "amr",  "aob", "ape",
	"au",  "awb",  "caf", "dts",  "flac",  "it",    "kar",   "m4a",  "m4b",  "m4p",  "m5p",  "mid",  "mka", "mlp",
	"mod", "mpa",  "mp1", "mp2",  "mp3",   "mpc",   "mpga",  "mus",  "oga",  "ogg",  "oma",  "opus", "qcp", "ra",
	"rmi", "s3m",  "sid", "spx",  "tak",   "thd",   "tta",   "voc",  "vqf",  "w64",  "wav",  "wma",  "wv",  "xa",
	"xm",  "3g2",  "3gp", "3gp2", "3gpp",  "amv",   "asf",   "avi",  "bik",  "crf",  "divx", "drc",  "dv",  "evo",
	"f4v", "flv",  "gvi", "gxf",  "iso",   "m1v",   "m2v",   "m2t",  "m2ts", "m4v",  "mkv",  "mov",  "mp2", "mp2v",
	"mp4", "mp4v", "mpe", "mpeg", "mpeg1", "mpeg2", "mpeg4", "mpg",  "mpv2", "mts",  "mtv",  "mxf",  "mxg", "nsv",
	"nuv", "ogg",  "ogm", "ogv",  "ogx",   "ps",    "rec",   "rm",   "rmvb", "rpl",  "thp",  "tod",  "ts",  "tts",
	"txd", "vob",  "vro", "webm", "wm",    "wmv",   "wtv",   nullptr};

static string GenerateSourceName(const char *base)
{
	string name;
	int inc = 0;

	for (;; inc++) {
		name = base;

		if (inc) {
			name += " (";
			name += to_string(inc + 1);
			name += ")";
		}

		OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());

		if (!source) {
			return name;
		}
	}
}

#ifdef _WIN32
static QString ReadWindowsURLFile(const QString &file)
{
	QSettings iniFile(file, QSettings::IniFormat);
	QVariant url = iniFile.value("InternetShortcut/URL");
	return url.toString();
}
#endif

void OBSBasic::AddDropURL(QUrl url, QString &name, obs_data_t *settings, const obs_video_info &ovi)
{
	QUrlQuery query = QUrlQuery(url.query(QUrl::FullyEncoded));

	int cx = (int)ovi.base_width;
	int cy = (int)ovi.base_height;

	if (query.hasQueryItem("layer-width")) {
		cx = query.queryItemValue("layer-width").toInt();
	}
	if (query.hasQueryItem("layer-height")) {
		cy = query.queryItemValue("layer-height").toInt();
	}
	if (query.hasQueryItem("layer-css")) {
		// QUrl::FullyDecoded does NOT properly decode a
		// application/x-www-form-urlencoded space represented as '+'
		// Thus, this is manually filtered out before QUrl's
		// decoding kicks in again. This is to allow JavaScript's
		// default searchParams.append function to simply append css
		// to the query parameters, which is the intended usecase for this.
		QString fullyEncoded = query.queryItemValue("layer-css", QUrl::FullyEncoded);
		fullyEncoded = fullyEncoded.replace("+", "%20");
		QString decoded = QUrl::fromPercentEncoding(QByteArray::fromStdString(QT_TO_UTF8(fullyEncoded)));
		obs_data_set_string(settings, "css", QT_TO_UTF8(decoded));
	}

	obs_data_set_int(settings, "width", cx);
	obs_data_set_int(settings, "height", cy);

	name = query.hasQueryItem("layer-name") ? query.queryItemValue("layer-name", QUrl::FullyDecoded) : url.host();

	query.removeQueryItem("layer-width");
	query.removeQueryItem("layer-height");
	query.removeQueryItem("layer-name");
	query.removeQueryItem("layer-css");
	url.setQuery(query);

	obs_data_set_string(settings, "url", QT_TO_UTF8(url.url()));
}

void OBSBasic::AddDropSource(const char *data, DropType image)
{
	OBSBasic *main = OBSBasic::Get();
	OBSDataAutoRelease settings = obs_data_create();
	const char *type = nullptr;
	QString name;

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	switch (image) {
	case DropType_RawText:
		obs_data_set_string(settings, "text", data);
#ifdef _WIN32
		type = "text_gdiplus";
#else
		type = "text_ft2_source";
#endif
		break;
	case DropType_Text:
#ifdef _WIN32
		obs_data_set_bool(settings, "read_from_file", true);
		obs_data_set_string(settings, "file", data);
		name = QUrl::fromLocalFile(QString(data)).fileName();
		type = "text_gdiplus";
#else
		obs_data_set_bool(settings, "from_file", true);
		obs_data_set_string(settings, "text_file", data);
		type = "text_ft2_source";
#endif
		break;
	case DropType_Image:
		obs_data_set_string(settings, "file", data);
		name = QUrl::fromLocalFile(QString(data)).fileName();
		type = "image_source";
		break;
	case DropType_Media:
		obs_data_set_string(settings, "local_file", data);
		name = QUrl::fromLocalFile(QString(data)).fileName();
		type = "ffmpeg_source";
		break;
	case DropType_Html:
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", data);
		obs_data_set_int(settings, "width", ovi.base_width);
		obs_data_set_int(settings, "height", ovi.base_height);
		name = QUrl::fromLocalFile(QString(data)).fileName();
		type = "browser_source";
		break;
	case DropType_Url:
		AddDropURL(QUrl(data), name, settings, ovi);
		type = "browser_source";
		break;
	}

	type = obs_get_latest_input_type_id(type);

	if (type == nullptr || !obs_source_get_display_name(type)) {
		return;
	}

	if (name.isEmpty()) {
		name = obs_source_get_display_name(type);
	}
	std::string sourceName = GenerateSourceName(QT_TO_UTF8(name));
	OBSSourceAutoRelease source = obs_source_create(type, sourceName.c_str(), settings, nullptr);
	if (source) {
		OBSDataAutoRelease wrapper = obs_save_source(source);

		OBSScene scene = main->GetCurrentScene();
		std::string sceneUUID = obs_source_get_uuid(obs_scene_get_source(scene));
		std::string sourceUUID = obs_source_get_uuid(source);

		auto undo = [sceneUUID, sourceUUID](const std::string &) {
			OBSSourceAutoRelease source = obs_get_source_by_uuid(sourceUUID.c_str());
			obs_source_remove(source);
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUUID.c_str());
			OBSBasic::Get()->SetCurrentScene(scene.Get(), true);
		};
		auto redo = [sceneUUID, sourceName, type](const std::string &data) {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(sceneUUID.c_str());
			OBSBasic::Get()->SetCurrentScene(scene.Get(), true);

			OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
			OBSSourceAutoRelease source = obs_load_source(dat);
			obs_scene_add(obs_scene_from_source(scene), source.Get());
		};

		undo_s.add_action(QTStr("Undo.Add").arg(sourceName.c_str()), undo, redo, "",
				  std::string(obs_data_get_json(wrapper)));
		obs_scene_add(scene, source);
	}
}

void OBSBasic::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat("application/x-obs-source-uuid")) {
		event->acceptProposedAction();
	}

	// refuse drops of our own widgets
	if (event->source() != nullptr) {
		event->setDropAction(Qt::IgnoreAction);
		return;
	}

	event->acceptProposedAction();
}

void OBSBasic::dragLeaveEvent(QDragLeaveEvent *event)
{
	event->accept();
}

void OBSBasic::dragMoveEvent(QDragMoveEvent *event)
{
	event->acceptProposedAction();
}

void OBSBasic::ConfirmDropUrl(const QString &url)
{
	if (url.left(7).compare("http://", Qt::CaseInsensitive) == 0 ||
	    url.left(8).compare("https://", Qt::CaseInsensitive) == 0) {

		activateWindow();

		QString msg = QTStr("AddUrl.Text");
		msg += "\n\n";
		msg += QTStr("AddUrl.Text.Url").arg(url);

		QMessageBox messageBox(this);
		messageBox.setWindowTitle(QTStr("AddUrl.Title"));
		messageBox.setText(msg);

		QPushButton *yesButton = messageBox.addButton(QTStr("Yes"), QMessageBox::YesRole);
		QPushButton *noButton = messageBox.addButton(QTStr("No"), QMessageBox::NoRole);
		messageBox.setDefaultButton(yesButton);
		messageBox.setEscapeButton(noButton);
		messageBox.setIcon(QMessageBox::Question);
		messageBox.exec();

		if (messageBox.clickedButton() == yesButton) {
			AddDropSource(QT_TO_UTF8(url), DropType_Url);
		}
	}
}

void OBSBasic::dropEvent(QDropEvent *event)
{
	const QMimeData *mimeData = event->mimeData();

	if (mimeData->hasUrls()) {
		QList<QUrl> urls = mimeData->urls();

		for (int i = 0; i < urls.size(); i++) {
			QUrl url = urls[i];
			QString file = url.toLocalFile();
			QFileInfo fileInfo(file);

			if (!fileInfo.exists()) {
				ConfirmDropUrl(url.url());
				continue;
			}

#ifdef _WIN32
			if (fileInfo.suffix().compare("url", Qt::CaseInsensitive) == 0) {
				QString urlTarget = ReadWindowsURLFile(file);
				if (!urlTarget.isEmpty()) {
					ConfirmDropUrl(urlTarget);
				}
				continue;
			} else if (fileInfo.isShortcut()) {
				file = fileInfo.symLinkTarget();
				fileInfo = QFileInfo(file);
				if (!fileInfo.exists()) {
					continue;
				}
			}
#endif

			QString suffixQStr = fileInfo.suffix();
			QByteArray suffixArray = suffixQStr.toUtf8();
			const char *suffix = suffixArray.constData();
			bool found = false;

			const char **cmp;

#define CHECK_SUFFIX(extensions, type)                         \
	cmp = extensions;                                      \
	while (*cmp) {                                         \
		if (astrcmpi(*cmp, suffix) == 0) {             \
			AddDropSource(QT_TO_UTF8(file), type); \
			found = true;                          \
			break;                                 \
		}                                              \
                                                               \
		cmp++;                                         \
	}                                                      \
                                                               \
	if (found)                                             \
		continue;

			CHECK_SUFFIX(textExtensions, DropType_Text);
			CHECK_SUFFIX(htmlExtensions, DropType_Html);
			CHECK_SUFFIX(imageExtensions, DropType_Image);
			CHECK_SUFFIX(mediaExtensions, DropType_Media);

#undef CHECK_SUFFIX
		}
	} else if (mimeData->hasText()) {
		AddDropSource(QT_TO_UTF8(mimeData->text()), DropType_RawText);
	} else if (event->mimeData()->hasFormat("application/x-obs-source-uuid")) {
		QString uuid = QString::fromUtf8(event->mimeData()->data("application/x-obs-source-uuid"));

		emit sourceUuidDropped(uuid);
		event->acceptProposedAction();
	}
}
