#include "pin_window.h"
#include "annotation_canvas.h"
#include "linux_settings.h"
#include "shortcut_bindings.h"
#include "pin_toolbar.h"
#include "tesseract_ocr.h"
#include <QFutureWatcher>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <QGraphicsView>
#include <QGraphicsOpacityEffect>
#include <QPainterPath>
#include <QLabel>
#include <QSlider>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include "pin/pin_controls.h"
#include "ui/toolbar.h"
#include <QApplication>
#include <QAbstractButton>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QHideEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QScopedValueRollback>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include "wayland_surface_support.h"

namespace {
/**
 * 贴图的固定透明输出表面：不随贴图滚轮请求原生尺寸，不依赖合成器提交边距。
 * 贴图以 QWidget 子控件移动，实际几何、输入遮罩与工具栏在同一轮事件中同步。
 * 每次绘制先清 alpha，移动后的旧边框不能残留在后台缓冲中。
 * Fixed transparent output surface for a child pin. Native size/margins never change
 * while dragging or zooming. Child geometry, input mask and toolbar agree immediately;
 * clearing alpha prevents old borders from surviving in the backing buffer.
 */
class PinSurface final : public QWidget {
public:
    QRect shadowRect;
    double shadowOpacity=1;
    PinSurface() : QWidget(nullptr,Qt::Window|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint) {
        setObjectName(QStringLiteral("linuxPinSurface"));
        setWindowTitle(QStringLiteral("截图吧 - 贴图"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_QuitOnClose,false);
        // 非空的屏幕外遮罩表示无输入；Qt 空遮罩会恢复全屏输入。
        // An off-surface sentinel means no input; an empty Qt mask restores full-window input.
        setMask(QRegion(QRect(-2,-2,1,1)));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(),Qt::transparent);
        if(!shadowRect.isEmpty()) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver); painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen); painter.setBrush(QColor(0,0,0,qRound(13*shadowOpacity)));
            for(int n=7;n>0;--n) painter.drawRoundedRect(shadowRect.translated(0,2).adjusted(-n,-n,n,n),7+n,7+n);
        }
    }
};
}

PinZoomAnchor PinZoomAnchor::fromPointer(const QPointF &topLeft, const QSize &size, const QPointF &pointer) {
    const QRectF center(size.width()*.25,size.height()*.25,size.width()*.5,size.height()*.5);
    if (!size.isEmpty() && center.contains(pointer))
        return {Corner::Pointer,topLeft+pointer,{pointer.x()/size.width(),pointer.y()/size.height()}};
    const bool right = pointer.x() >= size.width() / 2.0;
    const bool bottom = pointer.y() >= size.height() / 2.0;
    const Corner corner = right ? (bottom ? Corner::BottomRight : Corner::TopRight)
                                : (bottom ? Corner::BottomLeft : Corner::TopLeft);
    // 锚点始终取旧窗口的真实角，不把局部鼠标坐标误当作全局坐标。
    // Capture the old rectangle's corner, not a possibly synthetic Wayland global pointer.
    return {corner, topLeft + QPointF(right ? size.width() : 0, bottom ? size.height() : 0)};
}

QPointF PinZoomAnchor::topLeftFor(const QSize &size) const {
    if (corner==Corner::Pointer) return outputPoint-QPointF(size.width()*ratio.x(),size.height()*ratio.y());
    const bool right = corner == Corner::TopRight || corner == Corner::BottomRight;
    const bool bottom = corner == Corner::BottomLeft || corner == Corner::BottomRight;
    return outputPoint - QPointF(right ? size.width() : 0, bottom ? size.height() : 0);
}

