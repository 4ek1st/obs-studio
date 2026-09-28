#include "ExternalDrop.hpp"
#include "BridgeProtocol.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QMimeData>
#include <QPointer>
#include <QWidget>
namespace OBSWeb {
bool IsExternalDropOrigin(const QString &source, const QString &document)
{
	const QUrl url(source);
	return (document == QStringLiteral("index.html") || document == QStringLiteral("dialog.html")) &&
	       IsLocalUi(url) && url.path() == QLatin1Char('/') + document && !url.hasQuery() && !url.hasFragment();
}
std::optional<ExternalDropData> ParseExternalDrop(const QJsonObject &args, const QStringList &files, QString &error)
{
	error.clear();
	auto reject = [&](const char *reason) -> std::optional<ExternalDropData> { error = QString::fromLatin1(reason); return std::nullopt; };
	const QString kind = args.value("kind").toString();
	ExternalDropData drop;
	if (kind == QStringLiteral("files")) {
		if (args.size() != 2 || !args.value("count").isDouble() || files.isEmpty() || files.size() > 128 ||
		    args.value("count").toDouble() != files.size())
			return reject("A drop must contain between 1 and 128 native file objects.");
		for (const auto &file : files) {
			const QFileInfo info(file);
			if (file.size() > 32767 || file.contains(QChar(0)) || !info.isAbsolute() || (!info.isFile() && !info.isDir()))
				return reject("The dropped file is no longer available.");
			drop.urls.append(QUrl::fromLocalFile(file));
		}
	} else if (kind == QStringLiteral("urls")) {
		if (!files.isEmpty() || args.size() != 2 || !args.value("urls").isArray())
			return reject("Invalid URL drop.");
		const auto urls = args.value("urls").toArray();
		if (urls.isEmpty() || urls.size() > 128)
			return reject("Too many dropped URLs.");
		qsizetype size = 0;
		for (const auto &value : urls) {
			if (!value.isString()) return reject("Invalid URL.");
			const QString text = value.toString();
			size += text.size();
			const QUrl url(text, QUrl::StrictMode);
			if (text.size() > 8192 || size > 60000 || text.contains(QChar(0)) || !url.isValid() || url.host().isEmpty() ||
			    (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")))
				return reject("Only HTTP and HTTPS links can be imported without a native file object.");
			drop.urls.append(url);
		}
	} else if (kind == QStringLiteral("text")) {
		if (!files.isEmpty() || args.size() != 2 || !args.value("text").isString())
			return reject("Invalid text drop.");
		drop.text = args.value("text").toString();
		if (drop.text.isEmpty() || drop.text.size() > 60000 || drop.text.contains(QChar(0)))
			return reject("Dropped text is empty or too large.");
	} else {
		return reject("Unsupported external drop.");
	}
	return drop;
}
bool DispatchExternalDrop(QWidget *target, const ExternalDropData &drop)
{
	if (!target || (drop.urls.isEmpty() && drop.text.isEmpty())) return false;
	QPointer<QWidget> guard(target);
	QMimeData mime;
	if (!drop.urls.isEmpty()) mime.setUrls(drop.urls); else mime.setText(drop.text);
	// Match an external Qt drag (source() == nullptr), preserving OBS's import/undo/confirmation path.
	QDragEnterEvent enter(QPoint(0, 0), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(target, &enter);
	if (!guard || !enter.isAccepted()) return false;
	QDropEvent event(QPointF(0, 0), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(guard, &event);
	// OBS consumes file/text drops without setting accepted, so report delivery rather than an invented import result.
	return true;
}
}
