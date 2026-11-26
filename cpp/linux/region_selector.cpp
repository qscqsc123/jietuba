#include "region_selector.h"
#include "kwin_pointer_tracker.h"
#include <QApplication>
#include <QFileInfo>
#include "wayland_surface_support.h"
#include "core/theme.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>

LinuxRegionSelector::LinuxRegionSelector(QScreen *screen) : screen_(screen) {
    setObjectName(QStringLiteral("linuxLiveRegionSelector"));
    setWindowTitle(QStringLiteral("截图吧 - 框选"));
    // 工具窗口不参加 KWin 普通窗口的缩放入场动画，不改用户桌面的动画设置。
    // Tool surfaces avoid KWin's normal-window scale animation without changing desktop settings.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    // 在原生表面创建前启用 alpha，首帧就透明；不能先映射一个黑色不透明窗口再切换。
    // Request alpha before native-surface creation, so even the first frame is transparent.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_QuitOnClose, false);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    if (!screen) return;
    resize(screen->geometry().size());
    winId();
    windowHandle()->setScreen(screen);
#ifdef JIETUBA_HAS_LAYER_SHELL
    if (LinuxWayland::supportsLayerShell()) {
        if (auto *layer = LayerShellQt::Window::get(windowHandle())) {
            // KWin 按 scope 判断窗口类型；自定义名称会落入普通窗口并触发缩放。
            // KWin derives window type from scope; unknown names trigger normal-window animations.
            layer->setScope(QStringLiteral("utility"));
            layer->setScreen(screen);
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
                              LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft |
                              LayerShellQt::Window::AnchorRight);
            layer->setExclusiveZone(-1);
            layer->setDesiredSize(screen->geometry().size());
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
            layer_ = true;
        }
    }
#endif
    const auto cancel = [this] {
        if (completed_) return;
        completed_ = true;
        emit cancelled();
    };
    connect(screen, &QScreen::geometryChanged, this, cancel);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, cancel);
    connect(screen, &QObject::destroyed, this, cancel);
    ensurePolished();
}
void LinuxRegionSelector::configureAssist(bool crosshair,const QString &mode,bool animate) {
    crosshair_=crosshair; animate_=animate; elements_=mode=="element";
    if(mode=="off") return;
    snapAnimation_.setDuration(95);
    connect(&snapAnimation_,&QVariantAnimation::valueChanged,this,[this](const QVariant &v){drawn_=v.toRect(); update();});
    probeTimer_.setSingleShot(true); probeTimer_.setInterval(110);
    connect(&probeTimer_,&QTimer::timeout,this,&LinuxRegionSelector::probeElement);
    connect(&probe_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status){
        if(code || status!=QProcess::NormalExit || parked_ || dragging_ || !screen_ || hitPid_!=probePid_ || probePoint_!=hitPoint_ || window_!=probeWindow_) return;
        const auto values=probe_.readAllStandardOutput().simplified().split(' '); if(values.size()!=4) return;
        QRect r(values[0].toInt(),values[1].toInt(),values[2].toInt(),values[3].toInt());
        if(r.contains(hitPoint_)) setSnap(r.intersected(window_).intersected(screen_->geometry()).translated(-screen_->geometry().topLeft()));
    });
    auto *tracker=new KWinPointerTracker(this);
    connect(tracker,&KWinPointerTracker::windowHit,this,[this](QPoint point,QRect window,int pid){
        if(!screen_ || completed_ || parked_ || dragging_ || heldRegion_) return;
        const auto output=screen_->geometry(); hitPoint_=point; hitPid_=pid; window_=window;
        if(!output.contains(point)) window_={};
        setSnap(window_.intersected(output).translated(-output.topLeft()));
        if(elements_ && !window_.isEmpty()) probeTimer_.start();
    });
    tracker->start(true);
}
void LinuxRegionSelector::probeElement() {
    if(parked_ || dragging_ || completed_ || !hitPid_ || window_.isEmpty()) return;
    if(probe_.state()!=QProcess::NotRunning) {probeTimer_.start(); return;}
    const auto path=QCoreApplication::applicationDirPath()+"/jietuba-smart-element"; if(!QFileInfo(path).isExecutable()) return;
    probePoint_=hitPoint_; probeWindow_=window_; probePid_=hitPid_;
    probe_.start(path,{QString::number(hitPid_),QString::number(hitPoint_.x()),QString::number(hitPoint_.y()),QString::number(window_.x()),QString::number(window_.y())});
    const auto serial=++probeSerial_;
    QTimer::singleShot(1200,this,[this,serial]{if(serial==probeSerial_ && probe_.state()!=QProcess::NotRunning) probe_.kill();});
}
void LinuxRegionSelector::setSnap(QRect area) {
    if(area==target_) return;
    target_=area; snapAnimation_.stop();
    if(!animate_ || area.isEmpty() || drawn_.isEmpty()) {drawn_=area;update();return;}
    snapAnimation_.setStartValue(drawn_);snapAnimation_.setEndValue(area);snapAnimation_.start();
}
void LinuxRegionSelector::leaveEvent(QEvent *event) {
    pointer_.reset(); if(!dragging_) setSnap({}); update(); QWidget::leaveEvent(event);
}
void LinuxRegionSelector::showPrepared() {
    if (layer_) show();
    else showFullScreen();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}