LinuxPinWindow::LinuxPinWindow(const QImage &image, LinuxSettings &settings, QWidget *parent)
    : QWidget(parent), image_(image), settings_(settings) {
    borderEnabled_ = settings_.pinBorderEnabled();
    // 透明度效果需要真正的 alpha 子控件，否则 Qt 会把底图当作不透明区域缓存。
    // Keep an alpha-capable child so opacity changes do not reuse an opaque backing region.
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("截图吧 - 贴图"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setToolTip(QStringLiteral("拖动移动 · 滚轮缩放 · 右键菜单 · Esc 关闭"));
    // 贴图使用独立上下文；修改快捷键后即时生效，不拦截保存对话框和菜单输入。
    // Pins have their own binding context; preferences apply immediately without stealing dialog input.
    new LinuxShortcutRouter(this,&settings_,"pin",[this](const QString &action) { return triggerShortcut(action); },
                           [this] { return (toolbarHost_ && toolbarHost_->editingLayout()) || (canvas_ && canvas_->editingCanvasText()); });
    setMinimumSize(1, 1);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    qApp->installEventFilter(this);
    // 直接复用 Windows 右上角的关闭/工具栏按钮，不重复定义图标和悬停颜色。
    // Reuse Windows close/toolbar controls, including their icons and hover colors.
    controls_ = std::make_unique<jietuba::PinControlButtons>(this);
    controls_->close_button->setObjectName(QStringLiteral("pinClose"));
    controls_->toolbar_toggle_button->setObjectName(QStringLiteral("pinToolbarToggle"));
    controls_->connect_signals([this] { if(!lockBlocks("click_close")) close(); }, [this] { toggleToolbar(); });

}
LinuxPinWindow::~LinuxPinWindow() {
    // 独立表面可能在销毁时触发焦点/悬停事件，先于本类状态成员销毁它。
    // Tear down the separate surface before our state members, since unmapping can emit hover/focus events.
    delete throughBar_.data();
    if (toolbarHost_) {
        disconnect(toolbarHost_, nullptr, this, nullptr);
        delete canvas_;
        canvas_ = nullptr;
        delete toolbarHost_.data();
    }
    // 此时贴图仍是表面的子对象，延迟释放父表面避免析构期间递归删除自身。
    // Defer parent-surface deletion until this child finishes destruction.
    if (surface_) { surface_->hide(); surface_->deleteLater(); }
}
bool LinuxPinWindow::triggerShortcut(const QString &action) {
    if (action=="pin_copy") copyImage();
    else if (action=="pin_undo" || action=="pin_redo") {
        if (!toolbarHost_) return false;
        auto *bar = toolbarHost_->toolbar();
        (action == "pin_undo" ? bar->undo_btn : bar->redo_btn)->click();
    }
    else if (action=="pin_save") saveImage();
    else if (action=="pin_toggle_toolbar") toggleToolbar();
    else if (action=="pin_lock") {
        endDrag(); locked_=!locked_;
        if(settings_.pinFlag("lock_hint")) hint(locked_ ? "已锁定，右键可解锁" : "已解锁");
    } else if (action=="pin_reset") { if(!lockBlocks("move")) resetZoom(); }
    else if (action=="pin_zoom_in" || action=="pin_zoom_out") {
        if(!lockBlocks("move")) stepZoom(settings_.pinZoomStep()/100.0,action=="pin_zoom_in",zoomAnchor(rect().center()));
    } else return false;
    return true;
}
void LinuxPinWindow::showOnScreen(QScreen *screen, const QRectF &selection) {
    if (!screen) screen = QGuiApplication::primaryScreen();
    outputScreen_ = screen;
    if (screen) screenScale_ = std::max(0.1, double(screen->devicePixelRatio()));
    // 优先使用截图视图实际映射后的逻辑尺寸，兼容分数缩放和高 DPI。
    // Use the selection's mapped logical size rather than assuming an integer DPI ratio.
    baseSize_ = selection.isValid() ? selection.size() : QSizeF(image_.size()) / screenScale_;
    // 剪贴板贴图没有截图选区，先确定输出内位置；工具栏与贴图共用这套坐标。
    // 普通 Wayland 顶层窗口的 pos() 无法可靠反映合成器位置，不能据此跟随工具栏。
    // Clipboard pins have no capture rectangle. Establish output-local coordinates before mapping;
    // ordinary Wayland toplevel positions cannot reliably anchor a separate toolbar surface.
    layerPosition_ = selection.isValid() ? selection.topLeft()
        : screen ? QPointF((screen->geometry().width() - baseSize_.width()) / 2.0,
                           (screen->geometry().height() - baseSize_.height()) / 2.0) : QPointF();
    if(screen && settings_.pinFlag("auto_shrink") && !baseSize_.isEmpty()) {
        const QRect available=screen->availableGeometry().translated(-screen->geometry().topLeft());
        zoom_=std::min({1.0,available.width()/baseSize_.width(),available.height()/baseSize_.height()});
        if(zoom_<1) layerPosition_=QPointF(available.center())-QPointF(baseSize_.width()*zoom_/2,baseSize_.height()*zoom_/2);
    }
    // 在可定位的桌面使用固定宿主。离屏平台也走同一子控件路径，测试实际几何而非请求值。
    // Use a fixed host where positioning is supported; offscreen tests exercise the same child geometry.
    const bool wayland=QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    if (screen && (LinuxWayland::supportsLayerShell() || !wayland)) {
        if (!surface_) {
            surface_=new PinSurface;
            surface_->resize(screen->geometry().size());
            surface_->winId();
            surface_->windowHandle()->setScreen(screen);
            setParent(surface_,Qt::Widget|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
            surface_->setFocusProxy(this);
            connect(screen,&QScreen::geometryChanged,this,[this] { close(); });
            connect(screen,&QObject::destroyed,this,[this] { close(); });
        }
    } else {
        winId();
        if (windowHandle() && screen) windowHandle()->setScreen(screen);
    }
#ifdef JIETUBA_HAS_LAYER_SHELL
    // layer-shell 可指定输出及边距；普通 xdg-shell 的位置由合成器决定。
    // Layer-shell permits output-relative margins; xdg-shell placement remains compositor policy.
    if (surface_ && LinuxWayland::supportsLayerShell()) {
        layer_ = LayerShellQt::Window::get(surface_->windowHandle());
        if (layer_) {
            layer_->setScope(QStringLiteral("jietuba-pin"));
            layer_->setScreen(screen);
            layer_->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom |
                              LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
            layer_->setDesiredSize(screen->geometry().size());
            layer_->setMargins(QMargins());
            layer_->setExclusiveZone(-1);
            // KDE 会把当前全屏窗口放到 Top 层之上；贴图需位于 Overlay 才能在穿透点击后继续可见。
            // KWin raises an active fullscreen window above Top. Keep a pin on Overlay even
            // when click-through activates that fullscreen window underneath.
            layer_->setLayer(LayerShellQt::Window::LayerOverlay);
            layer_->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        }
    }
#endif
    applyAppearance(); setPinOpacity(settings_.pinPercent("default_opacity")/100.0);
    resizeForZoom();
    // 非 Wayland 平台也使用相同初始位置，方便离屏验证；xdg-shell 仍由合成器放置。
    // Use the same placement on non-Wayland platforms; xdg-shell placement stays compositor-owned.
    if (surface_ && !wayland) surface_->move(screen->geometry().topLeft());
    updateLayerPosition();
    show();
    if (surface_) surface_->show();
    setFocus(Qt::OtherFocusReason);
    if(!ocrStarted_ && settings_.pinFlag("auto_ocr")) startOcr();
}
void LinuxPinWindow::updateLayerPosition() {
    if (surface_) {
        // 原生表面保持原位：子控件的移动/缩放是同步的，不会出现“工具栏先走、图片没走”。
        // Keep the native surface fixed. Child geometry changes synchronously with the toolbar.
        setGeometry(QRect(layerPosition_.toPoint(),requestedSize_));
        syncSurface();
    }
    if (toolbarHost_ && toolbarHost_->isVisible()) toolbarHost_->syncPosition(outputScreen_, toolbarPinRect());
}
void LinuxPinWindow::resizeForZoom() {
    if (image_.isNull()) return;
    // 可以越过输出边缘，不再让尺寸上限与位置夹取互相争夺窗口位置。
    // Allow clipping past the output edge instead of fighting a size cap and position clamp.
    const QSize size(std::max(1, qRound(baseSize_.width() * zoom_)), std::max(1, qRound(baseSize_.height() * zoom_)));
    requestedSize_ = size;
    resize(size);
}
void LinuxPinWindow::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (canvas_) return; // 画布绘制图层及边框 / The canvas paints the layers and border.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(),Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(rect(), image_);
    paintBorder(painter);
}
void LinuxPinWindow::paintBorder(QPainter &painter) const {
    // 边框仅绘制在窗口中，不改变复制或保存的原始截图。
    // Draw the border as window decoration, keeping the exported image unchanged.
    if (borderEnabled_) {
        painter.save(); painter.setRenderHint(QPainter::Antialiasing,false);
        if(rounded_) {
            // 圆角边框沿圆角轮廓向内画一个物理像素，不再画会被圆角遮罩切断的四条直线。
            // Inset a cosmetic rounded outline to match the image mask at any display scale.
            const auto device=painter.deviceTransform();
            const double scale=std::hypot(device.m11(),device.m12()); const double inset=.5/std::max(.01,scale);
            const QRectF outline=QRectF(rect()).adjusted(inset,inset,-inset,-inset);
            const double radius=std::max(0.,std::min({7.,width()/2.,height()/2.})-inset);
            QPen pen(QColor("#8cc8ed")); pen.setWidthF(0); pen.setCosmetic(true);
            painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.setRenderHint(QPainter::Antialiasing);
            painter.drawRoundedRect(outline,radius,radius);
            painter.restore(); return;
        }
        // 分数缩放下边缘笔画会跨过父表面 clip 而消失。先映射到物理像素并向内对齐，
        // 再画四条一物理像素的实心边，避免随贴图位置的小数像素相位闪烁或缺边。
        // An edge-centered pen can be clipped at fractional DPI. Snap inward in device pixels,
        // then fill four one-pixel strips so movement cannot change border coverage.
        const auto transform=painter.deviceTransform();
        bool invertible=false; const auto inverse=transform.inverted(&invertible);
        const auto device=transform.mapRect(QRectF(rect()));
        const int left=int(std::ceil(device.left()-1e-7)),top=int(std::ceil(device.top()-1e-7));
        const int right=int(std::floor(device.right()+1e-7)),bottom=int(std::floor(device.bottom()+1e-7));
        const QColor color(QStringLiteral("#8cc8ed"));
        if(invertible && right>left && bottom>top) {
            for(const QRectF strip:{QRectF(left,top,right-left,1),QRectF(left,bottom-1,right-left,1),
                                   QRectF(left,top,1,bottom-top),QRectF(right-1,top,1,bottom-top)})
                painter.fillRect(inverse.mapRect(strip),color);
        } else painter.fillRect(rect(),color);
        painter.restore();
    }
}
bool LinuxPinWindow::beginDrag(const QPointF &globalPosition) {
    if (through_ || lockBlocks("move") || (!surface_ && !windowHandle())) return false;
    wheelGesture_.invalidate();
    wheelAnchorValid_ = false;
    dragAnchor_ = globalPosition;
    dragOrigin_ = pos();
    dragging_ = true;
    if (surface_) return true;
    // xdg-shell 必须在真实按下事件内请求移动；离屏平台沿用手动路径供回归验证。
    // xdg-shell needs the real press serial; non-Wayland platforms share the manual path for regression tests.
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        dragging_ = windowHandle()->startSystemMove();
    }
    return dragging_;
}
void LinuxPinWindow::dragTo(const QPointF &globalPosition) {
    if (!dragging_) return;
    if(resizeEdges_) {
        const bool left=resizeEdges_.testFlag(Qt::LeftEdge), top=resizeEdges_.testFlag(Qt::TopEdge);
        const auto d=globalPosition-dragAnchor_;
        const double dx=resizeEdges_ & (Qt::LeftEdge|Qt::RightEdge) ? baseSize_.width()*(left?-1:1) : 0;
        const double dy=resizeEdges_ & (Qt::TopEdge|Qt::BottomEdge) ? baseSize_.height()*(top?-1:1) : 0;
        const double scale=std::clamp(resizeStart_.width()/baseSize_.width()+(d.x()*dx+d.y()*dy)/(dx*dx+dy*dy),.1,4.0);
        zoom_=scale; resizeForZoom();
        const QPointF fixed(left ? 1 : dx ? 0 : .5, top ? 1 : dy ? 0 : .5);
        layerPosition_=resizeStart_.topLeft()+QPointF((resizeStart_.width()-requestedSize_.width())*fixed.x(),(resizeStart_.height()-requestedSize_.height())*fixed.y());
        if(surface_) updateLayerPosition(); else move(layerPosition_.toPoint()); return;
    }
    if (surface_) {
        // 子控件移动会产生重定位事件，相同的鼠标屏幕坐标不得再次累计位移。
        // Child relocation can synthesize motion. Repeating the same pointer position must be idempotent.
        layerPosition_ = dragOrigin_ + globalPosition - dragAnchor_;
        updateLayerPosition();
        return;
    }
    if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        move((dragOrigin_ + globalPosition - dragAnchor_).toPoint());
}
void LinuxPinWindow::endDrag() { dragging_ = false; resizeEdges_={}; }
void LinuxPinWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && (beginEdgeResize(event->position(),event->globalPosition()) || beginDrag(event->globalPosition()))) { event->accept(); return; }
    QWidget::mousePressEvent(event);
}
void LinuxPinWindow::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        dragTo(event->globalPosition()); event->accept(); return;
    }
    if (dragging_) dragging_ = false;
    // 窗口移动也会产生局部鼠标事件；只在滚轮停止一段时间后的真实移动中重选象限。
    // Surface relocation can also generate motion; only a later pointer move selects a new quadrant.
    if (wheelAnchorValid_ && wheelGesture_.isValid() && wheelGesture_.elapsed() > 350)
        wheelAnchorValid_ = false;
    QWidget::mouseMoveEvent(event);
}
void LinuxPinWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        endDrag();
        event->accept(); return;
    }
    QWidget::mouseReleaseEvent(event);
}
void LinuxPinWindow::wheelEvent(QWheelEvent *event) {
    applyPinWheel(event,event->position());
}
void LinuxPinWindow::applyPinWheel(QWheelEvent *event,QPointF pinPoint) {
    if(through_) {event->ignore();return;}
    const auto mode=event->modifiers() & (Qt::ControlModifier|Qt::AltModifier);
    const bool pixelOnly=event->angleDelta().isNull();
    // 与 Windows 原版相同：Alt 修饰的滚轮可能被平台映射为横向轴。
    // 仅读取 y 会把有效的精细缩放事件当成零增量；两轴只取一轴，避免对角滚动重复执行。
    // Match Windows: modifier-assisted wheel input may arrive on the horizontal axis.
    // Prefer vertical, then horizontal, using exactly one axis for each logical wheel event.
    const QPoint deltaVector=pixelOnly ? event->pixelDelta() : event->angleDelta();
    const int delta=deltaVector.y() ? deltaVector.y() : deltaVector.x();
    if(!delta) {event->ignore(); return;}
    // 一个物理刻度只执行一次，包括高分辨率鼠标分多次送来的 angleDelta。
    // Accumulate high-resolution deltas; keep the anchor stable throughout a wheel gesture.
    if(!wheelAnchorValid_ || wheelGesture_.elapsed()>350 || wheelMode_!=mode || event->phase()==Qt::ScrollBegin) {
        wheelAnchor_=zoomAnchor(pinPoint); wheelAnchorValid_=true; wheelMode_=mode; wheelRemainder_=0;
    }
    wheelGesture_.restart();
    const int unit=pixelOnly ? 30 : 120;
    wheelRemainder_+=delta;
    const int steps=wheelRemainder_/unit;
    wheelRemainder_%=unit;
    if(steps && mode.testFlag(Qt::ControlModifier)) {
        setPinOpacity(opacity_+steps*settings_.pinPercent("opacity_step")/100.);
        hint(QStringLiteral("不透明度 %1%").arg(qRound(opacity_*100)));
    } else if(steps && !lockBlocks("move")) {
        const double step=(mode.testFlag(Qt::AltModifier) ? settings_.pinPercent("fine_zoom_step") : settings_.pinZoomStep())/100.;
        for(int i=0;i<std::abs(steps);++i) stepZoom(step,steps>0,wheelAnchor_);
        if(mode.testFlag(Qt::AltModifier)) hint(QStringLiteral("缩放 %1%").arg(qRound(zoom_*100)));
    }
    event->accept();
}
void LinuxPinWindow::changeZoom(double factor, const PinZoomAnchor &anchor) {
    const double oldZoom = zoom_;
    zoom_ = std::clamp(zoom_ * factor, .1, 4.0);
    if (zoom_ == oldZoom) return;
    if (surface_) {
        const QSize target(std::max(1, qRound(baseSize_.width() * zoom_)), std::max(1, qRound(baseSize_.height() * zoom_)));
        layerPosition_ = anchor.topLeftFor(target);
    }
    resizeForZoom();
    updateLayerPosition();
}
void LinuxPinWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        if (toolbarHost_ && toolbarHost_->editingLayout()) toolbarHost_->toolbar()->cancel_preview_layout_dialog();
        else closeByKeyboard();
        return;
    }
    QWidget::keyPressEvent(event);
}
void LinuxPinWindow::contextMenuEvent(QContextMenuEvent *event) {
    wheelGesture_.invalidate();
    wheelAnchorValid_ = false;
    const auto menuAnchor = zoomAnchor(event->pos());
    // 先提供已实现且可操作的贴图菜单；菜单关闭后再执行动作，避免关闭贴图时销毁正在运行的菜单。
    // Offer working pin actions now; run them after the menu closes so Close cannot destroy an active menu.
    auto *menu = new QMenu(window());
    // KDE 深色主题会把菜单项文字调色板继承成白色；菜单使用浅色背景时必须同时指定文字色。
    // A dark desktop palette can leave menu item text white on a light menu background.
    auto palette = menu->palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f8fafc")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202838")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#202838")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#202838")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#40e0d0")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#10232a")));
    menu->setPalette(palette);
    menu->setStyleSheet(QStringLiteral("QMenu { background: #f8fafc; color: #202838; border: 1px solid #a8b6c6; padding: 4px; }"
                                       "QMenu::item { color: #202838; background: transparent; padding: 6px 20px; }"
                                       "QMenu::item:selected { background: #40e0d0; color: #10232a; }"
                                       "QMenu::item:disabled { color: #788494; }"
                                       "QMenu::separator { height: 1px; background: #d8e1ea; margin: 4px 8px; }"));
    auto *copy = menu->addAction(QStringLiteral("复制图片\t")+settings_.shortcut("pin_copy"));
    auto *save = menu->addAction(QStringLiteral("保存图片…\t")+settings_.shortcut("pin_save"));
    const auto toolbarShortcut = settings_.shortcut("pin_toggle_toolbar");
    auto *toolbarAction = menu->addAction(toolbarShortcut.isEmpty() ? QStringLiteral("标注工具栏")
        : QStringLiteral("标注工具栏\t") + toolbarShortcut);
    toolbarAction->setCheckable(true); toolbarAction->setChecked(toolbarHost_ && toolbarHost_->isVisible());
    menu->addSeparator();
    auto *zoomIn = menu->addAction(QStringLiteral("放大 %1%").arg(settings_.pinZoomStep()));
    auto *zoomOut = menu->addAction(QStringLiteral("缩小 %1%").arg(settings_.pinZoomStep()));
    auto *reset = menu->addAction(QStringLiteral("恢复原始大小"));
    auto *border = menu->addAction(QStringLiteral("显示边框"));
    border->setCheckable(true); border->setChecked(borderEnabled_);
    menu->addSeparator();
    auto *lockAction=menu->addAction("锁定贴图\t"+settings_.shortcut("pin_lock")); lockAction->setCheckable(true); lockAction->setChecked(locked_);
    auto *throughAction=menu->addAction("鼠标穿透");
    auto *ocrAction=menu->addAction(ocrPending_ ? "正在识别文字…" : ocrStarted_ && ocrText_.isEmpty() ? "重新识别文字" : ocrText_.isEmpty() ? "识别贴图文字" : "复制识别文字");
    ocrAction->setEnabled(!ocrPending_);
    auto *closeAction = menu->addAction(QStringLiteral("关闭贴图\tEsc"));
    const auto chosen = menu->exec(event->globalPos());
    enum class Choice { None, Copy, Save, Toolbar, ZoomIn, ZoomOut, Reset, Border, Close, Lock, Through, Ocr };
    const Choice choice = chosen == copy ? Choice::Copy : chosen == save ? Choice::Save
        : chosen == toolbarAction ? Choice::Toolbar
        : chosen == zoomIn ? Choice::ZoomIn : chosen == zoomOut ? Choice::ZoomOut
        : chosen == reset ? Choice::Reset : chosen == border ? Choice::Border
        : chosen == lockAction ? Choice::Lock : chosen == throughAction ? Choice::Through : chosen == ocrAction ? Choice::Ocr
        : chosen == closeAction ? Choice::Close : Choice::None;
    delete menu;
    if (choice == Choice::Copy) copyImage();
    else if (choice == Choice::Save) saveImage();
    else if (choice == Choice::Toolbar) toggleToolbar();
    else if (choice == Choice::ZoomIn || choice == Choice::ZoomOut) {
        stepZoom(settings_.pinZoomStep()/100.0,choice==Choice::ZoomIn,menuAnchor);
    } else if (choice == Choice::Reset) resetZoom();
    else if (choice == Choice::Border) { borderEnabled_ = !borderEnabled_; update(); if (canvas_) canvas_->update(); }
    else if (choice == Choice::Lock) triggerShortcut("pin_lock");
    else if (choice == Choice::Through) setClickThrough(true);
    else if (choice == Choice::Ocr) { if(!ocrText_.isEmpty()) { QApplication::clipboard()->setText(ocrText_); hint("已复制识别文字"); } else startOcr(true); }
    else if (choice == Choice::Close) close();
    event->accept();
}
void LinuxPinWindow::resetZoom() {
    const QPointF center=layerPosition_+QPointF(requestedSize_.width()/2.0,requestedSize_.height()/2.0);
    wheelAnchorValid_=false; zoom_=1.0; resizeForZoom();
    if (surface_) { layerPosition_=center-QPointF(requestedSize_.width()/2.0,requestedSize_.height()/2.0); updateLayerPosition(); }
}
QImage LinuxPinWindow::image() const { return canvas_ ? canvas_->result() : image_; }
void LinuxPinWindow::copyImage() {
    const QImage output = image();
    if (output.isNull()) return;
    QByteArray png; QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !output.save(&buffer, "PNG")) {
        QMessageBox::warning(window(), QStringLiteral("复制失败"), QStringLiteral("无法生成剪贴板图片。")); return;
    }
    auto *mime = new QMimeData;
    mime->setImageData(output); mime->setData(QStringLiteral("image/png"), png);
    QApplication::clipboard()->setMimeData(mime);
}
void LinuxPinWindow::saveImage() {
    const QImage output = image();
    if (output.isNull()) return;
    // 原生文件对话框打开期间也属于工具栏交互，不能因移出贴图而把宿主藏掉。
    // Keep the toolbar hover session alive while a native save dialog owns input.
    QFileDialog dialog(this, QStringLiteral("保存贴图"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilters({QStringLiteral("PNG 图片 (*.png)"), QStringLiteral("JPEG 图片 (*.jpg *.jpeg)"), QStringLiteral("BMP 图片 (*.bmp)")});
    const auto format = settings_.imageFormat();
    dialog.selectNameFilter(format == "jpg" ? QStringLiteral("JPEG 图片 (*.jpg *.jpeg)") : format == "bmp" ? QStringLiteral("BMP 图片 (*.bmp)") : QStringLiteral("PNG 图片 (*.png)"));
    dialog.setDefaultSuffix(format);
    connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog](const QString &filter) {
        dialog.setDefaultSuffix(filter.startsWith("JPEG") ? "jpg" : filter.startsWith("BMP") ? "bmp" : "png");
    });
    dialog.selectFile(QDir(settings_.saveDirectory()).filePath(QStringLiteral("贴图.") + format));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (!accepted || dialog.selectedFiles().isEmpty()) return;
    const QString path = dialog.selectedFiles().first();
    if (!path.isEmpty() && !output.save(path)) QMessageBox::warning(window(), QStringLiteral("保存失败"), QStringLiteral("无法保存贴图，请检查文件名和目录权限。"));
}

