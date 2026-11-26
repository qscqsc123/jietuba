#pragma once
#include <QSettings>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QRect>
#include <QSize>
#include "gif/video_settings.h"

/**
 * Linux 版基础设置的持久化存储。
 * 默认使用 XDG 配置目录；也可注入独立 INI 路径用于测试，避免修改用户配置。
 * 此类只保存已实现的选项，读取时校验外部文件，防止非法值进入截图逻辑。
 *
 * Persistent Linux settings, using XDG's configuration directory by default.
 * An explicit INI path isolates tests; getters validate hand-edited values.
 */
class LinuxSettings {
public:
    explicit LinuxSettings(const QString &fileName = {});
    QString fileName() const;
    bool showMainWindowOnStartup() const;
    void setShowMainWindowOnStartup(bool show);
    QString language() const;
    void setLanguage(const QString &language);
    // Import portable preferences only; return an error without applying an unreadable file.
    // 只导入可迁移的偏好；文件无法读取时不修改配置。
    QString importPreferences(const QString &path);
    QStringList globalShortcutSlots(const QString &action) const;
    bool hasGlobalShortcutSlots(const QString &action) const;
    void setGlobalShortcutSlots(const QString &action,const QStringList &values);
    QString saveDirectory() const;
    void setSaveDirectory(const QString &path);
    QString imageFormat() const;
    void setImageFormat(const QString &format);
    enum class CaptureFlag { Cursor, Crosshair, AutoSave, SavePin, SaveOriginal, FileUrl, SnapAnimation, CopyOcr, CopyCode, AutoWatermark };
    bool captureFlag(CaptureFlag flag) const;
    void setCaptureFlag(CaptureFlag flag, bool value);
    QString captureNamePattern() const;
    bool setCaptureNamePattern(const QString &pattern);
    QString smartSelectionMode() const;
    void setSmartSelectionMode(const QString &mode);
    QString ocrPosition() const;
    void setOcrPosition(const QString &position);
    QString ocrEngine() const;
    void setOcrEngine(const QString &engine);
    bool crossToolSelection() const;
    void setCrossToolSelection(bool value);
    int shiftLineAngle() const;
    void setShiftLineAngle(int degrees);
    bool textAlwaysOnTop() const;
    void setTextAlwaysOnTop(bool value);
    bool captureAllScreens() const;
    void setCaptureAllScreens(bool value);
    // GIF 设置在控制栏和设置页共享；只允许实现支持的帧率与倒计时。
    // Share validated GIF defaults between the recording controls and settings page.
    int gifFps() const;
    void setGifFps(int value);
    int gifDelay() const;
    void setGifDelay(int value);
    QString recordingMode() const;
    void setRecordingMode(const QString &mode);
    jietuba::RecordingOptions videoOptions() const;
    void setVideoOptions(const jietuba::RecordingOptions &options);
    // 保存原版 [[按钮标识, 显示模式], ...] 数据；布局规则由共用 toolbar_layout 校验。
    // Store original button/mode pairs; shared toolbar_layout owns normalization rules.
    QJsonArray screenshotToolbarLayout() const;
    bool saveScreenshotToolbarLayout(const QJsonArray &layout);
    // 贴图和截图各自保存工具栏布局，避免修改一处影响另一处。
    // Pin and capture layouts are independent preferences.
    QJsonArray annotationToolbarLayout() const;
    bool saveAnnotationToolbarLayout(const QJsonArray &layout);
    QJsonArray pinToolbarLayout() const;
    bool savePinToolbarLayout(const QJsonArray &layout);
    // 样式按工具分别存储；选中图形的临时编辑不会写入这些默认值。
    // Defaults are stored per tool, independently from temporary selected-item edits.
    QVariantMap toolStyle(const QString &tool) const;
    bool saveToolStyle(const QString &tool, const QVariantMap &style);
    bool magnifierOption(const QString &name) const;
    void setMagnifierOption(const QString &name, bool value);
    double magnifierZoom() const;
    void setMagnifierZoom(double value);
    QStringList magnifierFormats() const;
    void setMagnifierFormats(const QStringList &formats);
    // 单键组合使用 Qt 可移植文本；空值禁用，Esc 固定保留，重复按键在同一上下文内拒绝。
    // Portable single-chord bindings: empty disables, Escape is reserved, context conflicts are rejected.
    QString shortcut(const QString &action) const;
    bool setShortcut(const QString &action, const QString &key, QString *error = nullptr);
    void resetShortcuts();
    QRect lastRegion(const QSize &imageSize, const QString &output) const;
    void setLastRegion(const QRect &region, const QSize &imageSize, const QString &output);
    QString theme() const;
    void setTheme(const QString &value);
    int borderWidth() const;
    void setBorderWidth(int value);
    bool pinBorderEnabled() const;
    void setPinBorderEnabled(bool value);
    bool pinAutoToolbar() const;
    void setPinAutoToolbar(bool value);
    bool pinHoverButtons() const;
    void setPinHoverButtons(bool value);
    bool pinFlag(const QString &key) const;
    void setPinFlag(const QString &key, bool value);
    int pinPercent(const QString &key) const;
    void setPinPercent(const QString &key, int value);
    QString pinZoomAnchor() const;
    void setPinZoomAnchor(const QString &value);
    int pinZoomStep() const;
    void setPinZoomStep(int value);
    QString watermarkText() const;
    void setWatermarkText(const QString &value);
    QVariantMap watermarkStyle() const;
    void setWatermarkStyle(const QVariantMap &value);
    QString translationProvider() const;
    void setTranslationProvider(const QString &value);
    QString translationApiKey() const;
    void setTranslationApiKey(const QString &value);
    QString googleApiKey() const;
    void setGoogleApiKey(const QString &value);
    QString youdaoAppId() const;
    void setYoudaoAppId(const QString &value);
    QString youdaoAppSecret() const;
    void setYoudaoAppSecret(const QString &value);
    QString translationEndpoint() const;
    void setTranslationEndpoint(const QString &value);
    QString translationTarget() const;
    void setTranslationTarget(const QString &value);
    QSettings::Status sync();
private:
    QSettings settings_;
};
