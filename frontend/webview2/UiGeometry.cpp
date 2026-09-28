#include "UiGeometry.hpp"
#include <cmath>
#include <limits>

namespace OBSWeb {
std::optional<QRect> PreviewRect(const QJsonObject &args, const QRect &browserGeometry)
{
	auto number = [&](const char *key) {
		const auto value = args.value(QLatin1String(key));
		return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
	};
	const double x = number("x"), y = number("y"), width = number("width"), height = number("height");
	const double viewportWidth = number("viewportWidth"), viewportHeight = number("viewportHeight");
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
	    !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) || viewportWidth < 1 ||
	    viewportHeight < 1 || x < 0 || y < 0 || width < 0 || height < 0 ||
	    x + width > viewportWidth + 0.01 || y + height > viewportHeight + 0.01 ||
	    browserGeometry.width() <= 0 || browserGeometry.height() <= 0)
		return std::nullopt;
	// WebView2 CSS pixels and Qt logical pixels can have different scale factors
	// (for example Windows text scaling). Map viewport edges, not raw CSS units.
	const double scaleX = browserGeometry.width() / viewportWidth;
	const double scaleY = browserGeometry.height() / viewportHeight;
	const int left = qRound(x * scaleX), top = qRound(y * scaleY);
	const int right = qRound((x + width) * scaleX), bottom = qRound((y + height) * scaleY);
	return QRect(browserGeometry.x() + left, browserGeometry.y() + top, right - left, bottom - top)
		.intersected(browserGeometry);
}

QString ControlLabel(const QString &text, const QString &accessibleName, const QString &tooltip)
{
	for (const auto &value : {text, accessibleName, tooltip}) {
		if (!value.trimmed().isEmpty())
			return value.trimmed();
	}
	return {};
}
} // namespace OBSWeb
