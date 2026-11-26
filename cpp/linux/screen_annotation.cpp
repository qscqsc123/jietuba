#include "screen_annotation.h"
#include "annotation_canvas.h"
#include "linux_settings.h"
#include "kwin_pointer_tracker.h"
#include "wayland_surface_support.h"
#include "ui/toolbar.h"
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QUndoStack>
#include <QWindow>
#include <algorithm>
namespace {
/** 独立透明效果层，绘制效果不进入标注图层，且不会挡住桌面鼠标。
 * Transparent view-only effects, independent of annotations and mouse hit testing. */
class PointerEffects final : public QWidget {
public:
    std::function<void(QPainter &)> draw;
    explicit PointerEffects(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("annotationPointerEffects"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (draw) draw(painter);
    }
};
/**
 * 原版标注工具栏的薄适配：只补宿主专有动作，所有绘图按钮与布局继续使用共享代码。
 * 宿主固定在底部居中，禁止旧工具栏的全局坐标拖动逻辑抢占 Wayland 局部输入。
 * Thin adapter adding host-specific annotation actions to the shared toolbar/profile.
 * The host pins it bottom-center; legacy global-coordinate dragging is disabled here.
 */
class AnnotationToolbar final : public jietuba::Toolbar {
public:
    explicit AnnotationToolbar(QWidget *host):Toolbar(host,jietuba::toolbar_layout::ANNOTATION) {drag_handle->hide();}
    QPushButton *action(const QString &id,const QString &icon,const char *tip,std::function<void()> run,bool check=false) {
        auto *button=add_button(id,"svg/"+icon,tip,BASE_BTN_WIDTH,std::move(run),check); button->setObjectName(id); button->setAccessibleName(QString::fromUtf8(tip)); return button;
    }
    std::function<void()> positionRequested;
protected:
    void reposition_self() override {if(positionRequested) positionRequested();}
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object==drag_handle) return true;
        return Toolbar::eventFilter(object,event);
    }
};
}
LinuxScreenAnnotation::LinuxScreenAnnotation(QScreen *screen,LinuxSettings &settings):output_(screen) {
    setObjectName("linuxScreenAnnotation"); setWindowTitle(QStringLiteral("截图吧 — 屏幕标注"));
    setWindowFlags(Qt::Window|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_DeleteOnClose); setMouseTracking(true);
    resize(screen->geometry().size());
    auto *bar=new AnnotationToolbar(this); toolbar_=bar; bar->setObjectName("linuxAnnotationToolbar");
    pointer_=bar->action("annotation_pointer",QStringLiteral("穿透.svg"),"穿透桌面（Esc）",[this] {canvas_->activateRecordingTool("cursor"); sync();},true);
    boardButton_=bar->action("annotation_board",QStringLiteral("白板.svg"),"显示／隐藏白板",[this] {board_=boardButton_->isChecked(); sync();},true);
    highlight_=bar->action("annotation_cursor",QStringLiteral("光标高亮.svg"),"光标高亮与点击波纹",[this] {halo_=highlight_->isChecked(); sync();},true);
    bar->action("annotation_clear",QStringLiteral("清空.svg"),"清空标注（可撤销）",[this] {canvas_->clearAnnotationsUndoable(); sync();});
    hide_=bar->action("annotation_hide",QStringLiteral("隐藏标注.svg"),"隐藏／显示标注",[this] {
        hidden_=hide_->isChecked(); if(hidden_) canvas_->activateRecordingTool("cursor"); sync();
    },true);
    bar->action("annotation_exit",QStringLiteral("结束截图.svg"),"结束屏幕标注",[this] {close();});
    // 标注的按钮布局独立存储，不覆盖截图或贴图布局；与原版调整窗口共用校验和回滚。
    // Annotation layout has separate storage, sharing the original editor's validation and rollback.
    bar->set_preview_layout_storage([&settings] {return settings.annotationToolbarLayout();},[&settings](const QJsonArray &layout) {return settings.saveAnnotationToolbarLayout(layout);});
    bar->apply_scale(); bar->positionRequested=[this] {placeToolbar();};
    // 透明原版画布直接驱动同一工具栏，避免第二套按钮状态、样式面板或撤销栈不同步。
    // Drive the same toolbar from the transparent original canvas, avoiding duplicate tool/panel state.
    const QSize pixels(qRound(width()*screen->devicePixelRatio()),qRound(height()*screen->devicePixelRatio()));
    canvas_=LinuxAnnotationCanvas::createForScreenAnnotation(this,bar,pixels,settings);
    canvas_->setGeometry(rect()); canvas_->show(); bar->show(); bar->raise();
    auto *effects = new PointerEffects(this);
    effects_ = effects;
    effects->setGeometry(rect());
    effects->draw = [this](QPainter &painter) {
        if (!halo_ || hidden_) return;
        if (rect().contains(pointerPosition_.toPoint())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255,210,0,95));
            painter.drawEllipse(pointerPosition_,26,26);
        }
        for (const auto &ripple : ripples_) {
            const auto progress = std::clamp(double(clock_.elapsed()-ripple.born)/450,0.0,1.0);
            auto color = ripple.right ? QColor(60,140,255) : QColor(255,190,0);
            color.setAlphaF(.85*(1-progress));
            painter.setPen(QPen(color,4-3*progress));
            painter.setBrush(Qt::NoBrush);
            const double radius=12+40*(1-(1-progress)*(1-progress));
            painter.drawEllipse(ripple.position,radius,radius);
        }
    };
    effects->show();
    clock_.start();
    rippleTimer_.setInterval(16);
    connect(&rippleTimer_, &QTimer::timeout, this, [this] {
        ripples_.removeIf([this](const Ripple &r) { return clock_.elapsed()-r.born>=450; });
        effects_->update();
        if (ripples_.isEmpty()) rippleTimer_.stop();
    });
    tracker_=new KWinPointerTracker(this);
    connect(tracker_, &KWinPointerTracker::moved, this, [this](const QPointF &global) {
        if (!output_) return;
        pointerPosition_=global-output_->geometry().topLeft();
        effects_->update();
    });
    tracker_->start();
    canvas_->recordingChanged=[this] {
        if(!canvas_->recordingTool().isEmpty() && hidden_) {hidden_=false; hide_->setChecked(false);}
        sync();
    };
    auto *undo=canvas_->recordingUndoStack();
    connect(undo,&QUndoStack::canUndoChanged,bar->undo_btn,&QWidget::setEnabled);
    connect(undo,&QUndoStack::canRedoChanged,bar->redo_btn,&QWidget::setEnabled);
    bar->undo_btn->setEnabled(undo->canUndo()); bar->redo_btn->setEnabled(undo->canRedo());
    connect(bar,&jietuba::Toolbar::preview_layout_changed,this,[this] {placeToolbar(); sync();});
    connect(screen,&QScreen::geometryChanged,this,[this] {close();});
    connect(screen,&QScreen::logicalDotsPerInchChanged,this,[this] {close();});
    connect(screen,&QScreen::availableGeometryChanged,this,[this] {placeToolbar();});
    connect(screen,&QObject::destroyed,this,[this] {close();});
    qApp->installEventFilter(this); sync();
}
LinuxScreenAnnotation::~LinuxScreenAnnotation() {
    // 画布先于兄弟工具栏/面板销毁，提交文字和释放撤销命令时 UI 仍有效。
    // Destroy the canvas before sibling controls so text/undo teardown cannot touch freed panels.
    closing_=true; qApp->removeEventFilter(this); canvas_->recordingChanged={}; delete canvas_; canvas_=nullptr;
}
QRect LinuxScreenAnnotation::toolbarPlacement(const QRect &available,const QSize &size) {
    return QRect(QPoint(available.x()+std::max(0,(available.width()-size.width())/2),
                        available.y()+std::max(0,available.height()-size.height()-24)),size);
}
void LinuxScreenAnnotation::placeToolbar() {
    if(!output_ || placing_) return;
    placing_=true;
    toolbar_->set_preview_available_width(width());
    const auto available=output_->availableGeometry().translated(-output_->geometry().topLeft()).intersected(rect());
    toolbar_->move(toolbarPlacement(available,toolbar_->size()).topLeft()); toolbar_->drag_handle->hide();
    if(canvas_) canvas_->layoutRecordingPanels();
    placing_=false;
}
void LinuxScreenAnnotation::sync() {
    if(closing_) return;
    const bool drawing=!canvas_->recordingTool().isEmpty() && !hidden_;
    pointer_->setChecked(!drawing); canvas_->setVisible(!hidden_); canvas_->setAttribute(Qt::WA_TransparentForMouseEvents,!drawing);
    canvas_->setAnnotationAppearance(board_,std::nullopt);
    effects_->setVisible(!hidden_ && halo_);
    effects_->raise();
    toolbar_->raise(); canvas_->layoutRecordingPanels(); refreshInput(); update();
}
void LinuxScreenAnnotation::refreshInput() {
    QRegion input;
    if(!hidden_ && !canvas_->recordingTool().isEmpty()) input=rect();
    for(auto *child:findChildren<QWidget *>(QString(),Qt::FindDirectChildrenOnly))
        if(child!=canvas_ && child!=effects_ && !child->isHidden() && !child->isWindow()) input|=child->geometry();
    if(windowHandle()) windowHandle()->setMask(input.isEmpty() ? QRegion(QRect(-2,-2,1,1)) : input.intersected(rect()));
}
void LinuxScreenAnnotation::showPrepared() {
    if(!output_) return;
    winId(); windowHandle()->setScreen(output_);
#ifdef JIETUBA_HAS_LAYER_SHELL
    if(LinuxWayland::supportsLayerShell()) if(auto *layer=LayerShellQt::Window::get(windowHandle())) {
        layer->setScope("jietuba-screen-annotation"); layer->setScreen(output_); layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)|LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft|LayerShellQt::Window::AnchorRight);
        layer->setDesiredSize(size()); layer->setExclusiveZone(-1); layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        placeToolbar(); refreshInput(); show(); return;
    }
