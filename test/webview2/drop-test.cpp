#include "ExternalDrop.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QWidget>
#include <iostream>

class Receiver : public QWidget {
public:
	int drops = 0;
	QList<QUrl> urls;
	QString text;
	Receiver() { setAcceptDrops(true); }
	void dragEnterEvent(QDragEnterEvent *event) override { event->acceptProposedAction(); }
	void dropEvent(QDropEvent *event) override { ++drops; urls = event->mimeData()->urls(); text = event->mimeData()->text(); }
};
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	int checks = 0, failed = 0;
	auto check = [&](bool ok, const char *what) { ++checks; if (!ok) { ++failed; std::cerr << "FAIL: " << what << '\n'; } };
	check(OBSWeb::IsExternalDropOrigin("https://obs-ui.example/index.html"), "workspace is trusted");
	check(OBSWeb::IsExternalDropOrigin("https://obs-ui.example/dialog.html", "dialog.html"), "dialog host explicitly authorizes its own document");
	check(!OBSWeb::IsExternalDropOrigin("https://obs-ui.example/index.html", "dialog.html") &&
	      !OBSWeb::IsExternalDropOrigin("https://obs-ui.example/dialog.html?x=1", "dialog.html") &&
	      !OBSWeb::IsExternalDropOrigin("https://obs-ui.example/custom.html", "custom.html"), "dialog drop authorization cannot cross document boundaries");
	for (const auto *origin : {"https://example.com/index.html", "https://obs-ui.example/dialog.html", "file:///index.html", "https://obs-ui.example:444/index.html", "https://obs-ui.local/index.html"})
		check(!OBSWeb::IsExternalDropOrigin(origin), "other origins/documents cannot import drops");
	QString error;
	QTemporaryFile file; if (!file.open()) return 2;
	QTemporaryDir directory; if (!directory.isValid()) return 2;
	const auto folder = OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}}, {directory.path()}, error);
	check(folder && folder->urls == QList<QUrl>{QUrl::fromLocalFile(directory.path())}, "trusted native directory remains available to Remux and Importer enumeration");
	auto parsed = OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}}, {file.fileName()}, error);
	check(parsed && parsed->urls == QList<QUrl>{QUrl::fromLocalFile(file.fileName())}, "native file becomes local MIME URL");
	check(!OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}, {"path", file.fileName()}}, {}, error), "JSON cannot authorize a local file");
	check(!OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 2}}, {file.fileName()}, error), "missing COM file rejected");
	check(!OBSWeb::ParseExternalDrop({{"kind", "files"}, {"count", 1}}, {"relative.png"}, error), "nonabsolute file rejected");
	auto link = OBSWeb::ParseExternalDrop({{"kind", "urls"}, {"urls", QJsonArray{"https://example.com/stream?x=1"}}}, {}, error);
	check(link && link->urls.size() == 1 && link->urls[0].scheme() == "https", "external HTTP URL retains native confirmation route");
	for (const auto *uri : {"file:///C:/private.png", "C:/private.png", "javascript:alert(1)", "data:text/plain,test"})
		check(!OBSWeb::ParseExternalDrop({{"kind", "urls"}, {"urls", QJsonArray{uri}}}, {}, error), "JS URL cannot import local paths or active schemes");
	auto text = OBSWeb::ParseExternalDrop({{"kind", "text"}, {"text", "C:\\private.png"}}, {}, error);
	check(text && text->urls.isEmpty() && text->text == "C:\\private.png", "plain text is text rather than file import");
	check(!OBSWeb::ParseExternalDrop({{"kind", "text"}, {"text", QString(65537, 'x')}}, {}, error), "large input rejected");
	check(!OBSWeb::ParseExternalDrop({{"kind", "text"}, {"text", "hello"}}, {file.fileName()}, error), "mixed file payload rejected");
	Receiver target;
	if (parsed) check(OBSWeb::DispatchExternalDrop(&target, *parsed) && target.drops == 1 && target.urls == parsed->urls, "file MIME reaches native drop handler");
	else check(false, "file MIME reaches native drop handler");
	if (text) check(OBSWeb::DispatchExternalDrop(&target, *text) && target.drops == 2 && target.text == text->text, "raw text reaches native drop handler");
	else check(false, "raw text reaches native drop handler");
	std::cout << checks << " checks, " << failed << " failures\n";
	return failed ? 1 : 0;
}
