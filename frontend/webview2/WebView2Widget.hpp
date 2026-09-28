#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>
#include <functional>
#include "ExternalDrop.hpp"

class WebView2Widget : public QWidget {
	Q_OBJECT
public:
	WebView2Widget(QWidget *parent, QString assetsPath, QString profilePath,
		       QString document = QStringLiteral("index.html"));
	~WebView2Widget() override;
	void postMessage(const QJsonObject &message);
	// Native-only diagnostic API; paths are never accepted from web messages.
	void capturePreview(QString path, std::function<void(bool)> done);

signals:
	void ready();
	void presented();
	void messageReceived(QJsonObject message);
	void failed(QString message);
	void externalDrop(QString requestId, OBSWeb::ExternalDropData drop);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void focusInEvent(QFocusEvent *event) override;
	bool event(QEvent *event) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	void initialize();
	void presentFrame();
	void updateBounds();
	void queueBoundsUpdate();
	void reportFailure(const QString &stage, long result);
};
