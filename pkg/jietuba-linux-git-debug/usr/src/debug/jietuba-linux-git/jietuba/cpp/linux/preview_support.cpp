// Temporary services used only by the Linux UI preview target.
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include <QWidget>
#include <cmath>

namespace jietuba {
int scaled(double base) { return static_cast<int>(std::nearbyint(base)); }
double scaled_f(double base) { return base; }
double scale_factor() { return 1.0; }
int dialog_scaled(double base) { return scaled(base); }
double dialog_scaled_f(double base) { return base; }
int widget_scaled(const QWidget *, double base) { return scaled(base); }
qint64 python_round(double value) { return static_cast<qint64>(std::nearbyint(value)); }
// 预览尚未支持独立窗口缩放；保留原版对话框布局并使用 Qt 默认字体。
// Window scaling is not yet ported; keep the original dialog layout and Qt font.
void scale_dialog_font(QWidget &) {}
// 原版布局对话框逐个配置 Fluent 控件；Linux 由 Qt 处理系统缩放，逻辑缩放保持 1。
// The original layout dialog configures individual Fluent controls; Qt owns Linux scaling.
void configure_dialog_control(QWidget &widget) {
    widget.setProperty("dialog_scale_factor",1.0);
    if (widget.metaObject()->indexOfMethod("_apply_theme()")>=0)
        QMetaObject::invokeMethod(&widget,"_apply_theme",Qt::DirectConnection);
}
void configure_dialog_controls(QWidget &) {}
// The preview intentionally uses default styles and passes a null settings manager.
QVariant ToolSettingsManager::get_setting(const QString &, const QString &, const QVariant &fallback) const { return fallback; }
void ToolSettingsManager::update_settings(const QString &, const QVariantMap &, bool) {}
int ToolSettingsManager::get_shift_line_angle() const { return 15; }
ToolSettings *ToolSettingsManager::get_tool_settings(const QString &) { return nullptr; }
QVariant ToolSettings::get(const QString &, const QVariant &fallback) const { return fallback; }
QSettings &SettingsStore::store(const QString &) const { return *settings_; }
}
