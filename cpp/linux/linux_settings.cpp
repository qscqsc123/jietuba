#include "linux_settings.h"
#include "core/i18n.h"
#include "core/file_names.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QRect>
#include <QSize>
#include <cmath>
#include "shortcut_bindings.h"
#include <QStandardPaths>
#include <QStringList>
#include <algorithm>

namespace {
bool knownTool(const QString &tool) { return QStringList{"pen","highlighter","rect","ellipse","arrow","text","mosaic","number","spotlight","eraser"}.contains(tool); }
QString configFile(const QString &fileName) {
    if (!fileName.isEmpty()) return fileName;
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/jietuba-linux");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.ini");
}
}
QVariantMap LinuxSettings::toolStyle(const QString &tool) const {
    if (!knownTool(tool)) return {};
    const auto raw=settings_.value("tools/"+tool).toByteArray();
    return raw.size()<=16384 ? QJsonDocument::fromJson(raw).object().toVariantMap() : QVariantMap{};
}
bool LinuxSettings::saveToolStyle(const QString &tool, const QVariantMap &style) {
    if (!knownTool(tool)) return false;
    const auto raw=QJsonDocument(QJsonObject::fromVariantMap(style)).toJson(QJsonDocument::Compact);
    if (raw.size()>16384) return false;
    settings_.setValue("tools/"+tool,raw); return sync()==QSettings::NoError;
}
bool LinuxSettings::magnifierOption(const QString &name) const {
    if (!QStringList{"enabled","grid","selection","hints"}.contains(name)) return false;
    return settings_.value("magnifier/"+name,name=="enabled" || name=="hints").toBool();
}
void LinuxSettings::setMagnifierOption(const QString &name, bool value) {
    if (QStringList{"enabled","grid","selection","hints"}.contains(name)) settings_.setValue("magnifier/"+name,value);
}
double LinuxSettings::magnifierZoom() const {
    bool ok=false; const double value=settings_.value("magnifier/zoom",4.0).toDouble(&ok);
    return ok && std::isfinite(value) ? std::clamp(value,2.0,10.0) : 4.0;
}
void LinuxSettings::setMagnifierZoom(double value) { if (std::isfinite(value)) settings_.setValue("magnifier/zoom",std::clamp(value,2.0,10.0)); }
QStringList LinuxSettings::magnifierFormats() const {
    const auto saved=settings_.value("magnifier/formats",QStringList{"RGB","HEX"}).toStringList();
    QStringList result;
    for (const auto &value:saved) if (QStringList{"RGB","CSS rgb()","HEX","HEX without #","CSS hsl()"}.contains(value) && !result.contains(value)) result.append(value);
    return result.isEmpty() ? QStringList{"RGB","HEX"} : result;
}
void LinuxSettings::setMagnifierFormats(const QStringList &formats) { settings_.setValue("magnifier/formats",formats); }
QString LinuxSettings::shortcut(const QString &action) const {
    for (const auto &entry:linuxShortcutActions()) if (entry.id==action) {
        const auto raw=settings_.value("shortcuts/"+action,entry.defaultKey).toString();
        if (raw.isEmpty()) return {};
        const auto normalized=linuxNormalizeShortcut(raw);
        return normalized.isEmpty() ? entry.defaultKey : normalized;
    }
    return {};
}
bool LinuxSettings::setShortcut(const QString &action, const QString &key, QString *error) {
    const auto reject=[&](const QString &message) { if (error) *error=message; return false; };
    for (const auto &entry:linuxShortcutActions()) if (entry.id==action) {
        const auto normalized=linuxNormalizeShortcut(key);
        if (!key.isEmpty() && normalized.isEmpty())
            return reject(QStringLiteral("请输入一个有效组合键；Esc 固定用于取消，不能改绑。"));
        for (const auto &other:linuxShortcutActions()) if (!normalized.isEmpty() && other.context==entry.context && other.id!=action && shortcut(other.id)==normalized)
            return reject(QStringLiteral("与“%1”的快捷键重复，请先修改或清除该项。").arg(other.title));
        settings_.setValue("shortcuts/"+action,normalized); return sync()==QSettings::NoError;
    }
    return reject(QStringLiteral("未知快捷键动作。"));
}
void LinuxSettings::resetShortcuts() { settings_.remove("shortcuts"); sync(); }
QRect LinuxSettings::lastRegion(const QSize &imageSize, const QString &output) const {
    if (settings_.value("capture/last_output").toString()!=output || settings_.value("capture/last_size").toSize()!=imageSize) return {};
    return settings_.value("capture/last_region").toRect().intersected(QRect(QPoint(),imageSize));
}
void LinuxSettings::setLastRegion(const QRect &region, const QSize &imageSize, const QString &output) {
    if (!QRect(QPoint(),imageSize).contains(region) || region.isEmpty()) return;
    settings_.setValue("capture/last_region",region); settings_.setValue("capture/last_size",imageSize); settings_.setValue("capture/last_output",output); sync();
}
namespace {
QString pictures() {
    auto path = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return path.isEmpty() ? QDir::homePath() : path;
}
}
LinuxSettings::LinuxSettings(const QString &fileName) : settings_(configFile(fileName), QSettings::IniFormat) {}
QString LinuxSettings::fileName() const { return settings_.fileName(); }
bool LinuxSettings::showMainWindowOnStartup() const {
    // 首次运行显示设置；保留 Linux 已有用户启动即打开设置的行为，直到主动关闭。
    // Keep the existing visible startup until the user explicitly opts into background startup.
    return settings_.value("app/show_main_window", true).toBool();
}
void LinuxSettings::setShowMainWindowOnStartup(bool show) { settings_.setValue("app/show_main_window", show); }
QString LinuxSettings::language() const {
    const auto saved = settings_.value("app/language", "zh").toString();
    return jietuba::I18nManager::LANGUAGES().contains(saved) ? saved : QStringLiteral("zh");
}
void LinuxSettings::setLanguage(const QString &language) {
    if (jietuba::I18nManager::LANGUAGES().contains(language)) settings_.setValue("app/language", language);
}
QString LinuxSettings::importPreferences(const QString &path) {
    // 只映射当前版本支持的便携选项。不搬运本机路径、选区状态或桌面授予的快捷键。
    // Import only supported portable settings, leaving paths, capture state and desktop grants alone.
    static const QMap<QString, QString> aliases{
        {"appearance/theme", "app/ui_theme_mode"}, {"appearance/border_width", "app/selection_border_width"},
        {"capture/image_format", "app/screenshot_format"}, {"toolbar/screenshot_layout", "app/screenshot_toolbar_layout"},
        {"toolbar/pin_layout", "pin/toolbar_layout"}, {"toolbar/annotation_layout", "app/annotation_toolbar_layout"},
        {"magnifier/enabled", "app/magnifier_enabled"}, {"magnifier/grid", "app/magnifier_grid"},
        {"magnifier/selection", "app/magnifier_selection"}, {"magnifier/hints", "app/magnifier_hint"},
        {"translation/provider", "translation/active_provider"}, {"translation/api_key", "app/deepl_api_key"},
        {"translation/google_api_key", "translation/providers/google/api_key"},
        {"translation/youdao_app_id", "translation/providers/youdao/app_key"},
        {"translation/youdao_app_secret", "translation/providers/youdao/app_secret"},
        {"translation/target", "app/translation_target_lang"}
    };
    static const QMap<QString, QString> shortcutAliases{
        {"copy", "confirm"}, {"save", "save"}, {"quick_save", "quick_save"}, {"pin", "pin"},
        {"undo", "undo"}, {"redo", "redo"}, {"delete", "delete"}, {"restore_region", "restore_last_region"},
        {"translate", "translate"}, {"ocr", "text_recognize"}, {"pin_copy", "copy_pin"},
        {"pin_toggle_toolbar", "toggle_toolbar"}, {"pin_reset", "pin_reset_size"}
    };
    QStringList keys = aliases.keys() + QStringList{
        "app/show_main_window", "app/language", "capture/all_screens", "pin/auto_toolbar", "pin/auto_border",
        "pin/hover_buttons", "pin/zoom_step", "pin/default_opacity", "pin/click_through_opacity",
        "pin/rounded_corners", "pin/border_resize", "pin/auto_shrink", "pin/fine_zoom_step", "pin/opacity_step", "pin/zoom_anchor",
        "pin/lock_block_move", "pin/lock_block_click_close", "pin/lock_block_key_close", "pin/lock_hint",
        "pin/text_formatting", "pin/point_out_repeats", "app/ocr_enabled", "magnifier/zoom", "magnifier/formats", "recording/gif_fps",
        "recording/delay", "recording/format", "recording/video_fps", "recording/video_bitrate",
        "recording/video_audio", "recording/video_hardware", "recording/video_cursor", "translation/endpoint",
        "watermark/text", "watermark/style"
    };
    for (const auto &action : linuxShortcutActions()) keys.append("shortcuts/" + action.id);
    for (const QString &tool : {QString("pen"), QString("highlighter"), QString("rect"), QString("ellipse"),
                               QString("arrow"), QString("text"), QString("mosaic"), QString("number"),
                               QString("spotlight"), QString("eraser")}) keys.append("tools/" + tool);

    QVariantMap source;
    try {
        source = jietuba::settings_transfer::read_settings_file(path, keys);
    } catch (const jietuba::settings_transfer::SettingsFileError &) {
        return QCoreApplication::translate("SettingsDialog", "This file is not a Jietuba settings file, or it is damaged.");
    }

    QVariantMap imported;
    for (const auto &key : keys) {
        // 快捷键在下面统一校验，避免逐项应用时误判组合键交换。
        // Shortcuts are validated together below so two actions can swap keys.
        if (key.startsWith("shortcuts/")) continue;
        const QString alias = aliases.value(key);
        const bool native = source.contains(key);
        auto value = source.value(native ? key : alias);
        if (!value.isValid()) continue;
        if (key == "capture/image_format") {
            const auto format = value.toString().toLower();
            if (format != "jpg" && format != "bmp" && format != "png") continue;
            value = format;
        } else if (key == "translation/provider" && !native) {
            const auto provider = value.toString();
            if (provider == "deepl") value = source.value("app/deepl_use_pro").toBool() ? "deepl_pro" : "deepl_free";
            else if (provider != "google" && provider != "youdao") continue;
        } else if (key == "translation/target" && !native) {
            auto target = value.toString().toLower().section('-', 0, 0);
            if (!QStringList{"zh", "en", "ja", "ko"}.contains(target)) continue;
            value = target;
        } else if (key == "app/language" && !jietuba::I18nManager::LANGUAGES().contains(value.toString())) {
            continue;
        } else if (key.startsWith("toolbar/") || key.startsWith("tools/")) {
            const auto data = value.toByteArray();
            const bool layout = key.startsWith("toolbar/");
            const auto document = QJsonDocument::fromJson(data);
            if (data.size() > (layout ? 65536 : 16384) || (layout ? !document.isArray() : !document.isObject())) continue;
            value = data;
        }
        imported.insert(key, value);
    }

    // 同一批次校验快捷键，允许同时交换两项按键，拒绝在同一上下文制造冲突。
    // Validate shortcuts as a batch so swaps work and collisions never reach the event router.
    QVariantMap importedShortcuts;
    QSet<QString> assigned;
    bool conflict = false;
    for (const auto &action : linuxShortcutActions()) {
        const auto key = "shortcuts/" + action.id;
        const auto legacy = shortcutAliases.value(action.id, linuxIsToolShortcut(action.id) ? "tool_" + action.id : QString());
        const auto raw = source.value(key, legacy.isEmpty() ? QVariant() : source.value("inapp/inapp_" + legacy));
        QString binding = shortcut(action.id);
        if (raw.isValid()) {
            const auto text = raw.toString();
            const auto normalized = linuxNormalizeShortcut(text);
            if (text.isEmpty() || !normalized.isEmpty()) {
                binding = normalized;
                importedShortcuts.insert(key, binding);
            }
        }
        if (binding.isEmpty()) continue;
        const auto contextBinding = action.context + ':' + binding;
        conflict |= assigned.contains(contextBinding);
        assigned.insert(contextBinding);
    }
    if (!conflict) imported.insert(importedShortcuts);
    if (imported.isEmpty())
        return QCoreApplication::translate("SettingsDialog", "This file is not a Jietuba settings file, or it is damaged.");

    QVariantMap previous;
    for (auto it = imported.cbegin(); it != imported.cend(); ++it) {
        previous.insert(it.key(), settings_.value(it.key()));
        settings_.setValue(it.key(), it.value());
    }
    if (sync() == QSettings::NoError) return {};
    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
        if (it.value().isValid()) settings_.setValue(it.key(), it.value());
        else settings_.remove(it.key());
    }
    settings_.sync();
    return QCoreApplication::translate("SettingsDialog", "Error") + QStringLiteral(": ") + fileName();
}
bool LinuxSettings::hasGlobalShortcutSlots(const QString &action) const {return settings_.contains("global_shortcuts/"+action);}
QStringList LinuxSettings::globalShortcutSlots(const QString &action) const {
    QStringList keys=settings_.value("global_shortcuts/"+action).toStringList();
    if(!hasGlobalShortcutSlots(action)) {
        const QMap<QString,QString> defaults{{"capture","Alt+1"},{"pin_clipboard","Alt+2"},{"translate","Alt+3"},{"screen_annotation","Alt+4"}};
        keys<<defaults.value(action);
    }
    while(keys.size()<4) keys.append(QString()); return keys.mid(0,4);
}
void LinuxSettings::setGlobalShortcutSlots(const QString &action,const QStringList &values) {
    if(!QStringList{"capture","pin_clipboard","translate","screen_annotation","ocr"}.contains(action)) return;
    settings_.setValue("global_shortcuts/"+action,values.mid(0,4)); sync();
}
QString LinuxSettings::saveDirectory() const {
    const QString saved = settings_.value(QStringLiteral("capture/save_directory"), pictures()).toString();
    return saved.isEmpty() ? pictures() : saved;
}
void LinuxSettings::setSaveDirectory(const QString &path) { if (!path.trimmed().isEmpty()) settings_.setValue(QStringLiteral("capture/save_directory"), QDir::cleanPath(path)); }
QString LinuxSettings::imageFormat() const {
    const auto value = settings_.value(QStringLiteral("capture/image_format"), QStringLiteral("png")).toString().toLower();
    return value == "jpg" || value == "bmp" ? value : QStringLiteral("png");
}
void LinuxSettings::setImageFormat(const QString &format) { settings_.setValue(QStringLiteral("capture/image_format"), format == "jpg" || format == "bmp" ? format : QStringLiteral("png")); }
namespace {
struct CapturePreference { const char *key; bool initial; };
constexpr CapturePreference capturePrefs[] = {{"capture_include_cursor",false},{"capture_fullscreen_crosshair",false},
    {"screenshot_save_enabled",false},{"screenshot_pin_save_enabled",false},{"screenshot_save_original_enabled",false},
    {"clipboard_file_reference_enabled",true},{"smart_selection_animation",true},{"ocr_direct_copy",false},
    {"barcode_direct_copy",false},{"screenshot_watermark_auto",false}};
}
bool LinuxSettings::captureFlag(CaptureFlag flag) const {
    const auto &p=capturePrefs[static_cast<int>(flag)]; return settings_.value(QStringLiteral("app/")+p.key,p.initial).toBool();
}
void LinuxSettings::setCaptureFlag(CaptureFlag flag,bool value) { settings_.setValue(QStringLiteral("app/")+capturePrefs[static_cast<int>(flag)].key,value); }
QString LinuxSettings::captureNamePattern() const {
    const auto p=settings_.value("capture/name_pattern").toString();
    return jietuba::file_names::problem(p,jietuba::file_names::Screenshot).isEmpty() ? p : QStringLiteral("截图_{time}_{ms}");
}
bool LinuxSettings::setCaptureNamePattern(const QString &p) {
    if (!jietuba::file_names::problem(p,jietuba::file_names::Screenshot).isEmpty()) return false;
    settings_.setValue("capture/name_pattern",p); return true;
}
QString LinuxSettings::smartSelectionMode() const {
    const auto v=settings_.value("app/smart_selection_mode", "window").toString(); return v=="element" || v=="off" ? v : QStringLiteral("window");
}
void LinuxSettings::setSmartSelectionMode(const QString &v) { if(QStringList{"element","window","off"}.contains(v)) settings_.setValue("app/smart_selection_mode",v); }
QString LinuxSettings::ocrPosition() const {
    const auto v=settings_.value("app/ocr_result_position", "center").toString(); return v=="region" || v=="cursor" ? v : QStringLiteral("center");
}
void LinuxSettings::setOcrPosition(const QString &v) { if(QStringList{"region","cursor","center"}.contains(v)) settings_.setValue("app/ocr_result_position",v); }
QString LinuxSettings::ocrEngine() const {
    const auto v = settings_.value("app/ocr_engine", "auto").toString();
    return QStringList{"auto", "ppocr", "tesseract"}.contains(v) ? v : QStringLiteral("auto");
}
void LinuxSettings::setOcrEngine(const QString &v) { 
    if(QStringList{"auto", "ppocr", "tesseract"}.contains(v)) 
        settings_.setValue("app/ocr_engine", v); 
}

