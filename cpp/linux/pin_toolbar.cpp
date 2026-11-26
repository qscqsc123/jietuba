#include "pin_toolbar.h"
#include <QPainter>
#include "pin_window.h"
#include "linux_settings.h"
#include "shortcut_bindings.h"
#include "wayland_surface_support.h"
#include "ui/toolbar.h"
#include <QApplication>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QResizeEvent>
#include <QWindow>
#include <algorithm>
#include <functional>

namespace {
/**
 * 原版工具栏的 Linux 贴图适配：仅处理宿主内的拖动、复位和面板同步。
 * 不更改按钮、图标、配色与 PIN 配置规则；拖动使用局部坐标，避免 Wayland 全局坐标漂移。
 * Original toolbar adapted only for host-local dragging, reset and panel positioning.
 * Buttons, icons, colors and the PIN profile stay shared with Windows.
 */
class PinToolbarContent final : public jietuba::Toolbar {
public:
    explicit PinToolbarContent(QWidget *host) : Toolbar(host, jietuba::toolbar_layout::PIN) {}
    std::function<void()> positionRequested;
    bool manuallyPositioned() const { return manual_positioned_; }
    void syncPanels() { sync_all_panels_position(); }
protected:
    void reposition_self() override { if (positionRequested) positionRequested(); }
    void reset_auto_position() override { set_manual_positioned(false); reposition_self(); }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == drag_handle) {
            const auto type = event->type();
            if (type == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    dragging_ = true; drag_offset_ = mouse->position().toPoint();
                    drag_handle->setCursor(Qt::ClosedHandCursor); return true;
                }
            } else if (type == QEvent::MouseMove && dragging_) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->buttons() & Qt::LeftButton) {
                    const QPoint target = pos() + mouse->position().toPoint() - drag_offset_;
                    move(std::clamp(target.x(), 0, std::max(0, parentWidget()->width() - width())),
                         std::clamp(target.y(), 0, std::max(0, parentWidget()->height() - height())));
                    set_manual_positioned(true); reposition_self(); return true;
                }
                dragging_ = false;
            } else if (type == QEvent::MouseButtonRelease && dragging_) {
                dragging_ = false; drag_handle->setCursor(Qt::SizeAllCursor); return true;
            }
        }
        return Toolbar::eventFilter(watched, event);
    }
};
}