void LinuxRegionSelector::setParked(bool parked) {
    parked_ = parked;
    if (parked && dragging_) {
        dragging_ = false; heldRegion_ = !region_.isEmpty();
        if (QWidget::mouseGrabber() == this) releaseMouse();
    }
    LinuxWayland::setCaptureInput(this, layer_, !parked);
    if (!parked) { activateWindow(); setFocus(); }
    update();
}
void LinuxRegionSelector::captureCurrent() {
    if (!parked_) acceptRegion((dragging_ || heldRegion_) ? region_ : QRectF(QPointF(), QSizeF(size())));
}
void LinuxRegionSelector::withdraw() {
    withdrawing_=true;
    repaint(); // 立即更新最后提交的缓冲，而不是排到 hide 之后 / Update the last buffer before hide, not after it.
    hide();
}
QRect LinuxRegionSelector::nativeRegion(const QRectF &logical, const QSize &output, const QSize &pixels) {
    if (logical.isEmpty() || output.isEmpty() || pixels.isEmpty()) return {};
    const QRectF bounds = logical.intersected(QRectF(QPointF(), QSizeF(output)));
    if (bounds.isEmpty()) return {};
    // 使用后端返回的实际像素尺寸，不用整数 DPR；外扩取整保留分数缩放边缘像素。
    // Use actual capture dimensions, not integer DPR. Round outward at fractional-scale edges.
    const double sx = double(pixels.width()) / output.width();
    const double sy = double(pixels.height()) / output.height();
    // 浮点乘法可能把整数边缘变成 126.00000000000001，直接 ceil 会多取一行。
    // A tiny tolerance prevents floating-point noise at integral edges from adding a whole pixel.
    constexpr double epsilon = 1e-7;
    const int x1 = int(std::floor(bounds.left() * sx + epsilon));
    const int y1 = int(std::floor(bounds.top() * sy + epsilon));
    const int x2 = int(std::ceil(bounds.right() * sx - epsilon));
    const int y2 = int(std::ceil(bounds.bottom() * sy - epsilon));
    return QRect(x1, y1, x2-x1, y2-y1).intersected(QRect(QPoint(), pixels));
}
QPointF LinuxRegionSelector::bounded(const QPointF &position) const {
    return {std::clamp(position.x(), 0.0, double(width())), std::clamp(position.y(), 0.0, double(height()))};
}
void LinuxRegionSelector::acceptRegion(const QRectF &region) {
    if (completed_ || region.width() < 2 || region.height() < 2) return;
    completed_ = true;
    emit selected(region);
}
void LinuxRegionSelector::mousePressEvent(QMouseEvent *event) {
    if (completed_ || parked_) return;
    if (event->button() == Qt::RightButton) { completed_ = true; emit cancelled(); return; }
    if (event->button() != Qt::LeftButton) return;
    if (heldRegion_) { if (region_.contains(event->position())) acceptRegion(region_); return; }
    start_ = bounded(event->position());
    pointer_=start_.toPoint(); pressSnap_=target_.contains(*pointer_) ? target_ : QRect();
    region_ = {};
    dragging_ = true;
    update();
}
void LinuxRegionSelector::mouseMoveEvent(QMouseEvent *event) {
    if (parked_ || completed_) return;
    pointer_=event->position().toPoint(); if (!dragging_) { if(crosshair_) update(); return; }
    region_ = QRectF(start_, bounded(event->position())).normalized();
    update();
}
void LinuxRegionSelector::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !dragging_ || completed_) return;
    dragging_ = false;
    region_ = QRectF(start_, bounded(event->position())).normalized();
    update();
    if (!pressSnap_.isEmpty() && (bounded(event->position())-start_).manhattanLength()<QApplication::startDragDistance()) acceptRegion(pressSnap_);
    else acceptRegion(region_);
}
void LinuxRegionSelector::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!parked_ && event->button() == Qt::LeftButton) acceptRegion(QRectF(QPointF(), QSizeF(size())));
}
void LinuxRegionSelector::keyPressEvent(QKeyEvent *event) {
    if (completed_ || parked_) return;
    if (event->key() == Qt::Key_Escape) { completed_ = true; emit cancelled(); }
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        captureCurrent();
    else QWidget::keyPressEvent(event);
}
void LinuxRegionSelector::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(),Qt::transparent);
    if (withdrawing_) return;
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing);
    if(crosshair_ && pointer_ && !parked_) {
        painter.setPen(QPen(jietuba::get_theme().theme_color(),1,Qt::DashLine));
        painter.drawLine(0,pointer_->y(),width(),pointer_->y()); painter.drawLine(pointer_->x(),0,pointer_->x(),height());
    }
    const QRectF outline=!dragging_ && !heldRegion_ ? QRectF(drawn_) : region_;
    if (!outline.isEmpty()) {
        // 选区内部绝不绘制边框：即使合成器暂时保留关闭动画，区域像素也不会包含绿框。
        // Draw strictly outside the selected pixels, including during a compositor's closing animation.
        const QRegion outer(outline.adjusted(-5,-5,5,5).toAlignedRect());
        const QRegion inner(outline.adjusted(-3,-3,3,3).toAlignedRect());
        painter.save(); painter.setClipRegion(outer.subtracted(inner));
        painter.fillRect(rect(),jietuba::get_theme().theme_color()); painter.restore();
    }
    // 只画一小块提示，不填充整个窗口；桌面视频、窗口和鼠标下的内容仍实时可见。
    // Paint only a compact hint; video and desktop contents remain live through the surface.
    const QString hint = !dragging_ && !heldRegion_ && !target_.isEmpty() ? QStringLiteral("单击选中目标 · 拖动自由框选 · Esc 取消")
        : region_.isEmpty() ? QStringLiteral("拖动框选 · 双击/Enter 全屏 · Esc 取消")
        : heldRegion_ && !parked_ ? QStringLiteral("点击框内或 Enter 截图 · Esc 取消")
        : QStringLiteral("%1 × %2 · 松开鼠标截图").arg(qRound(region_.width())).arg(qRound(region_.height()));
    const int w = std::min(width(), fontMetrics().horizontalAdvance(hint) + 24);
    const int h = fontMetrics().height() + 14;
    const QRect badge((width() - w) / 2, std::min(12, std::max(0, height() - h)), w, h);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(32, 37, 44, 220));
    painter.drawRoundedRect(badge, 5, 5);
    painter.setPen(Qt::white);
    painter.drawText(badge, Qt::AlignCenter, hint);
}