// 屏幕标注设置
bool LinuxSettings::keepAnnotationWindow() const { 
    return settings_.value("annotation/keep_window", false).toBool(); 
}
void LinuxSettings::setKeepAnnotationWindow(bool value) { 
    settings_.setValue("annotation/keep_window", value); 
}
bool LinuxSettings::showAnnotationToolbarOnHover() const { 
    return settings_.value("annotation/toolbar_on_hover", false).toBool(); 
}
void LinuxSettings::setShowAnnotationToolbarOnHover(bool value) { 
    settings_.setValue("annotation/toolbar_on_hover", value); 
}
bool LinuxSettings::highlightCursorOnStart() const { 
    return settings_.value("annotation/highlight_cursor", false).toBool(); 
}
void LinuxSettings::setHighlightCursorOnStart(bool value) { 
    settings_.setValue("annotation/highlight_cursor", value); 
}

// 翻译设置
bool LinuxSettings::ignoreLineBreaksInTranslation() const { 
    return settings_.value("translation/ignore_line_breaks", false).toBool(); 
}
void LinuxSettings::setIgnoreLineBreaksInTranslation(bool value) { 
    settings_.setValue("translation/ignore_line_breaks", value); 
}
bool LinuxSettings::keepOriginalFormat() const { 
    return settings_.value("translation/keep_original_format", false).toBool(); 
}
void LinuxSettings::setKeepOriginalFormat(bool value) { 
    settings_.setValue("translation/keep_original_format", value); 
}

