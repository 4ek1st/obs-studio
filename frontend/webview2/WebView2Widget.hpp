#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>

class WebView2Widget : public QWidget {
	Q_OBJECT
public:
	WebView2Widget(QWidget *parent, QString assetsPath, QString profilePath);
	~WebView2Widget() override;
	void postMessage(const QJsonObject &message);

signals:
	void ready();
	void messageReceived(QJsonObject message);
	void failed(QString message);

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
	void updateBounds();
	void reportFailure(const QString &stage, long result);
};
