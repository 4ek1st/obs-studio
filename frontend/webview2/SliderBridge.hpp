#pragma once

#include <slider-ignorewheel.hpp>
#include <QApplication>
#include <QHash>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace OBSWeb {
inline QJsonObject SliderGeometry(SliderIgnoreScroll *slider)
{
	const double width = std::max(1, slider->width()), height = std::max(1, slider->height());
	auto option = slider->frontendStyleOption();
	option.sliderPosition = slider->minimum();
	const auto first = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
	option.sliderPosition = slider->maximum();
	const auto last = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
	return {{"first", (first.x() + first.width() / 2.0) / width}, {"last", (last.x() + last.width() / 2.0) / width},
		{"thumbWidth", first.width() / width}, {"thumbHeight", first.height() / height}, {"height", height},
		{"minimum", slider->minimum()}, {"maximum", slider->maximum()}};
}

inline bool DispatchSliderInput(SliderIgnoreScroll *slider, const QJsonObject &args)
{
	if (!slider || !slider->isEnabled()) return false;
	for (const auto *name : {"control", "shift"})
		if (args.contains(name) && !args.value(name).isBool()) return false;
	const auto modifiers = Qt::KeyboardModifiers((args.value("control").toBool() ? Qt::ControlModifier : 0) |
		(args.value("shift").toBool() ? Qt::ShiftModifier : 0));
	const auto kind = args.value("kind").toString();
	if (kind == "key") {
		static const QHash<QString, int> keys{{"ArrowLeft", Qt::Key_Left}, {"ArrowRight", Qt::Key_Right},
			{"ArrowUp", Qt::Key_Up}, {"ArrowDown", Qt::Key_Down}, {"PageUp", Qt::Key_PageUp},
			{"PageDown", Qt::Key_PageDown}, {"Home", Qt::Key_Home}, {"End", Qt::Key_End}};
		const auto key = args.value("key").toString();
		if (!keys.contains(key)) return false;
		QKeyEvent event(QEvent::KeyPress, keys.value(key), modifiers);
		QApplication::sendEvent(slider, &event);
		return true;
	}
	if (kind == "wheel") {
		const auto value = args.value("value");
		if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() == 0 ||
			std::abs(value.toDouble()) > 12000 || std::trunc(value.toDouble()) != value.toDouble()) return false;
		const QPoint center = slider->rect().center();
		QWheelEvent event(QPointF(center), QPointF(slider->mapToGlobal(center)), QPoint(), QPoint(0, value.toInt()),
			Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
		slider->handleFrontendWheel(&event);
		return true;
	}
	if (kind != "press" && kind != "move" && kind != "release") return false;
	const auto x = args.value("x"), y = args.value("y"), button = args.value("button");
	if (!x.isDouble() || !y.isDouble() || !std::isfinite(x.toDouble()) || !std::isfinite(y.toDouble()) ||
		x.toDouble() < -1 || x.toDouble() > 2 || y.toDouble() < -1 || y.toDouble() > 2 ||
		!button.isDouble() || (button.toDouble() != 0 && button.toDouble() != 1)) return false;
	const auto mouseButton = button.toInt() == 0 ? Qt::LeftButton : Qt::MiddleButton;
	const auto type = kind == "press" ? QEvent::MouseButtonPress : kind == "move" ? QEvent::MouseMove : QEvent::MouseButtonRelease;
	const QPointF position(x.toDouble() * slider->width(), y.toDouble() * slider->height());
	QMouseEvent event(type, position, QPointF(slider->mapToGlobal(position.toPoint())),
		kind == "move" ? Qt::NoButton : mouseButton, kind == "release" ? Qt::NoButton : mouseButton, modifiers);
	QApplication::sendEvent(slider, &event);
	return true;
}
} // namespace OBSWeb