// 外观设置
QString LinuxSettings::themeColor() const {
    return settings_.value("appearance/theme_color", "#0078D4").toString();
}
void LinuxSettings::setThemeColor(const QString &color) {
    settings_.setValue("appearance/theme_color", color);
}
QString LinuxSettings::outsideSelectionColor() const {
    return settings_.value("appearance/outside_selection_color", "#000000").toString();
}
void LinuxSettings::setOutsideSelectionColor(const QString &color) {
    settings_.setValue("appearance/outside_selection_color", color);
}
bool LinuxSettings::showSelectionPreview() const {
    return settings_.value("appearance/show_selection_preview", true).toBool();
}
void LinuxSettings::setShowSelectionPreview(bool value) {
    settings_.setValue("appearance/show_selection_preview", value);
}

// 日志设置
bool LinuxSettings::enableLogging() const {
    return settings_.value("log/enabled", false).toBool();
}
void LinuxSettings::setEnableLogging(bool value) {
    settings_.setValue("log/enabled", value);
}
bool LinuxSettings::saveLogToFile() const {
    return settings_.value("log/save_to_file", false).toBool();
}
void LinuxSettings::setSaveLogToFile(bool value) {
    settings_.setValue("log/save_to_file", value);
}
QString LinuxSettings::logLevel() const {
    const auto v = settings_.value("log/level", "info").toString();
    return QStringList{"debug", "info", "warning", "error"}.contains(v) ? v : "info";
}
void LinuxSettings::setLogLevel(const QString &level) {
    if (QStringList{"debug", "info", "warning", "error"}.contains(level))
        settings_.setValue("log/level", level);
}
int LinuxSettings::logRetentionDays() const {
    const int days = settings_.value("log/retention_days", 7).toInt();
    return days >= 1 && days <= 365 ? days : 7;
}
void LinuxSettings::setLogRetentionDays(int days) {
    if (days >= 1 && days <= 365)
        settings_.setValue("log/retention_days", days);
}
QString LinuxSettings::logDirectory() const {
    QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    return settings_.value("log/directory", defaultDir).toString();
}
void LinuxSettings::setLogDirectory(const QString &path) {
    if (!path.trimmed().isEmpty())
        settings_.setValue("log/directory", QDir::cleanPath(path));
}

