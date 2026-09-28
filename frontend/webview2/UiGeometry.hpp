#pragma once
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <optional>

namespace OBSWeb {
std::optional<QRect> PreviewRect(const QJsonObject &args, const QRect &browserGeometry);
QString ControlLabel(const QString &text, const QString &accessibleName, const QString &tooltip);
}