QRectF LinuxPinWindow::toolbarPinRect() const {
    // 工具栏读取实际子控件几何，不能读取尚未提交的原生窗口请求位置。
    // Follow actual child geometry, not an uncommitted native-window position request.
    if (surface_) return geometry();
    const auto *output = screen();
    return {QPointF(pos() - (output ? output->geometry().topLeft() : QPoint())), QSizeF(size())};
}
void LinuxPinWindow::syncToolbar() {
    // show/hide 会同步触发 Enter/Leave 和控件布局；同一轮同步只允许一个入口。
    // Mapping controls emits hover/layout events synchronously. Do not recursively remap the host.
    if (syncingToolbar_) return;
    QScopedValueRollback<bool> syncing(syncingToolbar_, true);
    const bool showToolbar = !through_ && isVisible() && toolbarOpen_;
    if (controls_) {
        const bool fits=width() >= 2*jietuba::PinControlButtons::BUTTON_SIZE+jietuba::PinControlButtons::SPACING+10 && height()>=34;
        controls_->update_positions(width());
        const bool visible=!through_ && settings_.pinHoverButtons() && pinHovered_ && fits;
        controls_->set_visible(visible,visible);
    }
    if (!showToolbar) { if (toolbarHost_ && toolbarHost_->isVisible()) toolbarHost_->hideControls(); return; }
    if (!toolbarHost_) {
        toolbarHost_ = new LinuxPinToolbar(this, settings_);
        canvas_ = LinuxAnnotationCanvas::createForPin(this, image_, settings_, toolbarHost_);
        canvas_->lower(); // 悬停按钮仍在最上层 / Keep corner controls above the canvas.
        canvas_->show();
        connect(toolbarHost_, &LinuxPinToolbar::controlsClosing, canvas_, &LinuxAnnotationCanvas::endPinEditing);
    }
    QScreen *target = surface_ ? outputScreen_.data() : screen();
    if (toolbarHost_->isVisible()) toolbarHost_->syncPosition(target, toolbarPinRect());
    else {
        canvas_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        toolbarHost_->showFor(target, toolbarPinRect());
    }
}
void LinuxPinWindow::toggleToolbar() {
    if(through_) return;
    toolbarOpen_=!toolbarOpen_; syncToolbar();
}
void LinuxPinWindow::enterEvent(QEnterEvent *event) { QWidget::enterEvent(event); pinHovered_=true; syncToolbar(); }
void LinuxPinWindow::leaveEvent(QEvent *event) { QWidget::leaveEvent(event); pinHovered_=false; syncToolbar(); }
void LinuxPinWindow::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); if(rounded_) applyAppearance(); if (canvas_) canvas_->setGeometry(rect()); syncToolbar(); }
void LinuxPinWindow::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    if (surface_) {
        layerPosition_=pos();
        syncSurface();
    }
    syncToolbar();
}
void LinuxPinWindow::hideEvent(QHideEvent *event) {
    // 关闭/隐藏贴图必须同时撤下独立的 layer-shell 工具栏，不能残留透明输入层。
    // Always unmap the separate toolbar surface with its pin, leaving no stale input layer.
    pinHovered_=false;
    endDrag();
    toolbarOpen_=false;
    if (toolbarHost_) toolbarHost_->hideControls();
    if (throughBar_) throughBar_->hide();
    if (surface_ && surface_->isVisible()) surface_->hide();
    QWidget::hideEvent(event);
}

