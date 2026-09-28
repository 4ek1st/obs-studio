#include "UiGeometry.hpp"
#include <QCoreApplication>
#include <iostream>

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	int failures = 0;
	int checks = 0;
	auto check = [&](bool pass, const char *label) {
		++checks;
		if (!pass) {
			++failures;
			std::cerr << "FAIL: " << label << '\n';
		}
	};
	const QJsonObject bounds{{"x", 10}, {"y", 50}, {"width", 980}, {"height", 450},
				 {"viewportWidth", 1000}, {"viewportHeight", 700}};
	for (const double scale : {1.0, 1.2, 1.25, 1.5, 2.0}) {
		const QRect host(0, 0, qRound(1000 * scale), qRound(700 * scale));
		const QRect expected(qRound(10 * scale), qRound(50 * scale), qRound(990 * scale) - qRound(10 * scale), qRound(500 * scale) - qRound(50 * scale));
		check(OBSWeb::PreviewRect(bounds, host) == expected, "CSS pixels map to the complete native preview area");
	}
	check(OBSWeb::PreviewRect(bounds, QRect(8, 30, 1000, 700)) == QRect(18, 80, 980, 450),
	      "Preview includes the browser's parent-relative offset");
	for (const auto *key : {"viewportWidth", "viewportHeight", "width", "height", "x", "y"}) {
		auto invalid = bounds;
		invalid[key] = -1;
		check(!OBSWeb::PreviewRect(invalid, QRect(0, 0, 1000, 700)), "Negative geometry is rejected");
	}
	auto invalid = bounds;
	invalid.remove("viewportWidth");
	check(!OBSWeb::PreviewRect(invalid, QRect(0, 0, 1000, 700)), "Missing viewport is rejected");
	invalid = bounds;
	invalid["viewportWidth"] = 0;
	check(!OBSWeb::PreviewRect(invalid, QRect(0, 0, 1000, 700)), "Zero viewport cannot divide");
	invalid = bounds;
	invalid["viewportWidth"] = 0.0000001;
	check(!OBSWeb::PreviewRect(invalid, QRect(0, 0, 1000, 700)), "Tiny viewport cannot overflow native coordinates");
	invalid = bounds;
	invalid["width"] = 1001;
	check(!OBSWeb::PreviewRect(invalid, QRect(0, 0, 1000, 700)), "Out-of-viewport rectangles are rejected");
	check(OBSWeb::ControlLabel(" Start ", "Accessible", "Tooltip") == "Start", "Visible text has priority");
	check(OBSWeb::ControlLabel("", "Virtual camera settings", "") == "Virtual camera settings",
	      "Icon-only control uses native accessible name");
	check(OBSWeb::ControlLabel(" ", "", "Pause recording") == "Pause recording", "Tooltip is the final text fallback");
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
