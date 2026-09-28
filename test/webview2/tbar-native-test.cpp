#include <SliderBridge.hpp>
#include <QApplication>
#include <iostream>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setStyle("Fusion");
	SliderIgnoreClick slider(Qt::Horizontal);
	slider.setRange(0, 1023); slider.resize(300, 40); slider.show(); app.processEvents();
	int failed = 0, checked = 0, released = 0;
	QObject::connect(&slider, &QSlider::sliderReleased, [&] { ++released; });
	auto check = [&](bool pass, const char *label) { ++checked; if (!pass) { ++failed; std::cerr << "FAIL: " << label << '\n'; } };
	auto mouse = [&](const char *kind, double x) {
		return OBSWeb::DispatchSliderInput(&slider, {{"kind", kind}, {"x", x}, {"y", 0.5}, {"button", 0}});
	};
	check(mouse("press", 0.8) && !slider.isSliderDown() && slider.value() == 0,
		"original SliderIgnoreClick ignores a press on the track");
	mouse("move", 0.6); mouse("release", 0.6);
	check(slider.value() == 0 && released == 0, "track press followed by movement never starts or releases a transition");
	const auto geometry = OBSWeb::SliderGeometry(&slider);
	check(geometry.value("thumbWidth").toDouble() > 0 && geometry.value("first").toDouble() < 0.2,
		"frontend handle geometry comes from the actual Qt style");
	mouse("press", geometry.value("first").toDouble());
	check(slider.isSliderDown(), "grabbing the native handle starts its original slider gesture");
	mouse("move", 0.7);
	check(slider.value() > 600 && slider.value() < 800, "native drag changes the T-bar through QSlider geometry");
	mouse("release", 0.7);
	check(!slider.isSliderDown() && released == 1, "pointer release invokes native sliderReleased exactly once");
	slider.setValue(100);
	check(OBSWeb::DispatchSliderInput(&slider, {{"kind", "key"}, {"key", "PageUp"}}) && slider.value() == 100 + slider.pageStep(),
		"native T-bar PageUp uses QSlider's real page step");
	const auto beforeWheel = slider.value();
	check(OBSWeb::DispatchSliderInput(&slider, {{"kind", "wheel"}, {"value", 120}}) && slider.value() > beforeWheel && released == 1,
		"native wheel changes the value without a fabricated release");
	check(!OBSWeb::DispatchSliderInput(&slider, {{"kind", "wheel"}, {"value", "120"}}), "malformed native input is rejected");
	slider.hide(); slider.setValue(0);
	const auto parkedGeometry = OBSWeb::SliderGeometry(&slider);
	mouse("press", parkedGeometry.value("first").toDouble());
	check(slider.isSliderDown(), "the original control still receives gestures while its native central widget is parked");
	mouse("release", parkedGeometry.value("first").toDouble());
	slider.setEnabled(false);
	check(!mouse("press", 0.3) && !OBSWeb::DispatchSliderInput(&slider, {{"kind", "key"}, {"key", "End"}}),
		"disabled native T-bar rejects both pointer and keyboard input");
	std::cout << checked << " checks, " << failed << " failures\n";
	return failed ? 1 : 0;
}
