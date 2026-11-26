#pragma once
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <functional>
class QScreen;
class QWidget;
/**
 * Wayland 光标定位器：后台快捷键触发时不能把 QCursor::pos 的旧缓存当成真实全局位置。
 * 在支持 layer-shell 的每个输出上短暂映射完全透明、无键盘焦点的输入表面，读取合成器
 * 发送的 pointer enter 局部逻辑坐标；取得位置就撤下全部表面，不截图、不绘制遮罩。
 * 不支持该协议时保留 Qt 位置回退，且超时必定释放输入表面，不阻塞桌面操作。
 * Wayland pointer locator for background shortcut activation. Transparent, keyboard-inert
 * layer surfaces obtain compositor pointer-enter coordinates on their actual output.
 * All probes unmap immediately after resolution; no capture or visible curtain is involved.
 * Unsupported compositors retain Qt fallback, with bounded timeout and deterministic cleanup.
 */
class LinuxPointerLocator final : public QObject {
public:
    using Completion=std::function<void(QScreen *,const QPoint &)>;
    explicit LinuxPointerLocator(QObject *parent=nullptr);
    ~LinuxPointerLocator() override;
    void locate(Completion completion);
    static QPoint globalPosition(const QRect &output,const QPointF &local);
private:
    void resolve(QScreen *screen,const QPoint &position);
    void clear();
    QList<QPointer<QWidget>> probes_;
    Completion completion_;
    bool resolved_=false;
};