bool LinuxSettings::crossToolSelection() const { return settings_.value("app/cross_tool_selection", true).toBool(); }
void LinuxSettings::setCrossToolSelection(bool value) { settings_.setValue("app/cross_tool_selection", value); }
bool LinuxSettings::textAlwaysOnTop() const { return settings_.value("app/text_always_on_top", true).toBool(); }
void LinuxSettings::setTextAlwaysOnTop(bool value) { settings_.setValue("app/text_always_on_top", value); }
int LinuxSettings::shiftLineAngle() const {
    bool ok = false;
    const int degrees = settings_.value("app/shift_line_angle").toInt(&ok);
    return ok && jietuba::SHIFT_LINE_ANGLES.contains(degrees) ? degrees : jietuba::DEFAULT_SHIFT_LINE_ANGLE;
}
void LinuxSettings::setShiftLineAngle(int degrees) {
    settings_.setValue("app/shift_line_angle", jietuba::SHIFT_LINE_ANGLES.contains(degrees) ? degrees : jietuba::DEFAULT_SHIFT_LINE_ANGLE);
}
bool LinuxSettings::captureAllScreens() const { return settings_.value(QStringLiteral("capture/all_screens"), true).toBool(); }
void LinuxSettings::setCaptureAllScreens(bool value) { settings_.setValue(QStringLiteral("capture/all_screens"), value); }
QString LinuxSettings::theme() const {
    const auto value = settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
    return value == "light" || value == "dark" ? value : QStringLiteral("system");
}
void LinuxSettings::setTheme(const QString &value) { settings_.setValue(QStringLiteral("appearance/theme"), value == "light" || value == "dark" ? value : QStringLiteral("system")); }
int LinuxSettings::borderWidth() const {
    const int width = settings_.value(QStringLiteral("appearance/border_width"), 4).toInt();
    return QList<int>{1, 2, 3, 4, 5, 6, 8, 10}.contains(width) ? width : 4;
}
void LinuxSettings::setBorderWidth(int value) { settings_.setValue(QStringLiteral("appearance/border_width"), std::clamp(value, 1, 10)); }
bool LinuxSettings::pinBorderEnabled() const { return settings_.value(QStringLiteral("pin/auto_border"), true).toBool(); }
// 与 Windows 一样，新配置默认只显示贴图；保留已保存的悬停偏好。
// Match Windows for fresh settings while honoring saved hover preferences.
bool LinuxSettings::pinAutoToolbar() const { return settings_.value(QStringLiteral("pin/auto_toolbar"), false).toBool(); }
void LinuxSettings::setPinAutoToolbar(bool value) { settings_.setValue(QStringLiteral("pin/auto_toolbar"), value); }
bool LinuxSettings::pinHoverButtons() const { return settings_.value(QStringLiteral("pin/hover_buttons"), pinAutoToolbar()).toBool(); }
void LinuxSettings::setPinHoverButtons(bool value) { settings_.setValue(QStringLiteral("pin/hover_buttons"), value); }
void LinuxSettings::setPinBorderEnabled(bool value) { settings_.setValue(QStringLiteral("pin/auto_border"), value); }
namespace {
const QMap<QString,bool> pinFlags{{"lock_block_move",true},{"lock_block_click_close",true},{"lock_block_key_close",true},{"lock_hint",true},
    {"rounded_corners",false},{"auto_shrink",true},{"border_resize",true},{"auto_ocr",true},{"point_out_repeats",true},{"text_formatting",true}};
const QMap<QString,QList<int>> pinSteps{{"default_opacity",{100,90,80,70,60,50,40,30}}, {"click_through_opacity",{50,90,80,70,60,40,30,20}},
    {"fine_zoom_step",{1,2,3,5}},{"opacity_step",{5,1,2,10}}};
const QStringList pinAnchors{"adaptive","top_left","top_right","bottom_left","bottom_right","center","mouse"};
}
bool LinuxSettings::pinFlag(const QString &k) const { return pinFlags.contains(k) && settings_.value(k=="auto_ocr" ? QStringLiteral("app/ocr_enabled") : "pin/"+k,pinFlags.value(k)).toBool(); }
void LinuxSettings::setPinFlag(const QString &k,bool v) { if(pinFlags.contains(k)) settings_.setValue(k=="auto_ocr" ? QStringLiteral("app/ocr_enabled") : "pin/"+k,v); }
int LinuxSettings::pinPercent(const QString &k) const {
    const auto values=pinSteps.value(k); if(values.isEmpty()) return 0;
    const auto raw=settings_.value("pin/"+k); bool ok=false; const double n=raw.toDouble(&ok);
    const int value=ok && std::isfinite(n) && n>=0 && n<=100 ? qRound(n*(k.endsWith("_opacity") ? 100 : 1)) : -1;
    return values.contains(value) ? value : values.first();
}
void LinuxSettings::setPinPercent(const QString &k,int v) { if(pinSteps.value(k).contains(v)) settings_.setValue("pin/"+k,k.endsWith("_opacity") ? v/100.0 : double(v)); }
QString LinuxSettings::pinZoomAnchor() const { const auto v=settings_.value("pin/zoom_anchor").toString(); return pinAnchors.contains(v) ? v : "adaptive"; }
void LinuxSettings::setPinZoomAnchor(const QString &v) { if(pinAnchors.contains(v)) settings_.setValue("pin/zoom_anchor",v); }
int LinuxSettings::pinZoomStep() const {
    const int step = settings_.value(QStringLiteral("pin/zoom_step"), 10).toInt();
    return QList<int>{5, 10, 15, 20, 25, 50}.contains(step) ? step : 10;
}
void LinuxSettings::setPinZoomStep(int value) {
    settings_.setValue(QStringLiteral("pin/zoom_step"), QList<int>{5, 10, 15, 20, 25, 50}.contains(value) ? value : 10);
}
QString LinuxSettings::watermarkText() const { return settings_.value(QStringLiteral("watermark/text")).toString(); }
void LinuxSettings::setWatermarkText(const QString &value) { settings_.setValue(QStringLiteral("watermark/text"), value); }
QVariantMap LinuxSettings::watermarkStyle() const { return settings_.value(QStringLiteral("watermark/style")).toMap(); }
void LinuxSettings::setWatermarkStyle(const QVariantMap &value) { settings_.setValue(QStringLiteral("watermark/style"), value); }
QString LinuxSettings::translationProvider() const {
    const QString value = settings_.value(QStringLiteral("translation/provider"), QStringLiteral("deepl_free")).toString();
    return QStringList{QStringLiteral("deepl_pro"), QStringLiteral("libretranslate"), QStringLiteral("google"), QStringLiteral("youdao")}.contains(value)
        ? value : QStringLiteral("deepl_free");
}
void LinuxSettings::setTranslationProvider(const QString &value) {
    settings_.setValue(QStringLiteral("translation/provider"),
        QStringList{QStringLiteral("deepl_pro"), QStringLiteral("libretranslate"), QStringLiteral("google"), QStringLiteral("youdao")}.contains(value)
            ? value : QStringLiteral("deepl_free"));
}
QString LinuxSettings::translationApiKey() const { return settings_.value(QStringLiteral("translation/api_key")).toString(); }
void LinuxSettings::setTranslationApiKey(const QString &value) { settings_.setValue(QStringLiteral("translation/api_key"), value.trimmed()); }
QString LinuxSettings::googleApiKey() const { return settings_.value(QStringLiteral("translation/google_api_key")).toString(); }
void LinuxSettings::setGoogleApiKey(const QString &value) { settings_.setValue(QStringLiteral("translation/google_api_key"), value.trimmed()); }
QString LinuxSettings::youdaoAppId() const { return settings_.value(QStringLiteral("translation/youdao_app_id")).toString(); }
void LinuxSettings::setYoudaoAppId(const QString &value) { settings_.setValue(QStringLiteral("translation/youdao_app_id"), value.trimmed()); }
QString LinuxSettings::youdaoAppSecret() const { return settings_.value(QStringLiteral("translation/youdao_app_secret")).toString(); }
void LinuxSettings::setYoudaoAppSecret(const QString &value) { settings_.setValue(QStringLiteral("translation/youdao_app_secret"), value.trimmed()); }
QString LinuxSettings::translationEndpoint() const { return settings_.value(QStringLiteral("translation/endpoint")).toString().trimmed(); }
void LinuxSettings::setTranslationEndpoint(const QString &value) { settings_.setValue(QStringLiteral("translation/endpoint"), value.trimmed()); }
QString LinuxSettings::translationTarget() const {
    const QString value = settings_.value(QStringLiteral("translation/target"), QStringLiteral("zh")).toString();
    return QStringList{QStringLiteral("zh"), QStringLiteral("en"), QStringLiteral("ja"), QStringLiteral("ko")}.contains(value) ? value : QStringLiteral("zh");
}
void LinuxSettings::setTranslationTarget(const QString &value) {
    if (QStringList{QStringLiteral("zh"), QStringLiteral("en"), QStringLiteral("ja"), QStringLiteral("ko")}.contains(value))
        settings_.setValue(QStringLiteral("translation/target"), value);
}
int LinuxSettings::gifFps() const {
    const int value = settings_.value(QStringLiteral("recording/gif_fps"), 16).toInt();
    return QList<int>{5,10,16,24,30}.contains(value) ? value : 16;
}
QString LinuxSettings::recordingMode() const {return settings_.value("recording/format","gif").toString()=="mp4" ? "mp4" : "gif";}
void LinuxSettings::setRecordingMode(const QString &mode) {if(mode=="gif" || mode=="mp4") settings_.setValue("recording/format",mode);}
jietuba::RecordingOptions LinuxSettings::videoOptions() const {
    jietuba::RecordingOptions value;
    const int fps=settings_.value("recording/video_fps",value.fps).toInt(),bitrate=settings_.value("recording/video_bitrate",value.bitrate).toInt();
    if(fps>=1 && fps<=60) value.fps=fps;
    if(bitrate>=500000 && bitrate<=50000000) value.bitrate=bitrate;
    value.system_audio=settings_.value("recording/video_audio",value.system_audio).toBool();
    value.hardware=settings_.value("recording/video_hardware",value.hardware).toBool();
    value.cursor=settings_.value("recording/video_cursor",value.cursor).toBool(); return value;
}
void LinuxSettings::setVideoOptions(const jietuba::RecordingOptions &value) {
    value.validate(); settings_.setValue("recording/video_fps",value.fps); settings_.setValue("recording/video_bitrate",value.bitrate);
    settings_.setValue("recording/video_audio",value.system_audio); settings_.setValue("recording/video_hardware",value.hardware); settings_.setValue("recording/video_cursor",value.cursor);
}
void LinuxSettings::setGifFps(int value) { if (QList<int>{5,10,16,24,30}.contains(value)) settings_.setValue(QStringLiteral("recording/gif_fps"), value); }
int LinuxSettings::gifDelay() const {
    const int value = settings_.value(QStringLiteral("recording/delay"), 0).toInt();
    return QList<int>{0,1,2,3,5,10}.contains(value) ? value : 0;
}
void LinuxSettings::setGifDelay(int value) { if (QList<int>{0,1,2,3,5,10}.contains(value)) settings_.setValue(QStringLiteral("recording/delay"), value); }
QJsonArray LinuxSettings::screenshotToolbarLayout() const {
    const QByteArray data = settings_.value(QStringLiteral("toolbar/screenshot_layout")).toByteArray();
    // 只解析小型布局数组。缺失/损坏返回空数组，共用规范化函数会补回原版默认按钮。
    // Parse only a small layout array; missing/corrupt data normalizes to original defaults.
    if (data.size()>64*1024) return {};
    return QJsonDocument::fromJson(data).array();
}
bool LinuxSettings::saveScreenshotToolbarLayout(const QJsonArray &layout) {
    const QString key = QStringLiteral("toolbar/screenshot_layout");
    const QVariant previous = settings_.value(key);
    settings_.setValue(key,QJsonDocument(layout).toJson(QJsonDocument::Compact));
    if (sync()==QSettings::NoError) return true;
    // 写盘失败时回退内存值，不能让下一次刷新误认为保存已经成功。
    // Roll back the in-memory value on failure so a later refresh cannot pretend it was saved.
    if (previous.isValid()) settings_.setValue(key,previous); else settings_.remove(key);
    settings_.sync(); return false;
}
QJsonArray LinuxSettings::pinToolbarLayout() const {
    const QByteArray data = settings_.value(QStringLiteral("toolbar/pin_layout")).toByteArray();
    // 与截图布局使用相同的损坏/超大输入保护，但不共享存储键。
    // Apply the same corrupt/oversized input guard as capture layouts, with an independent key.
    return data.size() <= 64 * 1024 ? QJsonDocument::fromJson(data).array() : QJsonArray{};
}
bool LinuxSettings::savePinToolbarLayout(const QJsonArray &layout) {
    const QByteArray data = QJsonDocument(layout).toJson(QJsonDocument::Compact);
    if (data.size() > 64 * 1024) return false;
    const QString key = QStringLiteral("toolbar/pin_layout");
    const QVariant previous = settings_.value(key);
    settings_.setValue(key, data);
    if (sync() == QSettings::NoError) return true;
    // 写盘失败必须恢复旧值，允许调整窗口继续重试。
    // Roll back failed writes so the customization editor can retry without false success.
    if (previous.isValid()) settings_.setValue(key, previous); else settings_.remove(key);
    settings_.sync(); return false;
}
QJsonArray LinuxSettings::annotationToolbarLayout() const {
    const QByteArray data = settings_.value(QStringLiteral("toolbar/annotation_layout")).toByteArray();
    // 与截图布局使用相同的损坏/超大输入保护，但不共享存储键。
    // Apply the same corrupt/oversized input guard as capture layouts, with an independent key.
    return data.size() <= 64 * 1024 ? QJsonDocument::fromJson(data).array() : QJsonArray{};
}
bool LinuxSettings::saveAnnotationToolbarLayout(const QJsonArray &layout) {
    const QByteArray data = QJsonDocument(layout).toJson(QJsonDocument::Compact);
    if (data.size() > 64 * 1024) return false;
    const QString key = QStringLiteral("toolbar/annotation_layout");
    const QVariant previous = settings_.value(key);
    settings_.setValue(key, data);
    if (sync() == QSettings::NoError) return true;
    // 写盘失败必须恢复旧值，允许调整窗口继续重试。
    // Roll back failed writes so the customization editor can retry without false success.
    if (previous.isValid()) settings_.setValue(key, previous); else settings_.remove(key);
    settings_.sync(); return false;
}
QSettings::Status LinuxSettings::sync() {
    settings_.sync();
    // API 密钥保存在 INI 中，写盘后只允许当前用户读取该文件。
    // The API key lives in the INI file; restrict it to the current user after writing.
    if (settings_.status() == QSettings::NoError && QFile::exists(settings_.fileName())
        && !QFile::setPermissions(settings_.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return QSettings::AccessError;
    return settings_.status();
}
