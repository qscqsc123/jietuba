#include "shortcut_bindings.h"
#include "linux_settings.h"
#include <QApplication>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>
#include <QWindow>

LinuxShortcutRouter::LinuxShortcutRouter(QWidget *owner, LinuxSettings *settings, QString context,
    std::function<bool(const QString &)> invoke, std::function<bool()> blocked, std::function<bool(QKeyEvent *)> fixedKey)
    : QObject(owner), owner_(owner), settings_(settings), context_(std::move(context)), invoke_(std::move(invoke)), blocked_(std::move(blocked)), fixed_key_(std::move(fixedKey)) {
    qApp->installEventFilter(this);
}
bool LinuxShortcutRouter::eventFilter(QObject *watched, QEvent *event) {
    if (event->type()!=QEvent::ShortcutOverride && event->type()!=QEvent::KeyPress && event->type()!=QEvent::MouseButtonPress) return false;
    auto *widget=qobject_cast<QWidget *>(watched);
    auto *surface=qobject_cast<QWindow *>(watched);
    if ((!widget || widget->window()!=owner_->window()) && (!surface || surface!=owner_->window()->windowHandle())) return false;
    if (!owner_->isVisible() || !owner_->isEnabled() || QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    // 只检查事件所在窗口的输入焦点。layer-shell 已接管键盘时，Qt 的全局 focusWidget
    // 可能暂时仍指向设置页的输入框，不能因此屏蔽截图窗口实际收到的 Shift。
    // Check inputs only in the event's owner. Layer-shell may own keyboard delivery
    // while Qt's global focusWidget still points to an editor in the settings window.
    for (QWidget *focus=QApplication::focusWidget(); focus && focus->window()==owner_->window(); focus=focus->parentWidget()) {
        if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus)
            || qobject_cast<QAbstractSpinBox *>(focus) || qobject_cast<QComboBox *>(focus) || qobject_cast<QKeySequenceEdit *>(focus)) return false;
        if (focus==owner_) break;
    }
    if(event->type()==QEvent::MouseButtonPress) {
        auto *mouse=static_cast<QMouseEvent *>(event);
        if(mouse->button()!=Qt::MiddleButton || !widget || (blocked_ && blocked_())) return false;
        QStringList parts;
        if(mouse->modifiers() & Qt::ControlModifier) parts<<"Ctrl";
        if(mouse->modifiers() & Qt::AltModifier) parts<<"Alt";
        if(mouse->modifiers() & Qt::ShiftModifier) parts<<"Shift";
        if(mouse->modifiers() & Qt::MetaModifier) parts<<"Meta";
        parts<<"mousemiddle";
        for(const auto &action:linuxShortcutActions()) {
            if(action.context!=context_) continue;
            const auto binding=settings_ ? settings_->shortcut(action.id) : action.defaultKey;
            if(binding.compare(parts.join('+'),Qt::CaseInsensitive)==0 && invoke_(action.id)) {event->accept(); return true;}
        }
        return false;
    }
    auto *key=static_cast<QKeyEvent *>(event);
    // 与 Windows 的截图统一处理器一致：在 QWidget/原生 QWindow 入口消费固定修饰键。
    // 一旦处理立即停止分发，不能再由视图和父窗口各切一次（两格式会切回原样）。
    // Like the Windows capture handler, consume fixed modifier actions at the widget/native surface entry.
    // Stop dispatch once handled; a view/parent double cycle would return two formats to their original state.
    if (fixed_key_ && fixed_key_(key)) { event->accept(); return true; }
    if (!widget || (blocked_ && blocked_())) return false;
    if (key->key()==Qt::Key_Escape || key->key()==Qt::Key_Shift || key->key()==Qt::Key_Control || key->key()==Qt::Key_Alt || key->key()==Qt::Key_Meta) return false;
    const QKeySequence pressed(key->keyCombination());
    for (const auto &action:linuxShortcutActions()) {
        if (action.context!=context_) continue;
        const QString binding=settings_ ? settings_->shortcut(action.id) : action.defaultKey;
        const auto configured=QKeySequence::fromString(binding,QKeySequence::PortableText);
        // “+”常由 Shift+= 输入；默认 Ctrl++ 应接受字符所需的隐式 Shift。
        // Plus often requires Shift+=; accept its implicit Shift for the default Ctrl++ binding.
        const bool implicitPlus=key->key()==Qt::Key_Plus && configured==QKeySequence(QKeyCombination(key->modifiers() & ~Qt::ShiftModifier,Qt::Key_Plus));
        if (binding.isEmpty() || (configured!=pressed && !implicitPlus)) continue;
        if (event->type()==QEvent::ShortcutOverride) { event->accept(); return true; }
        // 长按保存/确认不能重复打开对话框，缩放与删除等连续操作允许自动重复。
        // Suppress repeated modal/completion actions; allow repeated zoom and deletion.
        if (key->isAutoRepeat() && !action.id.contains("zoom") && action.id!="delete") return true;
        if (invoke_(action.id)) { event->accept(); return true; }
    }
    return false;
}
