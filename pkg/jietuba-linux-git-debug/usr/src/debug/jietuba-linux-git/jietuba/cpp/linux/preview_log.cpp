// Temporary preview logging adapter. The Windows logger will be ported separately.
#include "core/logger.h"
#include <QDebug>

namespace jietuba {
void log_debug(const LogMsg &message, const QString &module) {
    qDebug().noquote() << module << message.template_text();
}
// Linux 预览暂用 Qt 日志输出，确保原版工具的信息日志也能链接。
// Route original tools' informational logs through Qt in the Linux preview.
void log_info(const LogMsg &message, const QString &module) { qInfo().noquote() << module << message.template_text(); }
void log_warning(const LogMsg &message, const QString &module) {
    qWarning().noquote() << module << message.template_text();
}
void log_error(const LogMsg &message, const QString &module) { qWarning().noquote() << module << message.template_text(); }
QString exception_text(const std::exception &error) { return QString::fromUtf8(error.what()); }
QString geometry_text(const QPointF &point) { return QString("(%1,%2)").arg(point.x()).arg(point.y()); }
QString geometry_text(const QRectF &rect) { return QString("(%1,%2 %3x%4)").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()); }
}
