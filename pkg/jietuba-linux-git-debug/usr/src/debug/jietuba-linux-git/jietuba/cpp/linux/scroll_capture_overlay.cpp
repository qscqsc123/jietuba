#include "scroll_capture_overlay.h"
#include "wayland_surface_support.h"
#include "stitch/scroll_toolbar.h"
#include <QScrollArea>
#include <QApplication>
#include <limits>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QEnterEvent>
#include <QPainterPath>
#include <QDebug>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>

LinuxScrollPreview::LinuxScrollPreview(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("scrollCaptureLivePreview"));
    opacity_ = new QGraphicsOpacityEffect(this);
    opacity_->setOpacity(1.0);
    setGraphicsEffect(opacity_);
    fade_ = new QPropertyAnimation(opacity_, "opacity", this);
    fade_->setDuration(160);
}
void LinuxScrollPreview::setImage(const QImage &image, int frameHeight, bool horizontal, int position) {
    horizontal_ = horizontal; position_ = position;
    image_ = image;
    frameHeight_ = frameHeight;
    update();
}
qreal LinuxScrollPreview::previewOpacity() const { return opacity_->opacity(); }
void LinuxScrollPreview::enterEvent(QEnterEvent *event) {
    fade_->stop(); fade_->setStartValue(opacity_->opacity()); fade_->setEndValue(0.12); fade_->start();
    QWidget::enterEvent(event);
}
void LinuxScrollPreview::leaveEvent(QEvent *event) {
    fade_->stop(); fade_->setStartValue(opacity_->opacity()); fade_->setEndValue(1.0); fade_->start();
    QWidget::leaveEvent(event);
}
void LinuxScrollPreview::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#20252c"));
    if (image_.isNull() || width() < 8) return;
    // 按固定宽度显示并跟随尾部，新帧所在范围用绿色框标出。
    // Keep a readable width, follow the tail, and outline the newest viewport in green.
    const QRectF content = QRectF(rect()).adjusted(3, 3, -3, -3);
    const qreal scale = (horizontal_ ? content.height() : content.width()) / image_.width();
    const qreal extent = image_.height();
    const qreal visible = std::min(extent, (horizontal_ ? content.width() : content.height()) / scale);
    const qreal current = position_ < 0 ? extent - frameHeight_ : position_;
    const qreal begin = std::clamp(current + frameHeight_ - visible, 0.0, std::max(0.0, extent - visible));
    const QRectF source(0, begin, image_.width(), visible);
    const QRectF target(content.topLeft(), (horizontal_ ? source.size().transposed() : source.size()) * scale);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (horizontal_) {
        // 拼接帧已经顺时针旋转。直接逆向绘制可见条带，不为每次预览分配整张横向长图。
        // Undo the stitcher's rotation only for the visible strip; never rotate/copy the entire panorama per preview.
        painter.save(); painter.translate(target.bottomLeft()); painter.rotate(-90);
        painter.drawImage(QRectF(QPointF(), target.size().transposed()), image_, source); painter.restore();
    } else painter.drawImage(target, image_, source);
    painter.setPen(QPen(QColor("#72e66e"), 2));
    const QRectF viewport = horizontal_ ? QRectF((current-begin)*scale, 0, frameHeight_*scale, target.height())
                                       : QRectF(0, (current-begin)*scale, target.width(), frameHeight_*scale);
    painter.drawRect(viewport.translated(content.topLeft()).intersected(content).adjusted(1, 1, -1, -1));
    if (begin > 0) {
        painter.fillRect(QRectF(content.topLeft(), QSizeF(content.width(), 16)), QColor(32, 37, 44, 190));
        painter.setPen(Qt::white); painter.drawText(QRectF(content.topLeft(), QSizeF(content.width(), 16)), Qt::AlignCenter,
                                                  horizontal_ ? QStringLiteral("←") : QStringLiteral("↑"));
    }
}

