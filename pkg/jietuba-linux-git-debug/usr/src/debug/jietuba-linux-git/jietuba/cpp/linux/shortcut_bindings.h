#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QKeySequence>
#include <QSet>
#include <functional>

class QWidget;
class QKeyEvent;
class LinuxSettings;

/**
 * 一个可配置的应用内动作；相同上下文内按键不能重复，截图与贴图可分别使用 Ctrl+C。
 * 默认按键沿用 Windows；不属于已经移植功能的动作不显示空壳入口。
 * A configurable local action. Bindings are unique per context, so capture and pin
 * can both use Ctrl+C. Defaults follow Windows; only implemented actions are exposed.
 */
struct LinuxShortcutAction { QString id, title, context, defaultKey; };
inline const QList<LinuxShortcutAction> &linuxShortcutActions() {
    static const QList<LinuxShortcutAction> actions{
        {"copy","复制选区","capture","Ctrl+C"}, {"save","保存图片","capture","Ctrl+S"},
        {"quick_save","快速保存","capture","Ctrl+Shift+S"}, {"pin","钉图","capture","mousemiddle"},
        {"undo","撤销","capture","Ctrl+Z"}, {"redo","重做","capture","Ctrl+Y"},
        {"delete","删除选中图形","capture","Del"}, {"restore_region","恢复上次选区","capture","L"},
        {"translate","截图翻译","capture","Shift+C"}, {"ocr","文字识别","capture","Shift+T"},
        {"barcode","识别二维码","capture",""}, {"long_capture","长截图","capture",""},
        {"gif","GIF 录制","capture",""},
        {"cursor","选择工具","capture","S"}, {"pen","画笔","capture","P"},
        {"highlighter","荧光笔","capture","M"}, {"mosaic","马赛克","capture","X"},
        {"arrow","箭头","capture","A"}, {"number","序号","capture","N"},
        {"rect","矩形","capture","R"}, {"ellipse","椭圆","capture","O"},
        {"text","文字","capture","T"}, {"eraser","橡皮擦","capture","E"},
        {"spotlight","聚光灯","capture",""}, {"watermark","水印","capture",""},
        {"copy_color","复制光标颜色","capture","C"},
        {"zoom_in","放大镜放大","capture","PgUp"}, {"zoom_out","放大镜缩小","capture","PgDown"},
        {"pin_copy","复制贴图","pin","Ctrl+C"}, {"pin_save","保存贴图","pin","Ctrl+S"},
        {"pin_toggle_toolbar","切换贴图标注工具栏","pin","Space"},
        {"pin_undo","撤销贴图标注","pin","Ctrl+Z"}, {"pin_redo","重做贴图标注","pin","Ctrl+Y"},
        {"pin_lock","锁定或解锁贴图","pin","L"},
        {"pin_reset","贴图恢复原始大小","pin",""},
        {"pin_zoom_in","贴图放大","pin","Ctrl++"}, {"pin_zoom_out","贴图缩小","pin","Ctrl+-"},
    };
    return actions;
}

/**
 * 仅作用于宿主窗口的键盘路由，不申请 Wayland 全局监听权限。
 * 在 Qt 分发快捷键之前保留匹配事件；文本输入、弹出菜单、模态面板由原控件处理。
 * 每次读取配置，因此设置改动立即生效；回调决定当前状态是否允许执行该动作。
 * Window-local routing without Wayland global input hooks. Reserve matching Qt
 * events before shortcut dispatch, while leaving text inputs, popups and modal UI alone.
 * Read bindings on demand for immediate updates; the owner validates action availability.
 */
// 键盘与中键使用同一持久化入口；拒绝未知修饰符，避免显示一个不能执行的绑定。
// Normalize keyboard and middle-button bindings together, rejecting unknown modifier tokens.
inline QString linuxNormalizeShortcut(const QString &value) {
    const auto raw=value.trimmed(); if(raw.isEmpty()) return {};
    if(raw.endsWith("mousemiddle",Qt::CaseInsensitive)) {
        auto parts=raw.toLower().split('+'); if(parts.takeLast()!="mousemiddle") return {};
        Qt::KeyboardModifiers modifiers; QSet<QString> seen;
        for(const auto &part:parts) {
            if(seen.contains(part)) return {}; seen.insert(part);
            if(part=="ctrl") modifiers|=Qt::ControlModifier;
            else if(part=="shift") modifiers|=Qt::ShiftModifier;
            else if(part=="alt") modifiers|=Qt::AltModifier;
            else if(part=="meta" || part=="win") modifiers|=Qt::MetaModifier;
            else return {};
        }
        QStringList normalized;
        if(modifiers & Qt::ControlModifier) normalized<<"Ctrl";
        if(modifiers & Qt::AltModifier) normalized<<"Alt";
        if(modifiers & Qt::ShiftModifier) normalized<<"Shift";
        if(modifiers & Qt::MetaModifier) normalized<<"Meta";
        normalized<<"mousemiddle"; return normalized.join('+');
    }
    const auto sequence=QKeySequence::fromString(raw,QKeySequence::PortableText);
    if(sequence.count()!=1 || sequence[0].key()==Qt::Key_unknown || sequence[0].key()==0 || sequence[0].key()==Qt::Key_Escape) return {};
    return sequence.toString(QKeySequence::PortableText);
}
inline bool linuxIsToolShortcut(const QString &id) {
    return QStringList{"cursor","pen","highlighter","mosaic","arrow","number","rect","ellipse","text","eraser","spotlight","watermark","undo","redo"}.contains(id);
}
class LinuxShortcutRouter final : public QObject {
public:
    LinuxShortcutRouter(QWidget *owner, LinuxSettings *settings, QString context,
                        std::function<bool(const QString &)> invoke, std::function<bool()> blocked = {},
                        std::function<bool(QKeyEvent *)> fixedKey = {});
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QWidget *owner_;
    LinuxSettings *settings_;
    QString context_;
    std::function<bool(const QString &)> invoke_;
    std::function<bool()> blocked_;
    // 原版单 Shift 等固定动作先于普通按键分发，可在框选拖动期间单独判断是否有效。
    // Fixed actions such as lone Shift run before ordinary-action guards, including during crop dragging.
    std::function<bool(QKeyEvent *)> fixed_key_;
};
