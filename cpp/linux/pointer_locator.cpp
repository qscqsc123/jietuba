#include "pointer_locator.h"
#include "wayland_surface_support.h"
#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
namespace {
/**
 * 只接收一次 pointer enter/move 的透明表面。只使用局部坐标，避开 Qt 合成的 globalPosition。
 * A transparent one-shot enter/move surface; local coordinates avoid Qt's synthesized global position.
 */
class PointerProbe final : public QWidget {
public:
    std::function<void(const QPointF &)> entered;
    explicit PointerProbe(QScreen *screen) {
        setObjectName("linuxPointerProbe");
        setWindowFlags(Qt::Window|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setMouseTracking(true);
        resize(screen->geometry().size()); winId(); windowHandle()->setScreen(screen); windowHandle()->installEventFilter(this);
#ifdef JIETUBA_HAS_LAYER_SHELL
        if(auto *layer=LayerShellQt::Window::get(windowHandle())) {
            layer->setScope("jietuba-pointer-locator"); layer->setScreen(screen);
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)|LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft|LayerShellQt::Window::AnchorRight);
            layer->setExclusiveZone(-1); layer->setDesiredSize(size());
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        }
#endif
    }
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        // 在原生 QWindow 入口读取位置，避免 QWidget 为子控件重新从合成的全局坐标换算 enter。
        // Read native-window coordinates before QWidget reconstructs child enter positions from global coordinates.
        if(object==windowHandle() && entered) {
            if(event->type()==QEvent::Enter) {entered(static_cast<QEnterEvent *>(event)->position()); return true;}
            if(event->type()==QEvent::MouseMove) {entered(static_cast<QMouseEvent *>(event)->position()); return true;}
        }
        return QWidget::eventFilter(object,event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(),Qt::transparent);
    }
};
}
LinuxPointerLocator::LinuxPointerLocator(QObject *parent):QObject(parent) {}
LinuxPointerLocator::~LinuxPointerLocator() {clear();}
QPoint LinuxPointerLocator::globalPosition(const QRect &output,const QPointF &local) {
    // enter 的单位已经是 Qt 逻辑像素，不能再次乘 DPR；保留负坐标输出的原点。
    // Enter positions are logical pixels already; preserve negative output origins without multiplying DPR.
    return output.topLeft()+local.toPoint();
}
void LinuxPointerLocator::locate(Completion completion) {
    clear(); completion_=std::move(completion); resolved_=false;
    if(!LinuxWayland::supportsLayerShell()) {
        QTimer::singleShot(0,this,[this] {const auto pos=QCursor::pos(); resolve(QGuiApplication::screenAt(pos),pos);}); return;
    }
    for(auto *screen:QGuiApplication::screens()) {
        auto *probe=new PointerProbe(screen); probes_.append(probe);
        const QPointer<QScreen> output(screen);
        probe->entered=[this,output](const QPointF &local) {
            if(output) resolve(output,globalPosition(output->geometry(),local));
        };
    }
    // 先准备所有输出，再统一映射，避免第一个输出的 enter 触发清理尚在构造的列表。
    // Prepare every output before mapping; completion is deferred until the enter event returns.
    const auto probes=probes_; for(const auto &probe:probes) if(probe && !resolved_) probe->show();
    QTimer::singleShot(600,this,[this] {
        if(resolved_) return;
        const auto pos=QCursor::pos(); resolve(QGuiApplication::screenAt(pos),pos);
    });
}
void LinuxPointerLocator::resolve(QScreen *screen,const QPoint &position) {
    if(resolved_) return;
    resolved_=true;
    const QPointer<QScreen> output(screen ? screen : QGuiApplication::primaryScreen());
    // 先撤掉输入层；回调和删除延后一轮，不能在 QWidget 自己的 enter 事件栈中销毁它。
    // Withdraw input first; defer completion/deletion beyond the probe's own event stack.
    for(auto &probe:probes_) if(probe) probe->hide();
    QTimer::singleShot(0,this,[this,output,position] {
        clear(); auto completion=std::move(completion_);
        if(completion && output) completion(output,position);
        deleteLater();
    });
}
void LinuxPointerLocator::clear() {
    for(auto &probe:probes_) if(probe) {static_cast<PointerProbe *>(probe.data())->entered={}; probe->hide(); probe->deleteLater();}
    probes_.clear();
}