#endif
    move(output_->geometry().topLeft()); placeToolbar(); refreshInput();
    if(QGuiApplication::platformName().startsWith("wayland")) showFullScreen(); else show();
}
bool LinuxScreenAnnotation::eventFilter(QObject *object,QEvent *event) {
    auto *widget=qobject_cast<QWidget *>(object);
    if(!widget || widget->window()!=this || closing_) return QWidget::eventFilter(object,event);
    if(event->type()==QEvent::KeyPress) {
        auto *key=static_cast<QKeyEvent *>(event);
        if(key->key()==Qt::Key_Escape && !canvas_->editingCanvasText()) {
            if(toolbar_->preview_layout_dialog_open()) toolbar_->cancel_preview_layout_dialog();
            else {canvas_->activateRecordingTool("cursor"); sync();} return true;
        }
    }
    if(event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonPress) {
        auto *mouse=static_cast<QMouseEvent *>(event);
        pointerPosition_=widget->mapTo(this,mouse->position());
        if (halo_ && !hidden_ && event->type()==QEvent::MouseButtonPress
            && (mouse->button()==Qt::LeftButton || mouse->button()==Qt::RightButton)) {
            ripples_.append({pointerPosition_,clock_.elapsed(),mouse->button()==Qt::RightButton});
            rippleTimer_.start();
        }
        effects_->update();
    }
    if((event->type()==QEvent::Show || event->type()==QEvent::Hide || event->type()==QEvent::Move || event->type()==QEvent::Resize) && !queued_) {
        queued_=true; QTimer::singleShot(0,this,[this] {queued_=false; refreshInput();});
    }
    return QWidget::eventFilter(object,event);
}
void LinuxScreenAnnotation::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(),Qt::transparent);
}
void LinuxScreenAnnotation::closeEvent(QCloseEvent *event) {
    closing_=true; canvas_->activateRecordingTool("cursor"); canvas_->hide(); toolbar_->hide(); board_=false; hidden_=true; update(); repaint(); QWidget::closeEvent(event);
}
