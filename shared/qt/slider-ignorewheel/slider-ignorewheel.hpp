#pragma once

#include <QSlider>
#include <QInputEvent>
#include <QtCore/QObject>
#include <QStyleOptionSlider>

class SliderIgnoreScroll : public QSlider {
	Q_OBJECT

public:
	SliderIgnoreScroll(QWidget *parent = nullptr);
	SliderIgnoreScroll(Qt::Orientation orientation, QWidget *parent = nullptr);

	// A focused embedded frontend can forward input without moving Qt focus
	// away from its own surface. The normal native wheel focus gate is unchanged.
	void handleFrontendWheel(QWheelEvent *event) { QSlider::wheelEvent(event); }
	QStyleOptionSlider frontendStyleOption() const
	{
		QStyleOptionSlider option;
		initStyleOption(&option);
		return option;
	}

protected:
	virtual void wheelEvent(QWheelEvent *event) override;
};

class SliderIgnoreClick : public SliderIgnoreScroll {
public:
	inline SliderIgnoreClick(Qt::Orientation orientation, QWidget *parent = nullptr)
		: SliderIgnoreScroll(orientation, parent)
	{
	}

protected:
	virtual void mousePressEvent(QMouseEvent *event) override;
	virtual void mouseReleaseEvent(QMouseEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;

private:
	bool dragging = false;
};