LinuxPinToolbar::LinuxPinToolbar(LinuxPinWindow *pin, LinuxSettings &settings)
    : QWidget(pin->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), pin_(pin) {
    // 工具窗直接归属原生输出宿主，保持贴图和标注画布为普通子控件。
    // Parent to the existing native host so the image/canvas stay in one composited backing store.
    setObjectName(QStringLiteral("linuxPinToolbarHost"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto *content = new PinToolbarContent(this); toolbar_ = content;
    content->setObjectName(QStringLiteral("linuxPinToolbar"));
    content->positionRequested = [this] { positionToolbar(); };
    // 贴图布局独立保存，调整它不会改变截图工具栏；每次显示重新读取以同步其他贴图的修改。
    // Persist the PIN layout independently; reread on show to pick up changes made by another pin.
    toolbar_->set_preview_layout_storage([&settings] { return settings.pinToolbarLayout(); },
                                         [&settings](const QJsonArray &layout) { return settings.savePinToolbarLayout(layout); });
    connect(toolbar_, &jietuba::Toolbar::copy_clicked, pin, &LinuxPinWindow::copyImage);
    connect(toolbar_, &jietuba::Toolbar::save_clicked, pin, &LinuxPinWindow::saveImage);
    new LinuxShortcutRouter(this, &settings, "pin", [this](const QString &action) {
        return pin_ && pin_->triggerShortcut(action);
    }, [this] { return editingLayout(); });
    connect(toolbar_, &jietuba::Toolbar::preview_layout_changed, this, [this] { positionToolbar(); refreshInputRegion(); });
    connect(toolbar_, &jietuba::Toolbar::preview_layout_dialog_visibility_changed, this, [this](bool editing) {
        emit layoutEditingChanged(editing);
    });
    // 子菜单/编辑器是动态创建的；统一观察本宿主的控件几何，实时更新 Wayland 输入区域。
    // Observe dynamic child geometry so newly opened menus/editors enter the Wayland input region.
    qApp->installEventFilter(this);
}

QRect LinuxPinToolbar::toolbarGeometry(const QSize &output, const QRectF &pinRect, const QSize &bar) {
    constexpr int gap = 4;
    const int x = std::clamp(qRound(pinRect.right()) - bar.width(), 0, std::max(0, output.width() - bar.width()));
    int y = qRound(pinRect.bottom()) + gap;
    if (y + bar.height() > output.height()) y = qRound(pinRect.top()) - gap - bar.height();
    // 上下都无空间（全屏贴图或越界缩放）时放进屏幕，仍保证工具栏可操作。
    // Full-screen/oversized pins leave no outside space; clamp the bar inside the output.
    return {x, std::clamp(y, 0, std::max(0, output.height() - bar.height())), bar.width(), bar.height()};
}
void LinuxPinToolbar::showFor(QScreen *screen, const QRectF &pinRect) {
    if (!screen) return;
    // 第一次映射前设置 surface role，之后始终使用原角色；不在显示中切换 xdg/layer-shell。
    // Set the surface role before first mapping, never switching xdg/layer roles while visible.
    if (!windowHandle()) {
        winId(); windowHandle()->setScreen(screen);
#ifdef JIETUBA_HAS_LAYER_SHELL
        if (LinuxWayland::supportsLayerShell()) {
            layer_ = LayerShellQt::Window::get(windowHandle());
            if (layer_) {
                layer_->setScope(QStringLiteral("jietuba-pin-toolbar"));
                layer_->setScreen(screen);
                layer_->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorLeft);
                layer_->setExclusiveZone(-1);
                layer_->setLayer(LayerShellQt::Window::LayerOverlay);
                layer_->setDesiredSize(screen->geometry().size());
                // 只在点击本工具栏时获取键盘，不能像截图选区一样独占整个桌面的输入。
                // Acquire keyboard focus on demand; a pin toolbar must never monopolize desktop input.
                layer_->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
            }
        }
#endif
    }
    syncPosition(screen, pinRect);
    toolbar_->reload_layout(); toolbar_->show(); positionToolbar(); refreshInputRegion();
    bool fullscreen=QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
#ifdef JIETUBA_HAS_LAYER_SHELL
    fullscreen=fullscreen && !layer_;
#endif
    if(fullscreen) showFullScreen(); else show();
}
void LinuxPinToolbar::syncPosition(QScreen *screen, const QRectF &pinRect) {
    if (!screen) return;
    screen_ = screen; pinRect_ = pinRect;
    if (windowHandle() && windowHandle()->screen() != screen) windowHandle()->setScreen(screen);
    const QSize outputSize = screen->geometry().size();
    const bool resized = size()!=outputSize;
    if (resized) resize(outputSize);
#ifdef JIETUBA_HAS_LAYER_SHELL
    if (layer_) {
        if (layer_->screen()!=screen) layer_->setScreen(screen);
        if (resized) layer_->setDesiredSize(outputSize);
        layer_->setMargins(QMargins());
    }
    else
#endif
    move(screen->geometry().topLeft());
    if (resized || toolbar_->property("preview_width").toInt()!=std::max(1,width()-16))
        toolbar_->set_preview_available_width(std::max(1, width() - 16));
    positionToolbar(); refreshInputRegion();
}
void LinuxPinToolbar::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // 合成器确认的表面尺寸可能晚于 showFor；使用最终逻辑尺寸再次定位并更新输入区域。
    // Compositor configure may arrive after showFor. Reflow against the final logical surface size.
    toolbar_->set_preview_available_width(std::max(1, width() - 16));
    positionToolbar();
    refreshInputRegion();
}
void LinuxPinToolbar::positionToolbar() {
    auto *content = static_cast<PinToolbarContent *>(toolbar_);
    if (!content) return;
    if (!content->manuallyPositioned()) content->move(toolbarGeometry(size(), pinRect_, content->size()).topLeft());
    else content->move(std::clamp(content->x(), 0, std::max(0, width() - content->width())),
                       std::clamp(content->y(), 0, std::max(0, height() - content->height())));
    content->set_preview_below_selection(content->y() >= pinRect_.bottom());
    content->syncPanels();
    emit panelPositionChanged(content->y() >= pinRect_.bottom());
}
void LinuxPinToolbar::refreshInputRegion() {
    QRegion region;
    // 只合并直接子控件；工具栏自己的按钮已经包含在其矩形中。
    // Direct children suffice: toolbar buttons are already inside their parent's rectangle.
    for (auto *child : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (!child->isHidden() && !child->isWindow()) region += child->geometry();
    region &= rect();
    // Qt 的空 mask 表示“取消遮罩/整窗接收输入”，不是“无输入”。布局切换的短暂空状态
    // 必须用屏幕外的非空矩形表达，否则这个全屏透明宿主会挡住整张贴图和桌面。
    // An empty Qt mask resets to whole-window input. Use a nonempty off-surface sentinel when no
    // controls are visible, or this transparent full-output host intercepts every pin/desktop click.
    // 绘制区域必须保留整张透明表面，才能清除工具栏移动/换行前的位置，避免残留第二条工具栏。
    // Only mask native input. A QWidget paint mask would prevent clearing the bar's former pixels.
    clearMask();
    if(windowHandle()) windowHandle()->setMask(region.isEmpty() ? QRegion(QRect(-2,-2,1,1)) : region);
    update();
}
void LinuxPinToolbar::paintEvent(QPaintEvent *) {
    // 先清空旧帧的 alpha，再由子控件绘制当前唯一工具栏及面板。
    // Clear stale alpha before drawing the current bar/panels, including after repositioning.
    QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(),Qt::transparent);
}
bool LinuxPinToolbar::eventFilter(QObject *watched, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget && widget != this && widget->window() == this &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        // 合并同一轮布局产生的大量事件，避免在设置 mask 时递归布局。
        // Coalesce layout events instead of recursively changing masks during widget layout.
        if (!refreshing_) {
            refreshing_ = true;
            QTimer::singleShot(0, this, [this] { refreshing_ = false; refreshInputRegion(); });
        }
    }
    return QWidget::eventFilter(watched, event);
}
void LinuxPinToolbar::hideControls() {
    emit controlsClosing();
    toolbar_->cancel_preview_layout_dialog(); toolbar_->hide_all_panels(); toolbar_->hide(); hide();
    emit hoverChanged(false);
}
bool LinuxPinToolbar::editingLayout() const { return toolbar_->preview_layout_dialog_open(); }
void LinuxPinToolbar::enterEvent(QEnterEvent *event) { QWidget::enterEvent(event); emit hoverChanged(true); }
void LinuxPinToolbar::leaveEvent(QEvent *event) { QWidget::leaveEvent(event); emit hoverChanged(false); }
void LinuxPinToolbar::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        if (editingLayout()) toolbar_->cancel_preview_layout_dialog();
        else if (pin_) pin_->closeByKeyboard();
        event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}