QRect LinuxScrollCaptureOverlay::selectionInLogicalPixels(const QSize &outputSize, const QSize &nativeSize, const QRect &nativeRegion) {
    if (outputSize.isEmpty() || nativeSize.isEmpty() || nativeRegion.isEmpty()) return {};
    // 外扩取整保证任何分数缩放下，覆盖层都不会侵入原始像素选区。
    // Round outwards so fractional scaling never places overlay pixels inside the crop.
    const double sx = double(outputSize.width()) / nativeSize.width();
    const double sy = double(outputSize.height()) / nativeSize.height();
    const int x1 = int(std::floor(nativeRegion.x() * sx));
    const int y1 = int(std::floor(nativeRegion.y() * sy));
    const int x2 = int(std::ceil((nativeRegion.x() + nativeRegion.width()) * sx));
    const int y2 = int(std::ceil((nativeRegion.y() + nativeRegion.height()) * sy));
    return QRect(x1, y1, x2 - x1, y2 - y1).intersected(QRect(QPoint(), outputSize));
}
QRect LinuxScrollCaptureOverlay::toolbarPosition(const QSize &outputSize, const QRect &selection) {
    // 原版优先选区上方，控件随可用宽度收紧；不足时再尝试下方或两侧。
    // Prefer above the crop as on Windows, falling back to other available edges.
    constexpr int gap = 6, height = 40;
    const int width = std::min(620, outputSize.width());
    if (width < 300 || outputSize.height() < height) return {};
    const int x = std::clamp(selection.left(), 0, outputSize.width() - width);
    if (selection.top() >= gap + height) return {x, selection.top() - gap - height, width, height};
    if (selection.bottom() + 1 + gap + height <= outputSize.height())
        return {x, selection.bottom() + 1 + gap, width, height};
    const int y = std::clamp(selection.top(), 0, outputSize.height() - height);
    const int rightSpace = outputSize.width() - selection.right() - 1 - gap;
    if (rightSpace >= 340) return {selection.right() + 1 + gap, std::min(y, outputSize.height() - 56), std::min(width, rightSpace), 56};
    const int leftWidth = std::min(width, selection.left() - gap);
    if (leftWidth >= 340) return {selection.left() - gap - leftWidth, std::min(y, outputSize.height() - 56), leftWidth, 56};
    return {};
}
QRect LinuxScrollCaptureOverlay::previewPosition(const QSize &outputSize, const QRect &selection, const QRect &toolbar) {
    const int gap = 12;
    const int maxHeight = std::min(520, outputSize.height() - 20);
    for (bool right : {true, false}) {
        const int available = right ? outputSize.width() - selection.right() - 1 - gap : selection.left() - gap;
        const int width = std::min(200, available);
        if (width < 110 || maxHeight < 100) continue;
        const int x = right ? selection.right() + 1 + gap : selection.left() - gap - width;
        const int y = std::clamp(selection.bottom() + 1 - maxHeight, 0, outputSize.height() - maxHeight);
        const QRect candidate(x, y, width, maxHeight);
        if (!candidate.intersects(toolbar.adjusted(-4, -4, 4, 4))) return candidate;
    }
    QRegion freeSpace(QRect(QPoint(), outputSize));
    freeSpace -= QRegion(selection.adjusted(-8, -8, 8, 8));
    freeSpace -= QRegion(toolbar.adjusted(-4, -4, 4, 4));
    QRect best;
    for (const QRect &space : freeSpace) {
        const QSize size(std::min(220, space.width()), std::min(520, space.height()));
        if (size.width() < 110 || size.height() < 100) continue;
        if (size.width() * size.height() > best.width() * best.height()) best = QRect(space.topLeft(), size);
    }
    return best;
}
LinuxScrollCaptureOverlay *LinuxScrollCaptureOverlay::create(QScreen *screen, const QSize &nativeSize, const QRect &region,
                                                            const QRect &logicalRegion) {
#ifdef JIETUBA_HAS_LAYER_SHELL
    if (!screen || !LinuxWayland::supportsLayerShell()) return nullptr;
    const QSize logicalSize = screen->geometry().size();
    // 使用实际传给采集接口的逻辑矩形，不能再经像素取整反算出偏移的边框。
    // Use the exact capture request bounds instead of round-tripping native-pixel rounding.
    const QRect selection = logicalRegion.isEmpty() ? selectionInLogicalPixels(logicalSize,nativeSize,region) : logicalRegion;
    const QRect toolbar = toolbarPosition(logicalSize, selection);
    if (selection.isEmpty() || toolbar.isEmpty()) {
        qInfo() << "长截图：选区外无工具栏空间，使用托盘入口";
        return nullptr;
    }
    auto *overlay = new LinuxScrollCaptureOverlay(screen, selection, toolbar);
    overlay->winId();
    overlay->windowHandle()->setScreen(screen);
    auto *layer = LayerShellQt::Window::get(overlay->windowHandle());
    if (!layer) { delete overlay; return nullptr; }
    layer->setScope(QStringLiteral("jietuba-scroll-capture"));
    layer->setScreen(screen);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom |
                      LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
    layer->setExclusiveZone(-1);
    layer->setDesiredSize(logicalSize);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    return overlay;
#else
    Q_UNUSED(screen); Q_UNUSED(nativeSize); Q_UNUSED(region); Q_UNUSED(logicalRegion);
    return nullptr;
#endif
}
LinuxScrollCaptureOverlay::LinuxScrollCaptureOverlay(QScreen *screen, const QRect &selection, const QRect &toolbar)
    : QWidget(nullptr), selection_(selection), toolbar_(toolbar) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(screen->geometry().size());
    // 直接嵌入 Windows 同一个 FloatingToolbar，避免 Linux 另写样式逐渐偏离原版。
    // Embed the shared Windows FloatingToolbar instead of maintaining a lookalike stylesheet.
    auto *scroll = new QScrollArea(this);
    toolbarHost_ = scroll;
    scroll->setGeometry(toolbar);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; } QScrollBar:horizontal { height: 10px; }"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    actions_ = new jietuba::FloatingToolbar(nullptr, true);
    actions_->setObjectName(QStringLiteral("scrollCaptureToolbar"));
    actions_->adjustSize();
    scroll->setWidget(actions_);
    actions_->setAutoFillBackground(false);
    actions_->auto_scroll_btn->setToolTip(QStringLiteral("自动滚动（需桌面授权）；再次点击停止"));
    actions_->direction_btn->setToolTip(QStringLiteral("切换横向/竖向，使用当前画面重新开始拼接"));
    actions_->left_handle->setToolTip(QStringLiteral("拖动工具栏（不会移入选区）；双击恢复位置"));
    connect(actions_, &jietuba::FloatingToolbar::direction_changed, this, &LinuxScrollCaptureOverlay::directionRequested);
    connect(actions_, &jietuba::FloatingToolbar::crop_requested, this, &LinuxScrollCaptureOverlay::cropRequested);
    connect(actions_, &jietuba::FloatingToolbar::auto_scroll_clicked, this, &LinuxScrollCaptureOverlay::autoScrollRequested);
    connect(actions_, &jietuba::FloatingToolbar::interaction_active, this, &LinuxScrollCaptureOverlay::interactionActive);
    connect(actions_, &jietuba::FloatingToolbar::embedded_dragged, this, [this](const QPoint &delta) {
        const QRect moved = toolbar_.translated(delta);
        if (!rect().contains(moved) || moved.intersects(selection_.adjusted(-6,-6,6,6))) return;
        toolbar_ = moved; toolbarHost_->setGeometry(toolbar_);
        if (notice_) notice_->hide();
        if (thumbnail_ && thumbnail_->geometry().intersects(toolbar_)) thumbnail_->hide();
        updateInputMask();
    });
    actions_->reposition = [this] {
        toolbar_ = toolbarPosition(size(), selection_); toolbarHost_->setGeometry(toolbar_);
        if (thumbnail_) { thumbnail_->setGeometry(previewSlot_); thumbnail_->show(); }
        updateInputMask();
    };
    actions_->crop_menu_position = [this](const QSize &menuSize) {
        QRegion freeSpace(rect()); freeSpace -= QRegion(selection_.adjusted(-6,-6,6,6));
        // 优先菜单完整位于选区外；若空间太小，控制器仍会暂停采集直至菜单关闭。
        // Prefer a fully crop-free popup; capture remains suspended even on cramped screens.
        for (const QRect &space : freeSpace) if (space.width() >= menuSize.width() && space.height() >= menuSize.height()) {
            const QPoint pos(std::clamp(toolbar_.x(), space.left(), space.right()+1-menuSize.width()),
                             std::clamp(toolbar_.y()-menuSize.height(), space.top(), space.bottom()+1-menuSize.height()));
            return mapToGlobal(pos);
        }
        return mapToGlobal(toolbar_.bottomLeft());
    };
    actions_->save_btn->setObjectName(QStringLiteral("scrollSave"));
    actions_->pin_btn->setObjectName(QStringLiteral("scrollPin"));
    actions_->finish_btn->setObjectName(QStringLiteral("scrollConfirm"));
    connect(actions_, &jietuba::FloatingToolbar::save_clicked, this, &LinuxScrollCaptureOverlay::saveRequested);
    connect(actions_, &jietuba::FloatingToolbar::ocr_clicked, this, &LinuxScrollCaptureOverlay::ocrRequested);
    connect(actions_, &jietuba::FloatingToolbar::cancel_clicked, this, &LinuxScrollCaptureOverlay::cancelRequested);
    connect(actions_, &jietuba::FloatingToolbar::pin_clicked, this, &LinuxScrollCaptureOverlay::pinRequested);
    connect(actions_, &jietuba::FloatingToolbar::finish_clicked, this, &LinuxScrollCaptureOverlay::finishRequested);
    previewSlot_ = previewPosition(size(), selection_, toolbar);
    qInfo() << "长截图覆盖层：选区" << selection_ << "工具栏" << toolbar << "预览" << previewSlot_;
    if (!previewSlot_.isEmpty()) {
        thumbnail_ = new LinuxScrollPreview(this);
        thumbnail_->setGeometry(previewSlot_);
        thumbnail_->setToolTip(QStringLiteral("长图实时预览，悬停淡化"));
    }
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("scrollCaptureNotice"));
    notice_->setWordWrap(true);
    notice_->setMargin(6);
    notice_->setStyleSheet(QStringLiteral("QLabel { color: white; background: rgba(32,37,44,240); border: 1px solid #76828d; border-radius: 5px; font-size: 10pt; }"));
    notice_->hide();
    qApp->installEventFilter(this);
    // 遮罩同时限制 Wayland 输入区域；选区内部不接收鼠标，滚轮直接落到原页面。
    // The mask excludes the crop interior from the Wayland input region.
    // 与选区保留两个逻辑像素的间隙，避免分数缩放时滤波把边框混入截图。
    // Leave a two-logical-pixel gap so fractional scaling cannot bleed into the crop.
    updateInputMask();
}
void LinuxScrollCaptureOverlay::updateInputMask() {
    const QRect outer = selection_.adjusted(-5, -5, 5, 5).intersected(rect());
    const QRegion outline = QRegion(outer).subtracted(QRegion(selection_.adjusted(-2, -2, 2, 2)));
    QRegion input = outline.united(QRegion(toolbar_));
    if (thumbnail_ && !thumbnail_->isHidden()) input += thumbnail_->geometry();
    if (notice_ && !notice_->isHidden()) input += notice_->geometry();
    setMask(input);
}
void LinuxScrollCaptureOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#40e0d0")));
    const QRect outer = selection_.adjusted(-5, -5, 5, 5);
    painter.setClipRegion(QRegion(outer).subtracted(QRegion(selection_.adjusted(-2, -2, 2, 2))));
    painter.drawRect(outer);
}
void LinuxScrollCaptureOverlay::setPreview(const QImage &image, int frames, int frameHeight, bool horizontal, int position) {
    Q_UNUSED(frames);
    const QSize displaySize = horizontal ? image.size().transposed() : image.size();
    actions_->set_result_size(displaySize.width(), displaySize.height());
    if (!thumbnail_ || image.isNull()) return;
    const int height = std::clamp(qRound(double(displaySize.height()) * (previewSlot_.width() - 6) / displaySize.width()) + 6,
                                  std::min(100, previewSlot_.height()), previewSlot_.height());
    thumbnail_->setGeometry(previewSlot_.x(), previewSlot_.bottom() + 1 - height, previewSlot_.width(), height);
    thumbnail_->setImage(image, frameHeight, horizontal, position);
    updateInputMask();
}
void LinuxScrollCaptureOverlay::setStatus(const QString &text) {
    actions_->size_label->setToolTip(text);
    if (!notice_) return;
    notice_->hide();
    const bool warning = !text.isEmpty() && !text.startsWith(QStringLiteral("已拼接")) && !text.startsWith(QStringLiteral("自动滚动中"));
    if (warning) {
        // 失败原因和悬停说明也必须位于选区外，不能让普通 Qt tooltip 污染采集帧。
        // Keep errors/help outside the crop; native tooltips could otherwise contaminate capture.
        QRegion freeSpace(rect());
        freeSpace -= selection_.adjusted(-6,-6,6,6); freeSpace -= toolbar_.adjusted(-4,-4,4,4);
        if (thumbnail_ && !thumbnail_->isHidden()) freeSpace -= thumbnail_->geometry().adjusted(-4,-4,4,4);
        QRect best; int distance = std::numeric_limits<int>::max();
        for (const QRect &space : freeSpace) {
            const QSize extent(std::min(420,space.width()), 48);
            if (extent.width() < 240 || space.height() < extent.height()) continue;
            const QRect candidate(std::clamp(toolbar_.x(),space.left(),space.right()+1-extent.width()),
                                  std::clamp(toolbar_.bottom()+6,space.top(),space.bottom()+1-extent.height()),extent.width(),extent.height());
            const int d=(candidate.center()-toolbar_.center()).manhattanLength();
            if(d < distance) { best=candidate; distance=d; }
        }
        if (!best.isEmpty()) { notice_->setText(text); notice_->setGeometry(best); notice_->show(); }
    }
    updateInputMask();
}
bool LinuxScrollCaptureOverlay::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        if (auto *widget = qobject_cast<QWidget *>(watched); widget && (widget==this || isAncestorOf(widget))) {
            setStatus(widget->toolTip()); return true;
        }
    }
    return QWidget::eventFilter(watched,event);
}

void LinuxScrollCaptureOverlay::setDirection(bool horizontal) { actions_->update_direction(horizontal ? "horizontal" : "vertical"); }
void LinuxScrollCaptureOverlay::setAutoScrolling(bool active) { actions_->set_auto_scrolling(active); }
void LinuxScrollCaptureOverlay::setControlsEnabled(bool enabled) {
    actions_->direction_btn->setEnabled(enabled); actions_->crop_btn->setEnabled(enabled);
}