void LinuxPinWindow::hint(const QString &text) {
    if(!hintLabel_) {hintLabel_=new QLabel(this); hintLabel_->setAttribute(Qt::WA_TransparentForMouseEvents); hintLabel_->setStyleSheet("background:#253041;color:white;padding:4px;border-radius:4px");}
    hintLabel_->setText(text); hintLabel_->adjustSize(); hintLabel_->move(3,std::max(0,height()-hintLabel_->height()-3)); hintLabel_->show(); hintLabel_->raise();
    auto *timer=hintLabel_->findChild<QTimer *>(); if(!timer){timer=new QTimer(hintLabel_);timer->setSingleShot(true);connect(timer,&QTimer::timeout,hintLabel_,&QWidget::hide);} timer->start(1200);
}
bool LinuxPinWindow::lockBlocks(const QString &kind) {
    if(!locked_ || !settings_.pinFlag("lock_block_"+kind)) return false;
    if(settings_.pinFlag("lock_hint")) hint("贴图已锁定，右键可解锁");
    return true;
}
void LinuxPinWindow::closeByKeyboard() { if(!lockBlocks("key_close")) close(); }
PinZoomAnchor LinuxPinWindow::zoomAnchor(QPointF pointer) const {
    const auto mode=settings_.pinZoomAnchor();
    if(mode=="adaptive") return PinZoomAnchor::fromPointer(layerPosition_,requestedSize_,pointer);
    QPointF r(mode.endsWith("right")?1:0,mode.startsWith("bottom")?1:0);
    if(mode=="center") r={.5,.5};
    if(mode=="mouse") r={std::clamp(pointer.x()/std::max(1,width()),0.,1.),std::clamp(pointer.y()/std::max(1,height()),0.,1.)};
    return {PinZoomAnchor::Corner::Pointer,layerPosition_+QPointF(width()*r.x(),height()*r.y()),r};
}
void LinuxPinWindow::stepZoom(double step,bool up,const PinZoomAnchor &anchor) {
    const double notch=(zoom_-1)/step;
    const double next=1+step*(up ? std::floor(notch+1e-6)+1 : std::ceil(notch-1e-6)-1);
    changeZoom(next/zoom_,anchor);
}
Qt::Edges LinuxPinWindow::resizeEdgesAt(QPointF local) const {
    if(!settings_.pinFlag("border_resize") || through_ || (canvas_ && canvas_->editingCanvasText()) || !QRectF(rect()).contains(local)) return {};
    const double margin=std::min({8.,width()/4.,height()/4.});
    Qt::Edges edges;
    if(local.x()<margin) edges|=Qt::LeftEdge; else if(local.x()>=width()-margin) edges|=Qt::RightEdge;
    if(local.y()<margin) edges|=Qt::TopEdge; else if(local.y()>=height()-margin) edges|=Qt::BottomEdge;
    return edges;
}
void LinuxPinWindow::updateResizeCursor(QWidget *target,Qt::Edges edges) {
    if(resizeCursorWidget_ && (!edges || resizeCursorWidget_!=target)) {
        resizeCursorWidget_->setCursor(resizeOriginalCursor_); resizeCursorWidget_.clear();
    }
    if(!edges) return;
    if(!resizeCursorWidget_) {resizeCursorWidget_=target; resizeOriginalCursor_=target->cursor();}
    const bool horizontal=edges.testAnyFlags(Qt::LeftEdge|Qt::RightEdge);
    const bool vertical=edges.testAnyFlags(Qt::TopEdge|Qt::BottomEdge);
    Qt::CursorShape shape=horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor;
    if(horizontal && vertical) shape=(edges.testFlag(Qt::LeftEdge)==edges.testFlag(Qt::TopEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    target->setCursor(shape);
}
bool LinuxPinWindow::eventFilter(QObject *watched,QEvent *event) {
    auto *widget=qobject_cast<QWidget *>(watched);
    if(!widget || (widget!=this && !isAncestorOf(widget)) || widget->window()!=window()) return QWidget::eventFilter(watched,event);
    const auto kind=event->type();
    if(kind==QEvent::Leave && resizeCursorWidget_==widget && !resizeEdges_) updateResizeCursor(nullptr,{});
    if(kind!=QEvent::MouseMove && kind!=QEvent::MouseButtonPress && kind!=QEvent::MouseButtonRelease) return false;
    auto *mouse=static_cast<QMouseEvent *>(event);
    // 拖边手势优先于画布绘制与标注控制点；工具栏和右上角按钮仍独立处理自己的点击。
    // Intercept only this image's edge gestures before the canvas can consume them.
    if(resizeEdges_) {
        if(kind==QEvent::MouseMove) {if(mouse->buttons().testFlag(Qt::LeftButton)) dragTo(mouse->globalPosition()); else endDrag();}
        else if(kind==QEvent::MouseButtonRelease && mouse->button()==Qt::LeftButton) endDrag();
        mouse->accept(); return true;
    }
    const auto edges=qobject_cast<QAbstractButton *>(widget) ? Qt::Edges() : resizeEdgesAt(widget->mapTo(this,mouse->position()));
    if(kind==QEvent::MouseMove && mouse->buttons()==Qt::NoButton) {
        updateResizeCursor(widget,(locked_ && settings_.pinFlag("lock_block_move")) ? Qt::Edges() : edges);
    } else if(kind==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton && mouse->modifiers()==Qt::NoModifier && edges) {
        if(beginEdgeResize(widget->mapTo(this,mouse->position()),mouse->globalPosition())) {mouse->accept();return true;}
    }
    return false;
}
bool LinuxPinWindow::beginEdgeResize(QPointF local,QPointF global) {
    const auto edges=resizeEdgesAt(local);
    if(!edges) return false;
    if(lockBlocks("move")) return true;
    // 固定操作开始时的对边，避免改变子窗口尺寸后再累加局部坐标。
    // Keep the opposite edge fixed for the gesture; never integrate relocated local coordinates.
    if(!surface_) { resizeEdges_=edges; resizeStart_={QPointF(pos()),QSizeF(size())}; dragAnchor_=global; dragging_=true; return true; }
    resizeEdges_=edges; resizeStart_={layerPosition_,QSizeF(size())}; dragAnchor_=global; dragging_=true; wheelAnchorValid_=false; return true;
}
void LinuxPinWindow::syncSurface() {
    if(!surface_) return;
    auto *host=static_cast<PinSurface *>(surface_.data()); host->shadowRect=rounded_ ? geometry() : QRect(); host->shadowOpacity=opacity_;
    QRegion input=rounded_ ? mask().translated(pos()) : QRegion(geometry()); input &= host->rect();
    if(through_) input=QRegion();
    const QRegion sentinel(-2,-2,1,1);
    // QWidget 遮罩会裁掉阴影，也会让穿透的图片本身消失。只把可操作部分提交为原生输入区域。
    // Preserve the full backing buffer for shadows/pass-through. Restrict native input separately from paint.
    // 输入区始终只设在 QWindow 上；切换 QWidget 的绘制遮罩会使透明度效果的缓存失效。
    // Keep paint independent of native input, including when restoring an opacity effect's source.
    host->clearMask(); host->windowHandle()->setMask(input.isEmpty()?sentinel:input);
    // 打开过原生菜单/标注工具栏后，Qt 可能为画布创建额外的 wl_subsurface。
    // 父表面的 input_region 不约束子表面；逐一更新已有原生子控件，并保留它们自己的绘制 mask。
    // Native menus/toolbars may promote canvas widgets to wl_subsurface. The parent's input
    // region does not restrict those surfaces, so update their native input flags independently.
    auto children=findChildren<QWidget *>(); children.prepend(this);
    for(auto *child:children) {
        if(child->isWindow()) continue;
        if(auto *native=child->windowHandle()) {
            native->setFlag(Qt::WindowTransparentForInput,through_);
            native->setMask(through_ ? sentinel : child->mask());
            child->update();
        }
    }
    host->update();
}
void LinuxPinWindow::applyAppearance() {
    rounded_=settings_.pinFlag("rounded_corners");
    if(rounded_) { QPainterPath path; path.addRoundedRect(QRectF(rect()),7,7); setMask(QRegion(path.toFillPolygon().toPolygon())); }
    else clearMask();
    syncSurface(); update();
}
void LinuxPinWindow::refreshOpenAppearance(LinuxSettings &settings) {
    for(auto *w:QApplication::allWidgets()) if(auto *pin=qobject_cast<LinuxPinWindow *>(w); pin && &pin->settings_==&settings) { pin->applyAppearance(); pin->syncToolbar(); }
}
void LinuxPinWindow::setPinOpacity(double value) {
    opacity_=std::clamp(value,.15,1.0);
    // 效果作用于整张贴图及其非原生画布；标注重叠处只混合一次，控件窗口不跟着变淡。
    // Composite the image and its non-native canvas as one group. The independent control
    // surfaces stay opaque, and no compositor-specific window-opacity extension is required.
    if(opacity_<1 && !opacityEffect_) {opacityEffect_=new QGraphicsOpacityEffect(this); setGraphicsEffect(opacityEffect_);}
    if(opacityEffect_) {opacityEffect_->setOpacity(opacity_);opacityEffect_->setEnabled(opacity_<1);}
    syncSurface(); update();
    if(canvas_) {
        canvas_->update();
        if(auto *view=canvas_->findChild<QGraphicsView *>()) view->viewport()->update();
    }
}
void LinuxPinWindow::setClickThrough(bool on) {
    if(on==through_) return;
    through_=on; endDrag();
    setAttribute(Qt::WA_TransparentForMouseEvents,on);
    auto *native=window()->windowHandle();
    if(on) {
        toolbarOpen_=false; syncToolbar();
        previousOpacity_=opacity_; setPinOpacity(std::min(previousOpacity_,settings_.pinPercent("click_through_opacity")/100.));
        // 图片的原生表面完全穿透；调节条拥有自己的小表面，两个窗口不共享输入区。
        // Give the image no native input. Only the separate, panel-sized surface accepts clicks.
        native->setFlag(Qt::WindowTransparentForInput,true);
        native->setMask(QRegion(-2,-2,1,1));
        // 独立调节窗不设 QWidget 父对象；原生窗口创建不能把贴图及其画布提升成原生兄弟。
        // Keep the panel independently owned: native tool creation must not promote the image's
        // widget ancestors or siblings to additional input-receiving Wayland surfaces.
        auto *bar=new QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint|Qt::WindowDoesNotAcceptFocus);
        throughBar_=bar; bar->setObjectName("pinThroughBar"); bar->setAttribute(Qt::WA_ShowWithoutActivating);
        bar->setAttribute(Qt::WA_TranslucentBackground);
        auto *outer=new QHBoxLayout(bar); outer->setContentsMargins(0,0,0,0);
        auto *panel=new QWidget(bar); panel->setObjectName("pinThroughPanel"); outer->addWidget(panel);
        panel->setStyleSheet("#pinThroughPanel{background:#f6f7fa;border:1px solid #aab3bf;border-radius:7px} QLabel{color:#293344;background:transparent} QPushButton{background:transparent;color:#384252;border:0;border-radius:4px;font-size:16px} QPushButton:hover{background:#f3d6d6} QSlider::groove:horizontal{height:4px;border-radius:2px;background:#cbd3de} QSlider::sub-page:horizontal{background:#4397b4;border-radius:2px} QSlider::handle:horizontal{width:12px;margin:-4px 0;border-radius:6px;background:#27778f}");
        auto *row=new QHBoxLayout(panel); row->setContentsMargins(9,3,4,3); row->setSpacing(6);
        row->addWidget(new QLabel("不透明度",panel));
        auto *slider=new QSlider(Qt::Horizontal,panel); slider->setRange(15,100); slider->setValue(qRound(opacity_*100)); slider->setFixedWidth(76); row->addWidget(slider);
        auto *value=new QLabel(QString::number(slider->value())+"%",panel); value->setFixedWidth(value->fontMetrics().horizontalAdvance("100%")); row->addWidget(value);
        auto *exit=new QPushButton(QStringLiteral("×"),panel); exit->setFixedSize(24,24); exit->setToolTip("退出鼠标穿透"); exit->setAccessibleName(exit->toolTip()); row->addWidget(exit);
        connect(exit,&QPushButton::clicked,this,[this]{setClickThrough(false);});
        connect(slider,&QSlider::valueChanged,this,[this,value](int v){setPinOpacity(v/100.); value->setText(QString::number(v)+"%");});
        bar->adjustSize(); bar->setFixedSize(bar->sizeHint()); bar->winId(); bar->windowHandle()->setScreen(screen());
        const QRect area=screen()->geometry(); const QRect pinRect=toolbarPinRect().toAlignedRect();
        int y=pinRect.top()-bar->height()-4; if(y<0) y=pinRect.bottom()+5;
        const QPoint offset(std::clamp(pinRect.left(),0,std::max(0,area.width()-bar->width())),std::clamp(y,0,std::max(0,area.height()-bar->height())));
#ifdef JIETUBA_HAS_LAYER_SHELL
        if(layer_) {
            auto *controls=LayerShellQt::Window::get(bar->windowHandle()); controls->setScope("jietuba-pin-controls"); controls->setScreen(screen());
            controls->setLayer(LayerShellQt::Window::LayerOverlay); controls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)|LayerShellQt::Window::AnchorLeft);
            controls->setMargins(QMargins(offset.x(),offset.y(),0,0)); controls->setDesiredSize(bar->size()); controls->setExclusiveZone(-1);
            controls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        } else
#endif
        bar->move(area.topLeft()+offset);
        bar->show();
    } else {
        if(throughBar_) { throughBar_->hide(); throughBar_->deleteLater(); throughBar_.clear(); }
        native->setFlag(Qt::WindowTransparentForInput,false);
        if(!surface_) native->setMask(mask());
        setPinOpacity(previousOpacity_);
    }
    syncToolbar(); syncSurface();
}
void LinuxPinWindow::startOcr(bool notify) {
#ifdef JIETUBA_HAS_TESSERACT
    if(ocrPending_) return;
    ocrStarted_=ocrPending_=true;
    if(notify) hint("正在识别贴图文字");
    // 独立队列限制并发引擎数量，避免多张贴图同时占满截图和录制的工作线程。
    // Bound OCR concurrency without taking worker threads away from captures and recordings.
    auto *workers=qApp->findChild<QThreadPool *>("pinOcrWorker",Qt::FindDirectChildrenOnly);
    if(!workers) {
        workers=new QThreadPool(qApp); workers->setObjectName("pinOcrWorker"); workers->setMaxThreadCount(1);
        connect(qApp,&QCoreApplication::aboutToQuit,workers,[workers]{workers->clear(); workers->waitForDone();});
    }
    auto *job=new QFutureWatcher<LinuxOcrResult>(this);
    connect(job,&QFutureWatcher<LinuxOcrResult>::finished,this,[this,job,notify]{
        const auto result=job->result();
        ocrPending_=false; ocrText_=result.text; job->deleteLater();
        if(notify) hint(!result.error.isEmpty() ? result.error : ocrText_.isEmpty() ? "没有识别到文字" : "识别完成，右键可复制文字");
    });
    job->setFuture(QtConcurrent::run(workers,[source=image_]{return LinuxTesseractOcr::recognize(source);}));
#else
    ocrStarted_=true;
    if(notify) hint("此构建未启用 OCR");
#endif
}
void LinuxPinWindow::pointOut() {
    hint("此内容已贴出"); if(!surface_ || through_ || dragging_) return;
    auto *animation=new QPropertyAnimation(this,"pos",this); animation->setDuration(250);
    const QPoint origin=pos(); animation->setStartValue(origin); animation->setKeyValueAt(.25,origin+QPoint(5,0)); animation->setKeyValueAt(.65,origin-QPoint(5,0)); animation->setEndValue(origin); animation->start(QAbstractAnimation::DeleteWhenStopped);
}
