#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/items/selection_item.h"
#include "canvas/handle_editor.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/background_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/watermark_item.h"
#include "core/export.h"
#include "core/theme.h"
#include "core/i18n.h"
#include "tools/controller.h"
#include "tools/pen.h"
#include "tools/rect.h"
#include "tools/ellipse.h"
#include "tools/arrow.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include "tools/highlighter.h"
#include "tools/spotlight.h"
#include "ui/toolbar.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/arrow_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/watermark_settings_panel.h"
#include "ui/watermark_input_dialog.h"
#include "capture/capture_router.h"
#include "global_shortcut.h"
#include "linux_settings.h"
#include "capture_delivery.h"
#include "result_placement.h"
#include "settings_window.h"
#include "region_selector.h"
#include "pin_window.h"
#include "pin_toolbar.h"
#include "annotation_canvas.h"
#include "screen_annotation.h"
#include "quick_translation_window.h"
#include "scroll_capture_window.h"
#include "gif_capture_window.h"
#include "wayland_surface_support.h"
#include "ocr_result_window.h"
#include "ppocr_wrapper.h"
#include "barcode_result_window.h"
#include "translation_result_window.h"
#include "core/ui_theme.h"
#include "ui/magnifier.h"
#include "ui/color_picker_button.h"
#include "shortcut_bindings.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QGraphicsView>
#include <QImageReader>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QDateTime>
#include <QScreen>
#include <QWindow>
#include <QSignalBlocker>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextOption>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

// 样式快照包含绘图笔和文字效果；它独立于控制点几何状态。
// Style snapshots cover pens and text effects independently of geometry handles.
struct PreviewStyle {
    QPen pen;
    QFont font;
    QColor textColor, background, outline, shadow;
    bool hasBackground = false, hasOutline = false, hasShadow = false;
    double outlineWidth = 0;
    QString arrowStyle;
    QString numberStyle;
    jietuba::MosaicKind mosaicKind = jietuba::MosaicKind::Pixelate;
    int mosaicBlock = 10;
    qreal mosaicWidth = 30;
    bool operator==(const PreviewStyle &) const = default;
};

static PreviewStyle readStyle(QGraphicsItem *item) {
    PreviewStyle s;
    if (auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(item)) s.pen = shape->pen();
    if (auto *stroke = dynamic_cast<jietuba::StrokeItem *>(item); stroke && stroke->is_highlighter)
        s.pen.setWidthF(stroke->get_stroke_width().value_or(s.pen.widthF()));
    if (auto *arrow = dynamic_cast<jietuba::ArrowItem *>(item)) {
        s.arrowStyle = arrow->arrow_style();
        s.pen = QPen(arrow->color, arrow->base_width);
    }
    if (auto *number = dynamic_cast<jietuba::NumberItem *>(item)) {
        s.pen = QPen(number->color, number->get_stroke_width().value_or(16)); s.numberStyle = number->style;
    }
    if (auto *text = dynamic_cast<jietuba::TextItem *>(item)) {
        s.font = text->font(); s.textColor = text->defaultTextColor();
        s.hasBackground = text->has_background; s.background = text->background_color;
        s.hasOutline = text->has_outline; s.outline = text->outline_color; s.outlineWidth = text->outline_width;
        s.hasShadow = text->has_shadow; s.shadow = text->shadow_color;
    }
    if (auto *mosaic = dynamic_cast<jietuba::MosaicItem *>(item)) {
        s.mosaicKind = mosaic->kind(); s.mosaicBlock = mosaic->block_size(); s.mosaicWidth = mosaic->brush_width();
    }
    return s;
}

static void applyStyle(QGraphicsItem *item, const PreviewStyle &s) {
    if (auto *stroke = dynamic_cast<jietuba::StrokeItem *>(item)) {
        // 原版荧光笔实际线宽为面板值的三倍；快照只保存用户看到的宽度。
        // The original highlighter draws three times the logical panel width.
        auto pen = s.pen;
        if (stroke->is_highlighter) pen.setWidthF(pen.widthF() * 3);
        stroke->setPen(pen);
    }
    else if (auto *rect = dynamic_cast<jietuba::RectItem *>(item)) {
        rect->setPen(s.pen);
        if (rect->is_highlighter_rect) rect->setBrush(QBrush(s.pen.color()));
    }
    else if (auto *ellipse = dynamic_cast<jietuba::EllipseItem *>(item)) ellipse->setPen(s.pen);
    else if (auto *arrow = dynamic_cast<jietuba::ArrowItem *>(item)) { arrow->color = s.pen.color(); arrow->set_stroke_width(s.pen.widthF()); arrow->set_arrow_style(s.arrowStyle); }
    else if (auto *number = dynamic_cast<jietuba::NumberItem *>(item)) {
        number->color = s.pen.color(); number->set_stroke_width(s.pen.widthF()); number->set_style(s.numberStyle);
    }
    if (auto *text = dynamic_cast<jietuba::TextItem *>(item)) {
        text->setFont(s.font); text->setDefaultTextColor(s.textColor);
        text->set_background(s.hasBackground, s.background, s.background.alpha());
        text->set_outline(s.hasOutline, s.outline, s.outlineWidth);
        text->set_shadow(s.hasShadow, s.shadow);
    }
    if (auto *mosaic = dynamic_cast<jietuba::MosaicItem *>(item)) {
        // 按当前画布背景重新取得缩小图，避免撤销命令保留整屏图片副本。
        // Reuse the owning background cache; undo commands do not store full-screen images.
        if (auto *scene = dynamic_cast<jietuba::CanvasScene *>(mosaic->scene()); scene && scene->background) {
            const auto reduced = scene->background->reduced_image(s.mosaicBlock);
            if (!reduced.isNull()) mosaic->set_block_size(s.mosaicBlock, reduced);
        }
        mosaic->set_kind(s.mosaicKind);
        mosaic->set_brush_width(s.mosaicWidth);
    }
    item->update();
}

// 通过已有 ItemReference 保持撤销命令中的对象生命周期安全。
// ItemReference keeps undo commands safe when an item leaves the scene.
class PreviewStyleCommand : public QUndoCommand {
public:
    PreviewStyleCommand(jietuba::TrackedItem *item, PreviewStyle before, PreviewStyle after)
        : item_(item), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { if (auto *item = item_.get()) applyStyle(item, before_); }
    void redo() override { if (auto *item = item_.get()) applyStyle(item, after_); }
private:
    jietuba::ItemReference item_;
    PreviewStyle before_, after_;
};

/**
 * 水印文本和样式快照：保留原有时间戳，撤销后不重新计算日期变量。
 * Stores watermark text/style snapshots and their timestamps for deterministic undo.
 */
class WatermarkChangeCommand final : public QUndoCommand {
public:
    struct State { QString text; QDateTime time; jietuba::WatermarkStyle style; };
    WatermarkChangeCommand(jietuba::WatermarkItem *item, State before, State after, QString key)
        : item_(item), before_(std::move(before)), after_(std::move(after)), key_(std::move(key)), stamp_(QDateTime::currentMSecsSinceEpoch()) {}
    void undo() override { apply(before_); }
    void redo() override { apply(after_); }
    int id() const override { return key_.isEmpty() ? -1 : 0x574d; }
    bool mergeWith(const QUndoCommand *other) override {
        const auto *next = dynamic_cast<const WatermarkChangeCommand *>(other);
        if (!next || item_.get() != next->item_.get() || key_ != next->key_ || next->stamp_ - stamp_ > 500) return false;
        after_ = next->after_; stamp_ = next->stamp_; return true;
    }
private:
    void apply(const State &state) {
        if (auto *item = item_.as<jietuba::WatermarkItem>()) { item->set_template(state.text, state.time); item->set_style(state.style); }
    }
    jietuba::ItemReference item_;
    State before_, after_;
    QString key_;
    qint64 stamp_;
};

/**
 * 聚光灯共享一块遮罩；透明度变化记录在场景级命令中。
 * The spotlight curtain is shared by all holes, so its opacity is a scene-level edit.
 */
class SpotlightOpacityCommand final : public QUndoCommand {
public:
    SpotlightOpacityCommand(jietuba::CanvasScene *scene, qreal before, qreal after) : scene_(scene), before_(before), after_(after) {}
    void undo() override { apply(before_); }
    void redo() override { apply(after_); }
private:
    void apply(qreal opacity) { if (scene_) if (auto *curtain = jietuba::SpotlightCurtain::find(scene_)) curtain->setOpacity(opacity); }
    QPointer<jietuba::CanvasScene> scene_;
    qreal before_, after_;
};

static QString annotationTool(QGraphicsItem *item) {
    QString id;
    if (dynamic_cast<jietuba::SpotlightItem *>(item)) id = "spotlight";
    else if (auto *stroke = dynamic_cast<jietuba::StrokeItem *>(item)) id = stroke->is_highlighter ? "highlighter" : "pen";
    else if (auto *rect = dynamic_cast<jietuba::RectItem *>(item)) id = rect->is_highlighter_rect ? "highlighter" : "rect";
    else if (dynamic_cast<jietuba::EllipseItem *>(item)) id = "ellipse";
    else if (dynamic_cast<jietuba::ArrowItem *>(item)) id = "arrow";
    else if (dynamic_cast<jietuba::TextItem *>(item)) id = "text";
    else if (dynamic_cast<jietuba::MosaicItem *>(item)) id = "mosaic";
    else if (dynamic_cast<jietuba::NumberItem *>(item)) id = "number";
    return id;
}

// 新文字根据选区右边界自动换行，同时保持短文字框紧贴内容。
// Wrap new text at the selection's right edge while keeping short text compact.
static void installTextWrapping(jietuba::TextItem *text, jietuba::SelectionModel *selection) {
    QTextOption option = text->document()->defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    text->document()->setDefaultTextOption(option);
    const auto updateWidth = [text, selection] {
        if (text->property("preview_wrap_updating").toBool() || selection->is_empty()) return;
        text->setProperty("preview_wrap_updating", true);
        // mapFromScene 同时考虑移动与缩放；选区右侧没有空间时仍保留最小编辑宽度。
        // mapFromScene accounts for movement and scaling; retain a minimum editable width.
        const qreal available = text->mapFromScene(QPointF(selection->rect().right(), text->scenePos().y())).x();
        const qreal limit = std::max(1.0, double(available));
        text->setTextWidth(-1);
        const qreal natural = text->document()->idealWidth();
        text->setTextWidth(std::min(limit, std::max(1.0, double(natural))));
        text->setProperty("preview_wrap_updating", false);
    };
    QObject::connect(text->document(), &QTextDocument::contentsChanged, text, updateWidth);
    QObject::connect(text->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, text,
                     [updateWidth](const QSizeF &) { updateWidth(); });
    QObject::connect(text, &QGraphicsObject::xChanged, text, updateWidth);
    QObject::connect(selection, &jietuba::SelectionModel::rectChanged, text, [updateWidth](const QRectF &) { updateWidth(); });
    updateWidth();
}

// 画布、工具、撤销命令和导出渲染复用原项目；此视图负责 Linux 预览的输入与坐标。
// The canvas, tools, undo commands and export renderer are shared with the Windows app.
// This view adapts input and coordinates for the Linux image preview.
class PreviewView : public QGraphicsView, public jietuba::NumberRefreshContext {
public:
    explicit PreviewView(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setRenderHint(QPainter::Antialiasing);
        setMouseTracking(true);
    }
    jietuba::CanvasScene *canvas = nullptr;
    std::function<void()> selectionChanged;
    std::function<void()> selectionFinished;
    std::function<void(QGraphicsItem *)> itemSelected;
    std::function<void(int)> nextNumberChanged;
    // 原始图片像素位置供放大镜采样；离开视口后立即隐藏。
    // Pass source-pixel positions to the magnifier; clear when leaving the viewport.
    std::function<void(std::optional<QPointF>)> pointerChanged;
    std::function<void()> beginNewDrawing;
    std::function<bool(QKeyEvent *)> precisionKey;
    std::function<bool(QWheelEvent *)> precisionWheel;
    // 合并撤销命令时栈索引可能不变；原版编号逻辑会主动通知此接口。
    // Merged undo commands may not change the stack index, so accept explicit refreshes too.
    void refresh_number_toolbar(int value) override { if (nextNumberChanged) nextNumberChanged(value); }
    void refresh_number_cursor_if_active(bool) override {}
    const LinuxSettings *preferences = nullptr;
    QString tool;
    bool drawing = false;
    QPointF start;
    std::unique_ptr<jietuba::LayerEditor> editor;
    std::optional<QVariantMap> moveBefore;
    QPointF moveStart, itemStart;
    bool moving = false;
    PreviewStyle newTextStyle;

    // 控制点使用视口坐标绘制，但模型和撤销命令始终使用图片坐标。
    // Handles are painted in viewport coordinates; model and undo use image coordinates.
    void setCanvas(jietuba::CanvasScene *scene) {
        editor.reset();
        canvas = scene;
        if (!scene) return;
        editor = std::make_unique<jietuba::LayerEditor>();
        editor->screen_transform = [this] { return viewportTransform(); };
        editor->repaint_requested = [this] { viewport()->update(); };
        connect(scene->undo_stack, &QUndoStack::indexChanged, this, [this] {
            if (!editor || !editor->active_layer()) return;
            if (!editor->active_layer()->scene()) editor->stop_edit();
            else editor->refresh_handles();
            viewport()->update();
        });
    }

    LinuxPinWindow *pin = nullptr;
    bool draggingPin = false;
    bool fullScreenCapture = false;
    bool regionCapture = false;
    bool recording = false;
    bool annotationWhiteboard=false;
    std::optional<QPointF> annotationPointer;
    bool withdrawing = false;
    void fit() {
        if (!canvas || canvas->sceneRect().isEmpty()) return;
        if (!fullScreenCapture) { fitInView(canvas->sceneRect(), Qt::KeepAspectRatio); return; }
        // 全屏画布的坐标是截图原始像素；视口是 Qt 逻辑像素。
        // Map native screenshot pixels to the full logical viewport without fitInView margins.
        resetTransform();
        scale(viewport()->width() / canvas->sceneRect().width(), viewport()->height() / canvas->sceneRect().height());
        centerOn(canvas->sceneRect().center());
    }
protected:
    void paintEvent(QPaintEvent *event) override {
        if (withdrawing) {
            // 原生窗口隐藏前提交完全透明的视口，不能让关闭动画保留选区边框或标注。
            // Submit a transparent viewport before unmapping; closing animations must not retain crop decorations.
            QPainter painter(viewport()); painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(viewport()->rect(),Qt::transparent); return;
        }
        QGraphicsView::paintEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        // QGraphicsView 默认消费方向键，先让源像素微调处理，文字编辑仍交回场景。
        // QGraphicsView consumes arrows; offer pixel nudges first while preserving scene text editing.
        if (precisionKey && precisionKey(event)) return;
        QGraphicsView::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override {
        if (pin) { pin->applyPinWheel(event,viewport()->mapTo(pin,event->position())); return; }
        if (precisionWheel && precisionWheel(event)) return;
        QGraphicsView::wheelEvent(event);
    }
    void resizeEvent(QResizeEvent *event) override { QGraphicsView::resizeEvent(event); fit(); if (selectionChanged) selectionChanged(); }
    void drawBackground(QPainter *p, const QRectF &rect) override {
        if (regionCapture || pin) {
            // 透明画布每帧先清除旧像素；旧框与旧底图不能残留在新的选区外。
            // Clear the alpha surface before rendering: previous crops must never remain as ghost rectangles.
            p->save(); p->resetTransform(); p->setCompositionMode(QPainter::CompositionMode_Source);
            p->fillRect(viewport()->rect(), recording && annotationWhiteboard ? QColor(Qt::white) : QColor(Qt::transparent)); p->restore();
        }
        QGraphicsView::drawBackground(p, rect);
    }
    void drawForeground(QPainter *p, const QRectF &) override {
        if (!canvas) return;
        // 光圈放在视图前景，避免透明画布的 Source 清屏把父窗口绘制的效果擦掉。
        // Paint the halo in the viewport foreground, above the transparent Source-cleared canvas.
        if(recording && annotationPointer) {p->save(); p->resetTransform(); p->setPen(Qt::NoPen); p->setBrush(QColor(255,210,30,100)); p->drawEllipse(*annotationPointer,22,22); p->restore();}
        if (pin) {
            p->save();
            p->resetTransform();
            pin->paintBorder(*p);
            p->restore();
        } else if (!recording) {
            p->save();
            QPainterPath mask;
            mask.setFillRule(Qt::OddEvenFill);
            mask.addRect(canvas->sceneRect());
            const auto selection = canvas->selection_model->rect();
            if (!selection.isEmpty()) mask.addRect(selection);
            if (!regionCapture) p->fillPath(mask, jietuba::get_theme().mask_color());
            canvas->selection_item->render(p);
            p->restore();
        }
        if (editor && editor->is_editing()) {
            p->save();
            p->resetTransform();
            editor->render(p);
            p->restore();
        }
    }
    QPointF bounded(const QPointF &p, const QRectF &r) const {
        return {std::clamp(p.x(), r.left(), r.right()), std::clamp(p.y(), r.top(), r.bottom())};
    }
    void leaveEvent(QEvent *event) override { if (pointerChanged) pointerChanged(std::nullopt); QGraphicsView::leaveEvent(event); }
    void mousePressEvent(QMouseEvent *e) override {
        if (pointerChanged) pointerChanged(std::nullopt);
        if (!canvas || e->button() != Qt::LeftButton) { QGraphicsView::mousePressEvent(e); return; }
        auto pos = mapToScene(e->position().toPoint());
        auto &ctx = canvas->tool_controller->context();
        ctx.shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
        ctx.lineAngle = preferences ? preferences->shiftLineAngle() : 45;
        ctx.textOnTop = !preferences || preferences->textAlwaysOnTop();
        const bool cross = e->modifiers().testFlag(Qt::ControlModifier) && (!preferences || preferences->crossToolSelection());
        // 橡皮擦优先处理鼠标输入，不能被已有图形的选中或控制点拦截。
        // Erasing must take precedence over selecting existing items and their handles.
        if (!cross && tool == "eraser" && canvas->selection_model->is_confirmed() && canvas->selection_model->rect().contains(pos)) {
            if (editor) editor->stop_edit();
            drawing = canvas->tool_controller->on_press(pos, e->button());
            viewport()->update(); return;
        }
        if (auto *label = dynamic_cast<jietuba::NumberLabel *>(canvas->focusItem()); label && label->is_editing() && label->contains(label->mapFromScene(pos))) {
            QGraphicsView::mousePressEvent(e); return;
        }
        // 命中现有控制点时优先处理它，避免一次点击同时开始新绘图。
        // Handle hits take precedence over creating another drawing.
        if (editor && editor->is_editing()) {
            if (const auto handle = editor->hit_test(pos)) {
                if (jietuba::LayerEditor::is_number_adjust_handle(handle)) {
                    editor->adjust_number_with_handle(*handle, canvas->undo_stack); editor->refresh_handles(); viewport()->update(); return;
                }
                if (jietuba::LayerEditor::is_number_action_handle(handle)) {
                    editor->run_number_action(*handle, canvas->undo_stack); viewport()->setFocus();
                    if (itemSelected) itemSelected(editor->active_layer());
                    editor->refresh_handles(); viewport()->update(); return;
                }
                if (jietuba::LayerEditor::is_delete_handle(handle)) {
                    // 删除同样进入撤销栈，撤销后仍可恢复原对象。
                    // Deletion uses the original undo stack so the item can be restored.
                    if (auto *tracked = dynamic_cast<jietuba::TrackedItem *>(editor->active_layer())) {
                        jietuba::ItemReference item(tracked);
                        const bool number = dynamic_cast<jietuba::NumberItem *>(editor->active_layer());
                        editor->stop_edit();
                        if (number) canvas->undo_stack->push(new jietuba::RemoveNumberCommand(canvas, std::move(item)));
                        else canvas->undo_stack->push(new jietuba::RemoveItemCommand(canvas, std::move(item)));
                    }
                    viewport()->update();
                    return;
                }
                editor->start_drag(*handle, pos);
                viewport()->update();
                return;
            }
        }
        if (canvas->selection_model->is_confirmed() && canvas->selection_model->rect().contains(pos)) {
            for (auto *part : canvas->items(pos)) {
                auto *item = jietuba::annotation_owner(part);
                // 当前工具照常编辑自己的标注；只有 Ctrl 跨选才交给其他工具的图元。
                // Ordinary hits stay with the current tool. Ctrl explicitly permits picking another tool's item.
                if (!tool.isEmpty() && tool != annotationTool(item) && !cross) continue;
                if (tool == "highlighter" && !(e->modifiers() & Qt::ControlModifier) && (!editor || item != editor->active_layer())) {
                    // 与原版一致：普通拖动用于涂抹；矩形荧光标记可直接选中，其他图元用 Ctrl 选中。
                    // Match the original: draw over other items; Ctrl selects freehand or other annotations.
                    auto *highlightRect = dynamic_cast<jietuba::RectItem *>(item);
                    if (!highlightRect || !highlightRect->is_highlighter_rect) continue;
                }
                if (dynamic_cast<jietuba::StrokeItem *>(item) || dynamic_cast<jietuba::RectItem *>(item) ||
                    dynamic_cast<jietuba::EllipseItem *>(item) || dynamic_cast<jietuba::ArrowItem *>(item) ||
                    dynamic_cast<jietuba::TextItem *>(item) || dynamic_cast<jietuba::MosaicItem *>(item) || dynamic_cast<jietuba::NumberItem *>(item)) {
                    if (canvas->focusItem() && canvas->focusItem() != item) canvas->focusItem()->clearFocus();
                    editor->start_edit(item);
                    if (itemSelected) itemSelected(item);
                    moveBefore = editor->capture_state(item);
                    moveStart = pos;
                    itemStart = item->pos();
                    moving = true;
                    viewport()->update();
                    return;
                }
            }
            if (editor && editor->is_editing()) { editor->stop_edit(); viewport()->update(); }
        }
        if (!pin && !recording && tool.isEmpty() && canvas->selection_model->is_confirmed()
            && canvas->selection_item->hit_test(pos) != jietuba::SelectionItem::HANDLE_NONE) {
            // 原版 SelectionItem 负责八个控制点和整体移动；不再把按下事件当成重新框选。
            // Dispatch crop handles/body to the original SelectionItem instead of starting another rectangle.
            QGraphicsView::mousePressEvent(e);
            if (canvas->selection_item->active_handle != jietuba::SelectionItem::HANDLE_NONE) {
                if (selectionChanged) selectionChanged();
                viewport()->update(); return;
            }
        }
        if (tool.isEmpty()) {
            if (recording) { e->ignore(); return; }
            // QGraphicsView/viewport 会消费或重写事件接受状态，不能靠 ignore 向父窗口冒泡。
            // 在真实按下回调中直接开始拖动；使用固定原生表面的鼠标屏幕坐标之差，
            // 不把已经随贴图改变的 viewport 局部坐标反复累计到窗口位置。
            // Begin directly during the real press. Pointer deltas on the fixed native surface remain stable;
            // moving viewport-local coordinates must never be accumulated into the pin position.
            if (pin) {
                draggingPin = pin->beginEdgeResize(viewport()->mapTo(pin,e->position()),e->globalPosition()) || pin->beginDrag(e->globalPosition());
                e->accept(); return;
            }
            // 已确认的框只允许移动和拉伸；空白处不能开始另一轮框选。
            // After confirmation, background gestures never replace the existing crop.
            if (regionCapture && canvas->selection_model->is_confirmed()) { e->accept(); return; }
            if (!canvas->sceneRect().contains(pos)) return;
            start = pos;
            canvas->selection_model->deactivate();
            canvas->selection_model->activate();
            canvas->selection_model->start_dragging();
            if (selectionChanged) selectionChanged();
            viewport()->update();
            return;
        }
        const auto r = canvas->selection_model->rect();
        if (!r.contains(pos)) return;
        // 水印是覆盖整个选区的图层，通过面板调整，不从画布拖动新图形。
        // Watermark is a selection-wide layer configured through its panel, not a drawing gesture.
        if (tool == "watermark") return;
        if (beginNewDrawing) beginNewDrawing();
        if (tool == "text" && canvas->focusItem()) canvas->focusItem()->clearFocus();
        drawing = canvas->tool_controller->on_press(pos, e->button());
        if (tool == "text") {
            if (auto *text = dynamic_cast<jietuba::TextItem *>(canvas->focusItem())) {
                applyStyle(text, newTextStyle);
                installTextWrapping(text, canvas->selection_model);
                if (editor) editor->start_edit(text);
            }
            drawing = false;
            viewport()->setFocus();
        }
        viewport()->update();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        if (canvas && e->button() == Qt::LeftButton) {
            const auto pos = mapToScene(e->position().toPoint());
            for (auto *item : canvas->items(pos)) {
                if (auto *text = dynamic_cast<jietuba::TextItem *>(item)) {
                    // 双击文字时把键盘焦点交还给原版文字对象。
                    // Double-click returns keyboard focus to the original text item.
                    if (editor) editor->stop_edit();
                    moving = false;
                    moveBefore.reset();
                    text->setFocus(Qt::MouseFocusReason);
                    viewport()->setFocus();
                    return;
                }
            }
        }
        QGraphicsView::mouseDoubleClickEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (pin && draggingPin) {
            if (e->buttons() & Qt::LeftButton) pin->dragTo(e->globalPosition());
            else { pin->endDrag(); draggingPin=false; }
            e->accept(); return;
        }
        if (!canvas) return;
        auto pos = mapToScene(e->position().toPoint());
        if (pointerChanged) pointerChanged(pos);
        if (editor && editor->dragging_handle) {
            editor->drag_to(pos, e->modifiers().testFlag(Qt::ShiftModifier));
            viewport()->update();
            return;
        }
        if (moving && editor && editor->active_layer()) {
            editor->active_layer()->setPos(itemStart + pos - moveStart);
            editor->refresh_handles();
            viewport()->update();
            return;
        }
        if (!pin && canvas->selection_item->active_handle != jietuba::SelectionItem::HANDLE_NONE) {
            QGraphicsView::mouseMoveEvent(e);
            auto rect = canvas->selection_model->rect();
            const auto bounds = canvas->sceneRect();
            if (canvas->selection_item->active_handle == jietuba::SelectionItem::HANDLE_BODY) {
                rect.moveLeft(std::clamp(rect.left(), bounds.left(), bounds.right()-rect.width()));
                rect.moveTop(std::clamp(rect.top(), bounds.top(), bounds.bottom()-rect.height()));
            } else rect = rect.intersected(bounds);
            canvas->selection_model->set_rect(rect);
            viewport()->update(); return;
        }
        if (canvas->selection_model->is_dragging()) {
            auto r = QRectF(start, bounded(pos, canvas->sceneRect())).normalized();
            canvas->selection_model->set_rect(r);
            viewport()->update();
        } else if (drawing) {
            // 读取本次鼠标事件的修饰键，支持在一笔中按下或松开 Shift。
            // Use this mouse event's modifiers so Shift can change during a stroke.
            canvas->tool_controller->context().shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
            canvas->tool_controller->on_move(bounded(pos, canvas->selection_model->rect()));
            viewport()->update();
        } else if (pin && tool.isEmpty()) e->ignore();
        else QGraphicsView::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (pin && draggingPin && e->button()==Qt::LeftButton) {
            pin->endDrag(); draggingPin=false; e->accept(); return;
        }
        if (!canvas || e->button() != Qt::LeftButton) { QGraphicsView::mouseReleaseEvent(e); return; }
        if (editor && editor->dragging_handle) {
            editor->end_drag(canvas->undo_stack);
            viewport()->update();
            return;
        }
        if (moving && editor && editor->active_layer()) {
            // 仅在几何状态改变时提交一条原版撤销命令。
            // Commit a single original undo command only when geometry changed.
            auto *item = editor->active_layer();
            const auto after = editor->capture_state(item);
            if (moveBefore && after && *moveBefore != *after)
                if (auto *tracked = dynamic_cast<jietuba::TrackedItem *>(item))
                    canvas->undo_stack->push(new jietuba::EditItemCommand(jietuba::ItemReference(tracked), *moveBefore, *after));
            moving = false;
            moveBefore.reset();
            editor->refresh_handles();
            viewport()->update();
            return;
        }
        if (!pin && canvas->selection_item->active_handle != jietuba::SelectionItem::HANDLE_NONE) {
            const bool changed = canvas->selection_model->rect() != canvas->selection_item->start_rect;
            QGraphicsView::mouseReleaseEvent(e);
            if (!changed && regionCapture) canvas->background->show();
            if (selectionChanged) selectionChanged();
            if (changed && selectionFinished) selectionFinished();
            viewport()->update(); return;
        }
        if (canvas->selection_model->is_dragging()) {
            canvas->selection_model->stop_dragging();
            if (canvas->selection_model->is_active()) canvas->confirm_selection();
            if (selectionChanged) selectionChanged();
            if (selectionFinished) selectionFinished();
            viewport()->update();
        } else if (drawing) {
            const auto p = bounded(mapToScene(e->position().toPoint()), canvas->selection_model->rect());
            canvas->tool_controller->context().shiftHeld = e->modifiers().testFlag(Qt::ShiftModifier);
            canvas->tool_controller->on_move(p);
            canvas->tool_controller->on_release(p);
            drawing = false;
            viewport()->update();
        } else if (pin && tool.isEmpty()) e->ignore();
        else QGraphicsView::mouseReleaseEvent(e);
    }
};

/**
 * Linux 截图与标注预览窗口。
 * 负责管理屏幕授权的交互、单屏图片的画布生命周期、工具面板和导出操作。
 * 采集期间窗口临时隐藏；取消或失败会恢复原有画布，避免丢失未完成的选区。
 *
 * Linux capture and annotation preview. Owns the capture workflow, image canvas,
 * tool panels and export actions. The window is hidden while capturing and the
 * current canvas is restored unchanged if authorization is cancelled or fails.
 */
class CapturePreview : public LinuxAnnotationCanvas {
public:
    explicit CapturePreview(QImage image, LinuxSettings *settings = nullptr, LinuxPinWindow *pin = nullptr,
                            LinuxPinToolbar *host = nullptr, jietuba::Toolbar *annotationToolbar = nullptr)
        : LinuxAnnotationCanvas(pin), pinHost_(host), settings_(settings) {
        if (pin) setGeometry(pin->rect());
        else {
            setWindowTitle(QStringLiteral("截图吧 — Linux 画布预览"));
            resize(1000, 700);
            setMinimumSize(320, 240);
        }
        view_ = new PreviewView(this);
        view_->preferences = settings_;
        view_->setGeometry(rect());
        view_->pin = pin;
        view_->fullScreenCapture = pin != nullptr;
        if (pin) {
            view_->viewport()->setAutoFillBackground(false);
            view_->viewport()->setAttribute(Qt::WA_TranslucentBackground);
            view_->viewport()->setAttribute(Qt::WA_OpaquePaintEvent,false);
            view_->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
            toolbar_ = host->toolbar();
            setAttribute(Qt::WA_TransparentForMouseEvents);
            connect(host, &LinuxPinToolbar::panelPositionChanged, this, [this](bool below) {
                toolbarBelowSelection_ = below;
                showPanel(view_->tool);
            });
        } else {
            capture_ = new CaptureRouter(this);
            captureButton_ = new QPushButton(QStringLiteral("截图"), this);
            captureButton_->move(12, 12);
            captureButton_->setToolTip(QStringLiteral("截取屏幕；全局快捷键在设置中由桌面配置"));
            connect(captureButton_, &QPushButton::clicked, this, [this] { startCapture(); });
            connect(capture_, &CaptureRouter::frameReady, this, [this](const QImage &image) {
                // 区域返回值只有选区像素，不能交给旧整屏 setImage 流程重建坐标和撤销栈。
                // Area replies contain only a crop; keep the output-sized document and its undo history.
                if (view_->regionCapture) {
                    if (capturePending_) updateRegionFrame(regionRequested_, image);
                    return;
                }
                // 恢复窗口后再启动新选区；图像使用视频帧像素尺寸。
                // Restore the window with the captured frame in its native pixel dimensions.
                capturePending_ = false;
                captureButton_->setEnabled(true);
                setImage(image);
                show();
                activateWindow();
            });
            connect(capture_, &CaptureRouter::failed, this, [this](const QString &reason) {
                finishCaptureRequest();
                QMessageBox::warning(this, QStringLiteral("截图失败"), reason);
            });
            toolbar_ = annotationToolbar ? annotationToolbar : new jietuba::Toolbar(this);
            if (settings_ && !annotationToolbar) toolbar_->set_preview_layout_storage(
                [this] { return settings_->screenshotToolbarLayout(); },
                [this](const QJsonArray &entries) { return settings_->saveScreenshotToolbarLayout(entries); });
        }
        connect(toolbar_,&jietuba::Toolbar::preview_layout_changed,this,[this] { positionToolbar(); });
        connect(toolbar_,&jietuba::Toolbar::preview_layout_dialog_visibility_changed,this,[this](bool visible) {
            // 调整只影响按钮布局，保留选区、图形和撤销记录；编辑中的文字先正常提交。
            // Layout editing keeps the crop, annotations and undo history; commit active text normally.
            if (visible) { finishText(); hidePanels(); if (magnifier_) magnifier_->clear_cursor(); }
            else positionToolbar();
        });
        paintPanel_ = new jietuba::PaintSettingsPanel(this);
        paintPanel_->setObjectName(QStringLiteral("linuxPenPanel"));
        paintPanel_->set_highlighter_mode_visible(false);
        paintPanel_->hide();
        highlightPanel_ = new jietuba::PaintSettingsPanel(this);
        highlightPanel_->setObjectName(QStringLiteral("linuxHighlighterPanel"));
        highlightPanel_->set_highlighter_mode_visible(true);
        highlightPanel_->set_line_style_visible(false);
        highlightPanel_->set_size(15);
        highlightPanel_->set_color(QColor(QStringLiteral("#FFFF00")));
        highlightPanel_->set_opacity(255);
        spotlightPanel_ = new jietuba::PaintSettingsPanel(this);
        spotlightPanel_->setObjectName(QStringLiteral("linuxSpotlightPanel"));
        spotlightPanel_->set_highlighter_mode_visible(false);
        spotlightPanel_->set_line_style_visible(false);
        spotlightPanel_->set_size_visible(false);
        spotlightPanel_->set_color_visible(false);
        spotlightPanel_->set_opacity(qRound(jietuba::SpotlightCurtain::DEFAULT_DARKNESS * 255));
        shapePanel_ = new jietuba::ShapeSettingsPanel(this);
        arrowPanel_ = new jietuba::ArrowSettingsPanel(this);
        textPanel_ = new jietuba::TextSettingsPanel(this);
        mosaicPanel_ = new jietuba::MosaicSettingsPanel(this);
        numberPanel_ = new jietuba::NumberSettingsPanel(this);
        watermarkPanel_ = new jietuba::WatermarkSettingsPanel(this);
        watermarkPanel_->setObjectName(QStringLiteral("linuxWatermarkPanel"));
        if (settings_) { watermarkText_ = settings_->watermarkText(); watermarkStyle_ = jietuba::WatermarkStyle::from_values(settings_->watermarkStyle()); }
        watermarkPanel_->load_style(watermarkStyle_);
        watermarkPanel_->set_template(watermarkText_);
        view_->nextNumberChanged = [this](int value) { if (numberPanel_) numberPanel_->set_next_number(value); };
        // 让原版弹出框知道面板在工具栏的哪一侧。
        // Tell original popups which side of the toolbar the panel occupies.
        for (auto *panel : {static_cast<QWidget *>(paintPanel_), static_cast<QWidget *>(shapePanel_),
                            static_cast<QWidget *>(arrowPanel_), static_cast<QWidget *>(textPanel_), static_cast<QWidget *>(mosaicPanel_), static_cast<QWidget *>(numberPanel_), static_cast<QWidget *>(highlightPanel_), static_cast<QWidget *>(spotlightPanel_), static_cast<QWidget *>(watermarkPanel_)})
            panel->setProperty("owner_toolbar", QVariant::fromValue(static_cast<QWidget *>(toolbar_)));
        // 面板过宽时允许横向滚动，保证小窗口或屏幕边缘也能访问全部控件。
        // Allow horizontal scrolling when an original panel is wider than the host.
        for (auto *panel : {static_cast<QWidget *>(paintPanel_), static_cast<QWidget *>(shapePanel_),
                            static_cast<QWidget *>(arrowPanel_), static_cast<QWidget *>(textPanel_), static_cast<QWidget *>(mosaicPanel_), static_cast<QWidget *>(numberPanel_), static_cast<QWidget *>(highlightPanel_), static_cast<QWidget *>(spotlightPanel_), static_cast<QWidget *>(watermarkPanel_)}) {
            auto *container = new QScrollArea(toolbar_->parentWidget());
            container->setFrameShape(QFrame::NoFrame);
            container->setWidgetResizable(false);
            container->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            container->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            container->setStyleSheet("QScrollArea { background: transparent; border: none; }");
            container->setWidget(panel);
            panel->show();
            container->hide();
            panelContainers_.insert(panel, container);
        }
        // 面板默认线宽与画布工具上下文保持一致。
        // Match panel defaults to the canvas tool context.
        paintPanel_->set_size(3);
        shapePanel_->set_size(3);
        arrowPanel_->set_size(3);
        numberPanel_->set_size(16); numberPanel_->set_color(Qt::red); numberPanel_->set_opacity(255);
        initializeToolDefaults();
        // 初始文字样式来自原版面板；设置值作用于后续新建文字。
        // Seed new text from the original panel's default style.
        view_->newTextStyle.font = textPanel_->current_font;
        view_->newTextStyle.textColor = textPanel_->current_color;
        view_->newTextStyle.hasBackground = textPanel_->background_enabled;
        view_->newTextStyle.hasOutline = textPanel_->outline_enabled;
        view_->newTextStyle.hasShadow = textPanel_->shadow_enabled;
        view_->newTextStyle.background = textPanel_->background_color;
        view_->newTextStyle.background.setAlpha(textPanel_->background_opacity);
        view_->newTextStyle.outline = textPanel_->outline_color;
        view_->newTextStyle.outlineWidth = textPanel_->outline_width;
        view_->newTextStyle.shadow = textPanel_->shadow_color;
        if (!pin && !annotationToolbar) {
            for (auto *button : toolbar_->findChildren<QAbstractButton *>()) button->setEnabled(false);
            for (auto *button : {toolbar_->pen_btn, toolbar_->highlighter_btn, toolbar_->rect_btn, toolbar_->ellipse_btn, toolbar_->arrow_btn, toolbar_->text_btn,
                                 toolbar_->gif_btn, toolbar_->long_screenshot_btn, toolbar_->mosaic_btn, toolbar_->number_btn, toolbar_->eraser_btn, toolbar_->spotlight_btn, toolbar_->watermark_btn, toolbar_->pin_btn, toolbar_->undo_btn, toolbar_->redo_btn, toolbar_->save_btn, toolbar_->quick_save_btn, toolbar_->text_recognize_btn, toolbar_->scan_code_btn, toolbar_->screenshot_translate_btn, toolbar_->confirm_btn, toolbar_->cancel_btn}) button->setEnabled(true);
            toolbar_->more_btn->setEnabled(true);
        }
        connect(toolbar_, &jietuba::Toolbar::preview_more_visibility_changed, this, [this](bool visible) {
            // 更多菜单和设置面板共用选区外空间，交替显示以免互相遮挡。
            // More and settings share outside space; show only one at a time.
            if (visible) hidePanels();
            else showPanel(view_->tool);
        });
        for (const auto &entry : {std::pair{toolbar_->pen_btn, "pen"}, {toolbar_->highlighter_btn, "highlighter"}, {toolbar_->rect_btn, "rect"}, {toolbar_->ellipse_btn, "ellipse"},
                                  {toolbar_->arrow_btn, "arrow"}, {toolbar_->text_btn, "text"}, {toolbar_->mosaic_btn, "mosaic"}, {toolbar_->number_btn, "number"}, {toolbar_->eraser_btn, "eraser"}, {toolbar_->spotlight_btn, "spotlight"}, {toolbar_->watermark_btn, "watermark"}}) {
            connect(entry.first, &QPushButton::clicked, this, [this, id = QString::fromLatin1(entry.second)] {
                if (!scene_) return;
                finishText();
                if (view_->editor) view_->editor->stop_edit();
                view_->viewport()->update();
                view_->tool = view_->tool == id ? QString() : id;
                scene_->activate_tool(view_->tool.isEmpty() ? QStringLiteral("cursor") : view_->tool);
                // 从独立快照恢复默认值；选中过的旧图形不能污染下一次新建样式。
                // Restore independent defaults rather than the last selected item's panel values.
                applyToolDefault(view_->tool);
                for (auto it = toolbar_->tool_buttons.begin(); it != toolbar_->tool_buttons.end(); ++it) it.value()->setChecked(it.key() == view_->tool);
                view_->viewport()->setFocus();
                syncDefaultPanel(view_->tool);
                showPanel(view_->tool);
                if (view_->tool == "watermark") {
                    if (!jietuba::WatermarkItem::find(scene_) && !watermarkText_.isEmpty()) addWatermark(watermarkText_);
                    if (watermarkText_.isEmpty()) editWatermark();
                }
                if (recordingMode_) {setAttribute(Qt::WA_TransparentForMouseEvents,view_->tool.isEmpty()); if(recordingChanged) recordingChanged();}
            });
        }
        // 面板信号既更新新图形默认样式，也把已有图形的改动记录到撤销栈。
        // Panel signals update future tool defaults and record edits to selected items.
        auto connectPenPanel = [this](jietuba::BaseSettingsPanel *panel) {
            connect(panel, &jietuba::BaseSettingsPanel::color_changed, this, [this](const QColor &v) { changePen([&](QPen &p) { auto c = v; c.setAlpha(p.color().alpha()); p.setColor(c); }, v, {}, {}); });
            connect(panel, &jietuba::BaseSettingsPanel::size_changed, this, [this](int v) { changePen([&](QPen &p) { p.setWidthF(v); }, {}, double(v), {}); });
            connect(panel, &jietuba::BaseSettingsPanel::opacity_changed, this, [this](int v) { changePen([&](QPen &p) { auto c = p.color(); c.setAlpha(v); p.setColor(c); }, {}, {}, v / 255.0); });
        };
        connectPenPanel(paintPanel_);
        connectPenPanel(highlightPanel_);
        connectPenPanel(shapePanel_);
        connectPenPanel(numberPanel_);
        connect(arrowPanel_, &jietuba::ArrowSettingsPanel::color_changed, this, [this](const QColor &v) { changePen([&](QPen &p) { auto c = v; c.setAlpha(p.color().alpha()); p.setColor(c); }, v, {}, {}); });
        connect(arrowPanel_, &jietuba::ArrowSettingsPanel::size_changed, this, [this](int v) { changePen([&](QPen &p) { p.setWidthF(v); }, {}, double(v), {}); });
        connect(arrowPanel_, &jietuba::ArrowSettingsPanel::opacity_changed, this, [this](int v) { changePen([&](QPen &p) { auto c = p.color(); c.setAlpha(v); p.setColor(c); }, {}, {}, v / 255.0); });
        auto lineStyle = [this](const QString &style) {
            if (!scene_) return;
            if (view_->tool == "pen") if (auto *t = scene_->tool_controller->get_tool_as<jietuba::PenTool>("pen")) t->line_style = style;
            if (view_->tool == "rect") if (auto *t = scene_->tool_controller->get_tool_as<jietuba::RectTool>("rect")) t->line_style = style;
            if (view_->tool == "ellipse") if (auto *t = scene_->tool_controller->get_tool_as<jietuba::EllipseTool>("ellipse")) t->line_style = style;
            changeSelected([&](PreviewStyle &s) { jietuba::apply_line_style(s.pen, style); });
        };
        connect(paintPanel_, &jietuba::PaintSettingsPanel::line_style_changed, this, lineStyle);
        connect(highlightPanel_, &jietuba::PaintSettingsPanel::highlighter_mode_changed, this, [this](const QString &mode) {
            if (scene_) if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::HighlighterTool>("highlighter")) tool->linux_mode = mode;
        });
        connect(shapePanel_, &jietuba::ShapeSettingsPanel::line_style_changed, this, lineStyle);
        connect(arrowPanel_, &jietuba::ArrowSettingsPanel::arrow_style_changed, this, [this](const QString &v) {
            if (scene_) if (auto *t = scene_->tool_controller->get_tool_as<jietuba::ArrowTool>("arrow")) t->arrow_style = v;
            changeSelected([&](PreviewStyle &s) { s.arrowStyle = v; });
        });
        connect(textPanel_, &jietuba::TextSettingsPanel::font_changed, this, [this](const QFont &v) { changeText([&](PreviewStyle &s) { s.font = v; }); });
        connect(textPanel_, &jietuba::TextSettingsPanel::color_changed, this, [this](const QColor &v) { changeText([&](PreviewStyle &s) { s.textColor = v; }); });
        connect(textPanel_, &jietuba::TextSettingsPanel::background_changed, this, [this](bool enabled, const QColor &c, int opacity) { changeText([&](PreviewStyle &s) { s.hasBackground = enabled; s.background = c; s.background.setAlpha(opacity); }); });
        connect(textPanel_, &jietuba::TextSettingsPanel::outline_changed, this, [this](bool enabled, const QColor &c, double width) { changeText([&](PreviewStyle &s) { s.hasOutline = enabled; s.outline = c; s.outlineWidth = width; }); });
        connect(textPanel_, &jietuba::TextSettingsPanel::shadow_changed, this, [this](bool enabled, const QColor &c) { changeText([&](PreviewStyle &s) { s.hasShadow = enabled; s.shadow = c; }); });
        // 默认设置作用于新图形；有选中图形时，样式同时进入撤销栈。
        // Panel values seed new strokes; changes to a selected item are undoable.
        connect(mosaicPanel_, &jietuba::MosaicSettingsPanel::draw_mode_changed, this, [this](const QString &mode) {
            if (scene_) if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::MosaicTool>("mosaic")) tool->linux_mode = mode;
        });
        connect(mosaicPanel_, &jietuba::MosaicSettingsPanel::style_changed, this, [this](const QString &style) {
            if (scene_) if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::MosaicTool>("mosaic")) tool->linux_style = style;
            if (view_->editor && dynamic_cast<jietuba::MosaicItem *>(view_->editor->active_layer()))
                changeSelected([&](PreviewStyle &s) { s.mosaicKind = jietuba::MosaicTool::kind_of(style); });
        });
        connect(mosaicPanel_, &jietuba::MosaicSettingsPanel::block_size_changed, this, [this](int size) {
            if (scene_) if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::MosaicTool>("mosaic")) tool->linux_block_size = size;
            if (view_->editor && dynamic_cast<jietuba::MosaicItem *>(view_->editor->active_layer())) changeSelected([&](PreviewStyle &s) { s.mosaicBlock = size; });
        });
        connect(mosaicPanel_, &jietuba::MosaicSettingsPanel::size_changed, this, [this](int size) {
            if (scene_ && view_->tool == "mosaic") scene_->update_style({}, double(size), {});
            if (view_->editor && dynamic_cast<jietuba::MosaicItem *>(view_->editor->active_layer())) changeSelected([&](PreviewStyle &s) { s.mosaicWidth = size; });
        });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::next_number_changed, this, [this](int value) {
            if (scene_) numberPanel_->set_next_number(jietuba::NumberTool::set_next_number(scene_, value));
        });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::style_changed, this, [this](const QString &style) {
            if (scene_) if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::NumberTool>("number")) tool->linux_style = style;
            if (view_->editor && dynamic_cast<jietuba::NumberItem *>(view_->editor->active_layer())) changeSelected([&](PreviewStyle &s) { s.numberStyle = style; });
        });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::label_font_changed, this, [this](const QString &v) { changeNumberExtra("label_font", v); });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::label_size_changed, this, [this](int v) { changeNumberExtra("label_size", v); });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::label_outline_changed, this, [this](bool v) { changeNumberExtra("label_outline", v); });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::label_guide_changed, this, [this](bool v) { changeNumberExtra("label_guide", v); });
        connect(numberPanel_, &jietuba::NumberSettingsPanel::arrow_style_changed, this, [this](const QString &v) { changeNumberExtra("arrow_style", v); });
        connect(spotlightPanel_, &jietuba::PaintSettingsPanel::opacity_changed, this, [this](int value) {
            if (!scene_) return;
            const qreal opacity = value / 255.0;
            if (view_->tool == "spotlight") scene_->update_style({}, {}, opacity);
            auto *curtain = jietuba::SpotlightCurtain::find(scene_);
            if (curtain && !qFuzzyCompare(curtain->opacity(), opacity)) scene_->undo_stack->push(new SpotlightOpacityCommand(scene_, curtain->opacity(), opacity));
        });
        connect(watermarkPanel_, &jietuba::WatermarkSettingsPanel::text_edit_requested, this, [this] { editWatermark(); });
        connect(watermarkPanel_, &jietuba::WatermarkSettingsPanel::text_cleared, this, [this] { clearWatermark(); });
        connect(watermarkPanel_, &jietuba::WatermarkSettingsPanel::style_changed, this, [this](const QString &key, const QVariant &value) { changeWatermarkStyle(key, value); });
        connectDefaultPersistence();
        view_->beginNewDrawing=[this] { applyToolDefault(view_->tool); };
        view_->precisionKey=[this](QKeyEvent *event) { return handlePrecisionKey(event); };
        view_->precisionWheel=[this](QWheelEvent *event) {
            // 沿用原版：仅放大镜正在显示时滚轮调整，每格 0.25，绘图/文本输入不抢滚轮。
            // Match Windows: wheel zoom only while magnifying, in 0.25 steps, leaving drawing/text input alone.
            if (!magnifier_ || !magnifier_->should_render() || !event->angleDelta().y()) return false;
            magnifier_->adjust_zoom(event->angleDelta().y()>0 ? 1 : -1); event->accept(); return true;
        };
        if (!pin) new LinuxShortcutRouter(this, settings_, "capture", [this](const QString &action) {
            if (recordingMode_ && !QStringList{"cursor","pen","highlighter","number","rect","ellipse","arrow","text","eraser","undo","redo","delete"}.contains(action)) return false;
            return invokeShortcut(action);
        }, [this] {
            return toolbar_->preview_layout_dialog_open() || !scene_ || view_->drawing || view_->moving || scene_->selection_model->is_dragging()
                || (view_->editor && view_->editor->dragging_handle) || editingCanvasText();
        }, [this](QKeyEvent *event) { return handleMagnifierShift(event); });
        view_->itemSelected = [this](QGraphicsItem *item) { selectItem(item); };
        connect(toolbar_, &jietuba::Toolbar::undo_clicked, this, [this] { finishText(); if (scene_) scene_->undo_stack->QUndoStack::undo(); });
        connect(toolbar_, &jietuba::Toolbar::redo_clicked, this, [this] { finishText(); if (scene_) scene_->undo_stack->QUndoStack::redo(); });
        if (!pin) {
            connect(toolbar_, &jietuba::Toolbar::cancel_clicked, this, [this] { resetSelection(); });
            connect(toolbar_, &jietuba::Toolbar::save_clicked, this, [this] { save(); });
            connect(toolbar_, &jietuba::Toolbar::quick_save_clicked, this, [this] { quickSave(); });
            connect(toolbar_, &jietuba::Toolbar::text_recognize_clicked, this, [this] { recognizeText(); });
            connect(toolbar_, &jietuba::Toolbar::scan_code_clicked, this, [this] { scanCode(); });
            connect(toolbar_, &jietuba::Toolbar::screenshot_translate_clicked, this, [this] { translateImage(); });
            connect(toolbar_, &jietuba::Toolbar::confirm_clicked, this, [this] { copy(); });
            connect(toolbar_, &jietuba::Toolbar::pin_clicked, this, [this] { this->pin(); });
            connect(toolbar_, &jietuba::Toolbar::long_screenshot_clicked, this, [this] { startLongCapture(); });
            toolbar_->gif_btn->setToolTip(QStringLiteral("录制选区为 GIF 动画"));
            connect(toolbar_, &jietuba::Toolbar::gif_record_clicked, this, [this] { startGifCapture(); });
        }
        view_->selectionChanged = [this] {
            if (view_->regionCapture && scene_ && scene_->selection_model->is_dragging()) {
                scene_->background->hide();
                view_->viewport()->update();
            }
            if (!pinHost_ && settings_ && scene_ && scene_->selection_model->is_confirmed()
                && !scene_->selection_model->is_dragging() && !scene_->selection_model->is_empty())
                settings_->setLastRegion(scene_->selection_model->rect().toAlignedRect(), baseImage_.size(), screen() ? screen()->name() : QString());
            positionToolbar();
        };
        toolbar_->hide();
        if (image.isNull()) { image = QImage(1000, 700, QImage::Format_RGB32); image.fill(QColor("#eeeeee")); }
        setImage(image);
    }
    ~CapturePreview() override {
        // 宿主可能先销毁兄弟面板。撤销栈 clear 会发 indexChanged，销毁前必须断开 UI 回写。
        // A host may destroy sibling panels first. Undo-stack destruction emits indexChanged,
        // so disconnect UI callbacks before committing text and releasing the scene.
        recordingChanged={};
        if(scene_) {disconnect(scene_,nullptr,this,nullptr); disconnect(scene_->undo_stack,nullptr,this,nullptr);}
        finishText();
        delete magnifier_; magnifier_=nullptr;
        view_->setCanvas(nullptr);
        view_->setScene(nullptr);
        delete scene_;
    }
    // 控制窗口保留原预览入口，全屏窗口在一次截图结束时统一通知会话管理器。
    // The launcher delegates capture; overlay windows report completion to their session owner.
    void setCaptureHandler(std::function<void()> handler) { captureHandler_ = std::move(handler); }
    void setPinHandler(std::function<void(const QImage &, QScreen *, const QRectF &)> handler) { pinHandler_ = std::move(handler); }
    void runOnSelection(const QString &action) {
        if (action != "ocr" && action != "translate") return;
        // 只在用户完成有效框选时执行，视口布局和窗口大小变化不会触发识别或翻译。
        // Only a confirmed selection triggers the action, never a resize or layout notification.
        connect(scene_, &jietuba::CanvasScene::selectionConfirmed, this, [this, action] {
            if (scene_->selection_model->is_empty()) return;
            if (action == "ocr") recognizeText();
            else translateImage();
        });
    }
    void configureRegionCapture(QScreen *output, const QRect &logicalRegion, const QImage &image) {
        regionOutput_ = output;
        regionGeometry_ = output->geometry();
        regionRequested_ = logicalRegion;
        view_->regionCapture = true;
        setAttribute(Qt::WA_TranslucentBackground);
        view_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        view_->setBackgroundBrush(Qt::NoBrush);
        view_->viewport()->setAutoFillBackground(false);
        view_->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        view_->selectionFinished = [this] { requestRegionFrame(); };
        updateRegionFrame(logicalRegion, image, true);
    }
    void updateRegionFrame(const QRect &logicalRegion, const QImage &image, bool initial = false) {
        regionRequested_ = logicalRegion;
        const qreal scale = image.devicePixelRatio();
        const QSize pixels(qRound(regionGeometry_.width()*scale), qRound(regionGeometry_.height()*scale));
        const QRect mapped = LinuxRegionSelector::nativeRegion(regionRequested_, regionGeometry_.size(), pixels);
        // 区域接口按区域宽高取整，可能与整屏坐标外扩取整相差一像素；以真实采集尺寸为准。
        // Native area rounding can differ by one pixel from output-edge rounding. Preserve the actual crop size.
        const QRect native = QRect(mapped.topLeft(),image.size()).intersected(QRect(QPoint(),pixels));
        // 只有透明画布使用全屏坐标；真正采集到的像素只有选区，选区外不保留旧画面。
        // The canvas keeps output coordinates, but only the requested crop contains captured pixels.
        if ((!initial && pixels!=baseImage_.size()) || image.isNull()) {
            capturePending_=false; if (finished_) finished_(); return;
        }
        QImage canvas(pixels,QImage::Format_ARGB32_Premultiplied);
        if (canvas.isNull()) { capturePending_=false; if (finished_) finished_(); return; }
        canvas.fill(Qt::transparent);
        QImage crop = image; crop.setDevicePixelRatio(1);
        // 不缩放选区像素：首帧和连续帧必须逐像素一致，否则细字会错行、拼接出现拖影。
        // Never resample the crop: first/continuous frames need identical native pixels for text matching.
        { QPainter painter(&canvas); painter.drawImage(native.topLeft(), crop); }
        if (initial) setImage(canvas);
        else {
            // 重采集只替换底图，图形对象、样式与撤销记录继续归属于同一个画布。
            // Replace only the background; preserve annotation objects, styles and undo history.
            baseImage_ = canvas;
            scene_->background->update_image(canvas);
            scene_->background->show();
        }
        regionFrame_ = native;
        capturePending_ = false;
        setInitialRegion(native);
        if (initial && settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::AutoWatermark)) addWatermark(watermarkText_);
        view_->viewport()->update();
        if (!initial) showPreparedOverlay();
    }
    void requestRegionFrame() {
        if (capturePending_) return;
        if (!regionOutput_ || regionOutput_->geometry()!=regionGeometry_) { if (finished_) finished_(); return; }
        const auto logical = QRectF(scene_->selection_model->rect().x()*regionGeometry_.width()/baseImage_.width(),
            scene_->selection_model->rect().y()*regionGeometry_.height()/baseImage_.height(),
            scene_->selection_model->rect().width()*regionGeometry_.width()/baseImage_.width(),
            scene_->selection_model->rect().height()*regionGeometry_.height()/baseImage_.height());
        const auto next = logical.toAlignedRect().intersected(QRect(QPoint(),regionGeometry_.size()));
        if (next.isEmpty()) {
            scene_->background->show(); setInitialRegion(regionFrame_); return;
        }
        regionRequested_ = next;
        finishText();
        if (view_->editor) view_->editor->stop_edit();
        hidePanels(); if (magnifier_) magnifier_->clear_cursor();
        capturePending_ = true;
        withdrawOverlay();
        // 编辑层及选区边框先撤下，再调用区域接口，避免截图把自己录进去。
        // Unmap the editor and its border before invoking native area capture.
        QTimer::singleShot(60,this,[this] {
            // 测试注入/取消已经完成的请求不可再次发出，避免迟到的回调覆盖新状态。
            // A completed or cancelled request must not be issued by its delayed callback.
            if (!capturePending_) return;
            if (!regionOutput_ || regionOutput_->geometry()!=regionGeometry_) {
                capturePending_=false; if (finished_) finished_(); return;
            }
            capture_->captureRegion(regionOutput_->name(),regionRequested_.translated(regionGeometry_.topLeft()),
                                    regionGeometry_,CallerWindowPolicy::Include, settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::Cursor));
        });
    }
    void setInitialRegion(const QRect &nativeRegion) {
        if (!scene_ || nativeRegion.isEmpty()) return;
        // 透明框选阶段已经完成选区；编辑器首帧直接显示该选区，不要求用户再框一次。
        // The live selector already chose the crop. Seed it before mapping the editor's first frame.
        scene_->preset_selection(QRectF(nativeRegion));
        if (settings_) settings_->setLastRegion(nativeRegion, baseImage_.size(), screen() ? screen()->name() : QString());
        positionToolbar();
        view_->viewport()->update();
    }
    void setOverlayMode(std::function<void()> finished) {
        overlay_ = true;
        finished_ = std::move(finished);
        view_->fullScreenCapture = true;
        captureButton_->hide();
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
    void setLongCaptureHandler(std::function<void(QScreen *, const QSize &, const QRect &, const QImage &, const QRect &)> handler) {
        longCaptureHandler_ = std::move(handler);
    }
    void setGifCaptureHandler(std::function<void(QScreen *, const QSize &, const QRect &,const QRect &)> handler) { gifCaptureHandler_ = std::move(handler); }
    // 全局截图键可能与本地快速保存相同，门户拦截时由会话把这个动作转回当前画布。
    // If the portal intercepts a key shared with a local action, dispatch it back to the active canvas.
    bool triggerLocalBinding(const QString &binding) {
        if (toolbar_->preview_layout_dialog_open() || editingCanvasText() || view_->drawing || view_->moving || scene_->selection_model->is_dragging()) return false;
        const auto sequence=QKeySequence::fromString(binding,QKeySequence::NativeText);
        if (sequence.isEmpty() || sequence.count()!=1) return false;
        for (const auto &entry:linuxShortcutActions()) if (entry.context=="capture") {
            const auto key=settings_ ? settings_->shortcut(entry.id) : entry.defaultKey;
            if (!key.isEmpty() && sequence==QKeySequence::fromString(key,QKeySequence::PortableText)) return invokeShortcut(entry.id);
        }
        return false;
    }
    QRect captureBounds() const { return regionRequested_; }
    void setSaveDialogHandler(std::function<void(bool)> handler) { saveDialogHandler_ = std::move(handler); }
    void setNativeOverlayLevel(bool upper) {
#ifdef JIETUBA_HAS_LAYER_SHELL
        if (layerOverlay_) {
            auto *layer = LayerShellQt::Window::get(windowHandle());
            const auto wanted = upper ? LayerShellQt::Window::LayerOverlay : LayerShellQt::Window::LayerTop;
            if (layer->layer() != wanted) { layer->setLayer(wanted); update(); }
        }
#else
        Q_UNUSED(upper);
#endif
    }
    void setEditorInput(bool active) {
        // 禁用整张截图的控件树，包括作为工具栏兄弟控件的更多菜单和样式面板。
        // Disable this screenshot's widget tree, including sibling popups and settings panels.
        setEnabled(active);
        LinuxWayland::setCaptureInput(this, layerOverlay_, active);
        update(); // 提交输入区域变动，即使画面本身没改变。 / Commit the input region even if the pixels haven't changed.
        if (active && isVisible()) view_->setFocus(Qt::OtherFocusReason);
    }
    QRectF outputCrop() const {
        QRectF crop = view_->viewportTransform().mapRect(scene_->selection_model->rect());
        return crop.translated(view_->viewport()->mapTo(this, QPoint()));
    }
    std::function<void(QImage &,const QRectF &)> compositeChildren;
    bool canSuspend() const {
        return isVisible() && !capturePending_ && !toolbar_->preview_layout_dialog_open()
            && !view_->drawing && !view_->moving && !(view_->editor && view_->editor->dragging_handle);
    }
    bool parkForCapture() {
        const bool dragging = scene_->selection_model->is_dragging();
        if (dragging) {
            auto *item = scene_->selection_item;
            if (scene_->mouseGrabberItem() == item) item->ungrabMouse();
            item->active_handle = jietuba::SelectionItem::HANDLE_NONE;
            scene_->selection_model->stop_dragging();
            if (scene_->selection_model->is_empty()) setInitialRegion(regionFrame_);
            else if (scene_->selection_model->is_active()) scene_->confirm_selection();
            refreshAfterChild_ = true;
            positionToolbar();
        }
        if (auto *grab = QWidget::mouseGrabber(); grab && grab->window() == this) grab->releaseMouse();
        setEditorInput(false);
        repaint();
        return dragging;
    }
    void resumeAfterCapture() {
        setEditorInput(true);
        if (std::exchange(refreshAfterChild_, false)) requestRegionFrame();
    }
    bool outputStillValid() const { return regionOutput_ && regionOutput_->geometry() == regionGeometry_; }
    // 显示前完成输出绑定、窗口角色及画布尺寸设置，禁止先显示默认尺寸再切全屏。
    // Prepare output, surface role and canvas geometry before mapping the first frame.
    void prepareOverlayOnScreen(QScreen *screen) {
        resize(screen->geometry().size());
        winId();
        windowHandle()->setScreen(screen);
#ifdef JIETUBA_HAS_LAYER_SHELL
        if (LinuxWayland::supportsLayerShell()) {
            if (auto *layer = LayerShellQt::Window::get(windowHandle())) {
                // Qt::Tool 不影响 layer-shell 类型，KWin 必须用已识别的 utility scope。
                // Layer-shell type ignores Qt::Tool; use KWin's recognized utility scope.
                layer->setScope(QStringLiteral("utility"));
                layer->setScreen(screen);
                layer->setLayer(LayerShellQt::Window::LayerOverlay);
                layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom |
                                  LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
                layer->setExclusiveZone(-1); // 覆盖面板，不挤压桌面工作区 / Cover panels without reserving space.
                layer->setDesiredSize(screen->geometry().size());
                layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
                layerOverlay_ = true;
            }
        }
#endif
        // 隐藏 QWidget 的 resize 事件可能延后，显式同步首帧画布，避免默认大小的闪帧。
        // Hidden widgets may defer resize events; explicitly fit the first-frame canvas now.
        view_->setGeometry(rect());
        view_->fit();
        ensurePolished();
    }
    void showPreparedOverlay() {
        view_->withdrawing=false;
        positionToolbar(); // 恢复保存取消时隐藏的工具栏与当前样式面板 / Restore controls after a cancelled save.
        // layer-shell 本身覆盖整个输出，无需普通窗口的全屏状态切换或打开动画。
        // Layer surfaces already fill the output and do not need an xdg fullscreen transition.
        qInfo().noquote() << QStringLiteral("选区窗口:")
                          << (layerOverlay_ ? QStringLiteral("Wayland layer-shell 覆盖层") : QStringLiteral("普通全屏回退"));
        if (layerOverlay_) show();
        else showFullScreen();
        view_->setFocus(Qt::OtherFocusReason);
        view_->viewport()->update();
    }
    void withdrawOverlay() {
        if (!view_->regionCapture) { hide(); return; }
        // hide 本身不保证旧缓冲离开合成器；先移除所有装饰并同步提交透明帧。
        // hide alone does not invalidate cached compositor textures. Clear decorations and flush alpha first.
        toolbar_->hide();
        if (auto *more=toolbar_->more_popup()) more->hide(); // 更多菜单是工具栏的兄弟控件 / More is a sibling of the bar.
        hidePanels(); if (magnifier_) magnifier_->hide();
        view_->withdrawing=true;
        view_->viewport()->repaint();
        repaint();
        hide();
    }
    void configureRecording(QWidget *surface,QWidget *controls) {
        recordingMode_=true; recordingControls_=controls;
        setMinimumSize(1,1); setParent(surface); setObjectName(QStringLiteral("linuxRecordingCanvas"));
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        view_->recording=true; view_->regionCapture=true; view_->fullScreenCapture=true;
        view_->setBackgroundBrush(Qt::NoBrush); view_->viewport()->setAutoFillBackground(false);
        view_->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        toolbar_->hide(); captureButton_->hide();
        delete magnifier_; magnifier_=nullptr; view_->pointerChanged={};
        scene_->background->hide(); scene_->preset_selection(scene_->sceneRect()); scene_->selection_item->hide();
        for(auto it=panelContainers_.begin();it!=panelContainers_.end();++it) {
            it.value()->setParent(surface); it.value()->hide();
            it.key()->setProperty("owner_toolbar",QVariant::fromValue(controls));
        }
        setAttribute(Qt::WA_TransparentForMouseEvents);
        view_->fit();
    }
    void activateRecordingTool(const QString &tool) override {
        if (!recordingMode_) return;
        if (tool.isEmpty() || tool=="cursor") endPinEditing();
        else if (auto *button=toolbar_->tool_buttons.value(tool); button && view_->tool!=tool) button->click();
        setAttribute(Qt::WA_TransparentForMouseEvents,view_->tool.isEmpty());
        if(recordingChanged) recordingChanged();
    }
    QString recordingTool() const override { return view_->tool; }
    QUndoStack *recordingUndoStack() const override { return scene_->undo_stack; }
    QImage recordingImage() override {
        // 不调用 result()/finishText：逐帧合成不能中断正在输入的文字；控制点仅由视图绘制，永不导出。
        // Do not finish text each frame. View-only edit handles are excluded from this scene export.
        bool hasAnnotations=false;
        for(auto *item:scene_->items()) if(item!=scene_->background && item!=scene_->selection_item && item->isVisible()) { hasAnnotations=true; break; }
        return hasAnnotations ? jietuba::ExportService(scene_).export_image(scene_->sceneRect()) : QImage();
    }
    void layoutRecordingPanels() override {
        if(!recordingMode_ || !recordingControls_) return;
        toolbarBelowSelection_=recordingControls_->y()>=geometry().bottom();
        showPanel(view_->tool);
    }
    void setAnnotationAppearance(bool whiteboard,std::optional<QPointF> pointer) override {
        view_->annotationWhiteboard=whiteboard;
        view_->annotationPointer=pointer ? std::optional<QPointF>(view_->viewport()->mapFrom(this,*pointer)) : std::nullopt;
        view_->viewport()->update();
    }
    void clearAnnotationsUndoable() override {
        // 先提交活动文字/控制点；仅删除根图层，复合序号整体进入一条撤销宏。
        // Commit active editing, then remove root annotations in one undo macro (including compound numbers).
        activateRecordingTool("cursor");
        QList<jietuba::TrackedItem *> roots;
        for(auto *item:scene_->items()) if(!item->parentItem() && item!=scene_->background && item!=scene_->selection_item)
            if(auto *tracked=dynamic_cast<jietuba::TrackedItem *>(item)) roots.append(tracked);
        if(roots.isEmpty()) return;
        scene_->undo_stack->beginMacro(QStringLiteral("清空屏幕标注"));
        for(auto *item:roots) if(dynamic_cast<jietuba::NumberItem *>(item)) scene_->undo_stack->push(new jietuba::RemoveNumberCommand(scene_,jietuba::ItemReference(item)));
            else scene_->undo_stack->push(new jietuba::RemoveItemCommand(scene_,jietuba::ItemReference(item)));
        scene_->undo_stack->endMacro(); view_->viewport()->update();
    }
    void clearRecording() override {
        if(!recordingMode_) return;
        activateRecordingTool("cursor"); scene_->undo_stack->clear();
        QList<QGraphicsItem *> roots;
        for(auto *item:scene_->items()) if(!item->parentItem() && item!=scene_->background && item!=scene_->selection_item) roots.append(item);
        qDeleteAll(roots);
        view_->viewport()->update(); if(recordingChanged) recordingChanged();
    }

protected:
    void resizeEvent(QResizeEvent *e) override { QWidget::resizeEvent(e); if (view_) view_->setGeometry(rect()); }
    void keyPressEvent(QKeyEvent *e) override {
        if (recordingMode_) {
            if(e->key()==Qt::Key_Escape) activateRecordingTool("cursor");
            else QWidget::keyPressEvent(e);
            return;
        }
        // 子面板未消费的按键可能冒泡到宿主；此时禁止撤销/打开图片，Esc 只关闭调整。
        // Guard bubbled keys while editing: no canvas undo/open, and Escape dismisses only the editor.
        if (toolbar_->preview_layout_dialog_open()) {
            if (e->key() == Qt::Key_Escape) toolbar_->cancel_preview_layout_dialog();
            e->accept();
            return;
        }
        if (pinHost_) { e->ignore(); return; }
        if (e->key() == Qt::Key_Escape) { resetSelection(); return; }
        if (handlePrecisionKey(e)) return;
        if ((e->key()==Qt::Key_Return || e->key()==Qt::Key_Enter) && e->modifiers()==Qt::NoModifier && !editingCanvasText()
            && scene_->selection_model->is_confirmed() && !scene_->selection_model->is_empty()) { copy(); return; }
        if (e->key() == Qt::Key_O && e->modifiers() == Qt::ControlModifier) {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开图片"));
            if (!path.isEmpty()) { QImageReader reader(path); reader.setAutoTransform(true); const auto image = reader.read();
                if (image.isNull()) QMessageBox::warning(this, QStringLiteral("打开失败"), reader.errorString()); else setImage(image); }
            return;
        }
        QWidget::keyPressEvent(e);
    }
private:
    // 面板只是当前显示状态；toolDefaults_ 才是新建图形的独立、可持久化默认值。
    // Panels display current state; toolDefaults_ independently owns persistent new-item defaults.
    jietuba::BaseSettingsPanel *basePanel(const QString &tool) const {
        if (tool=="pen") return paintPanel_;
        if (tool=="highlighter") return highlightPanel_;
        if (tool=="rect" || tool=="ellipse") return shapePanel_;
        if (tool=="number") return numberPanel_;
        if (tool=="spotlight") return spotlightPanel_;
        return nullptr;
    }
    QVariantMap panelStyle(const QString &tool) const {
        QVariantMap state;
        if (const auto *panel=basePanel(tool)) {
            state={{"color",panel->current_color.name(QColor::HexArgb)},{"width",panel->current_size},{"opacity",panel->current_opacity}};
            if (tool=="pen") state["line"]=paintPanel_->line_style();
            if (tool=="rect" || tool=="ellipse") state["line"]=shapePanel_->line_style();
            if (tool=="highlighter") state["mode"]=highlightPanel_->current_highlighter_mode;
            if (tool=="number") state["style"]=numberPanel_->current_style();
        } else if (tool=="arrow") state={{"color",arrowPanel_->current_color.name(QColor::HexArgb)},
            {"width",arrowPanel_->current_size},{"opacity",qRound(arrowPanel_->opacity_spin->value()*2.55)},{"style",arrowPanel_->arrow_style()}};
        else if (tool=="mosaic") state={{"width",mosaicPanel_->current_size},{"mode",mosaicPanel_->draw_mode()},
            {"style",mosaicPanel_->style()},{"block",mosaicPanel_->block_size()}};
        else if (tool=="text") state={{"color",textPanel_->current_color.name(QColor::HexArgb)},{"font",textPanel_->current_font.toString()},
            {"background",textPanel_->background_enabled},{"backgroundColor",textPanel_->background_color.name(QColor::HexArgb)},
            {"backgroundOpacity",textPanel_->background_opacity},{"outline",textPanel_->outline_enabled},
            {"outlineColor",textPanel_->outline_color.name(QColor::HexArgb)},{"outlineWidth",textPanel_->outline_width},
            {"shadow",textPanel_->shadow_enabled},{"shadowColor",textPanel_->shadow_color.name(QColor::HexArgb)}};
        return state;
    }
    void initializeToolDefaults() {
        const QStringList tools{"pen","highlighter","rect","ellipse","arrow","mosaic","number","spotlight","text"};
        // 矩形与椭圆共用一块面板，必须先采集全部工厂值，再载入各自配置。
        // Rectangle/ellipse share a panel: snapshot all factory values before applying any saved defaults.
        for (const auto &tool:tools) toolDefaults_[tool]=panelStyle(tool);
        for (const auto &tool:tools) {
            auto state=toolDefaults_.value(tool);
            const auto saved=settings_ ? settings_->toolStyle(tool) : QVariantMap{};
            // 白名单合并、类型/范围校验，防止手改配置产生超大画笔、非法颜色或异常字体。
            // Merge known fields with type/range validation, bounding hand-edited widths and fonts.
            for (auto it=state.begin();it!=state.end();++it) {
                if (!saved.contains(it.key())) continue;
                const auto value=saved.value(it.key());
                if (it.key().contains("color",Qt::CaseInsensitive)) {
                    const QColor color(value.toString()); if (color.isValid()) it.value()=color.name(QColor::HexArgb);
                } else if (it.key()=="font") {
                    QFont font; if (value.toString().size()<2048 && font.fromString(value.toString())) {
                        font.setPointSizeF(std::clamp(font.pointSizeF(),double(jietuba::TextItem::MIN_POINT_SIZE),double(jietuba::TextItem::MAX_POINT_SIZE))); it.value()=font.toString();
                    }
                } else if (it.value().typeId()==QMetaType::Bool) {
                    if (value.typeId()==QMetaType::Bool) it.value()=value;
                } else if (it.value().typeId()!=QMetaType::QString) {
                    bool ok=false; const double number=value.toDouble(&ok);
                    if (ok && std::isfinite(number)) {
                        const double maximum=it.key().contains("Opacity") || it.key()=="opacity" ? 255.0 : it.key()=="outlineWidth" ? 8.0 : 100.0;
                        it.value()=std::clamp(number,it.key()=="width" || it.key()=="block" ? 1.0 : 0.0,maximum);
                    }
                } else if (value.toString().size()<64) it.value()=value.toString();
            }
            toolDefaults_[tool]=state; applyToolDefault(tool);
            toolDefaults_[tool]=panelStyle(tool); // 保存实际控件规范化后的值 / Retain values normalized by real controls.
        }
    }
    void rememberToolDefault() {
        if (!scene_ || view_->tool.isEmpty() || (view_->editor && view_->editor->active_layer())) return;
        const auto state=panelStyle(view_->tool);
        if (state.isEmpty()) return;
        toolDefaults_[view_->tool]=state;
        if (settings_ && !settings_->saveToolStyle(view_->tool,state)) qWarning("Unable to persist Linux tool style");
    }
    void rememberDrawMode(const QString &tool, const QString &mode) {
        // 手绘/矩形是下一次绘制的输入方式，不是现有图形样式；选中旧图形时也允许切换。
        // Freehand/rectangle is a drawing gesture, not an item style; changing it also affects the next drawing.
        toolDefaults_[tool]["mode"]=mode;
        if (settings_) settings_->saveToolStyle(tool,toolDefaults_.value(tool));
    }
    void connectDefaultPersistence() {
        // 这些连接排在真正修改样式的槽之后，只记录用户更改后的面板状态；读取旧图形时信号被阻止。
        // Run after style handlers, recording user changes only; item-to-panel synchronization blocks signals.
        const auto save=[this] { rememberToolDefault(); };
        for (auto *panel:QList<jietuba::BaseSettingsPanel *>{paintPanel_,highlightPanel_,shapePanel_,numberPanel_,spotlightPanel_}) {
            connect(panel,&jietuba::BaseSettingsPanel::color_changed,this,save);
            connect(panel,&jietuba::BaseSettingsPanel::size_changed,this,save);
            connect(panel,&jietuba::BaseSettingsPanel::opacity_changed,this,save);
        }
        connect(numberPanel_,&jietuba::BaseSettingsPanel::size_changed,this,[this](int size) {
            if (!scene_ || view_->tool!="number") return;
            // 序号创建后会自动选中，普通样式持久化因此会跳过它。用户明确调整字号时，
            // 同时更新后续序号的大小；选中旧序号时的面板同步被 QSignalBlocker 阻止，不会误保存。
            // Numbers auto-select after insertion, bypassing generic default persistence. An explicit size edit
            // also sets the next number's size; signal-blocked item inspection cannot overwrite this preference.
            toolDefaults_["number"]["width"]=size;
            if (settings_ && !settings_->saveToolStyle("number",toolDefaults_.value("number")))
                qWarning("Unable to persist Linux number size");
        });
        connect(paintPanel_,&jietuba::PaintSettingsPanel::line_style_changed,this,save);
        connect(shapePanel_,&jietuba::ShapeSettingsPanel::line_style_changed,this,save);
        connect(highlightPanel_,&jietuba::PaintSettingsPanel::highlighter_mode_changed,this,save);
        connect(highlightPanel_,&jietuba::PaintSettingsPanel::highlighter_mode_changed,this,[this](const QString &mode) { rememberDrawMode("highlighter",mode); });
        connect(arrowPanel_,&jietuba::ArrowSettingsPanel::color_changed,this,save);
        connect(arrowPanel_,&jietuba::ArrowSettingsPanel::size_changed,this,save);
        connect(arrowPanel_,&jietuba::ArrowSettingsPanel::opacity_changed,this,save);
        connect(arrowPanel_,&jietuba::ArrowSettingsPanel::arrow_style_changed,this,save);
        connect(mosaicPanel_,&jietuba::MosaicSettingsPanel::size_changed,this,save);
        connect(mosaicPanel_,&jietuba::MosaicSettingsPanel::style_changed,this,save);
        connect(mosaicPanel_,&jietuba::MosaicSettingsPanel::draw_mode_changed,this,save);
        connect(mosaicPanel_,&jietuba::MosaicSettingsPanel::draw_mode_changed,this,[this](const QString &mode) { rememberDrawMode("mosaic",mode); });
        connect(mosaicPanel_,&jietuba::MosaicSettingsPanel::block_size_changed,this,save);
        connect(numberPanel_,&jietuba::NumberSettingsPanel::style_changed,this,save);
        connect(textPanel_,&jietuba::TextSettingsPanel::font_changed,this,save);
        connect(textPanel_,&jietuba::TextSettingsPanel::color_changed,this,save);
        connect(textPanel_,&jietuba::TextSettingsPanel::background_changed,this,save);
        connect(textPanel_,&jietuba::TextSettingsPanel::outline_changed,this,save);
        connect(textPanel_,&jietuba::TextSettingsPanel::shadow_changed,this,save);
    }
    void applyToolDefault(const QString &tool) {
        const auto state=toolDefaults_.value(tool);
        if (state.isEmpty()) return;
        const QColor color(state.value("color").toString());
        const int width=state.value("width").toInt(), opacity=state.value("opacity").toInt();
        if (auto *panel=basePanel(tool)) {
            const QSignalBlocker blocker(panel);
            panel->set_color(color); panel->set_size(width); panel->set_opacity(opacity);
            if (tool=="pen") paintPanel_->set_line_style(state.value("line").toString());
            if (tool=="rect" || tool=="ellipse") shapePanel_->set_line_style(state.value("line").toString());
            if (tool=="highlighter") highlightPanel_->set_highlighter_mode(state.value("mode").toString());
            if (tool=="number") numberPanel_->set_style(state.value("style").toString());
        } else if (tool=="arrow") {
            const QSignalBlocker blocker(arrowPanel_);
            arrowPanel_->set_color(color); arrowPanel_->set_size(width); arrowPanel_->set_opacity(opacity);
            arrowPanel_->set_arrow_style(state.value("style").toString());
        } else if (tool=="mosaic") {
            const QSignalBlocker blocker(mosaicPanel_);
            mosaicPanel_->set_size(width); mosaicPanel_->set_draw_mode(state.value("mode").toString());
            mosaicPanel_->set_style(state.value("style").toString()); mosaicPanel_->set_block_size(state.value("block"));
        } else if (tool=="text") {
            const QSignalBlocker blocker(textPanel_);
            QFont font; font.fromString(state.value("font").toString()); textPanel_->set_font(font);
            textPanel_->current_color=color; textPanel_->color_btn->set_color(color);
            textPanel_->set_background_settings(state.value("background").toBool(),QColor(state.value("backgroundColor").toString()),state.value("backgroundOpacity").toInt());
            textPanel_->set_outline_settings(state.value("outline").toBool(),QColor(state.value("outlineColor").toString()),state.value("outlineWidth"));
            textPanel_->set_shadow_settings(state.value("shadow").toBool(),QColor(state.value("shadowColor").toString()));
            auto &text=view_->newTextStyle;
            text.font=font; text.textColor=color;
            text.hasBackground=textPanel_->background_enabled; text.background=textPanel_->background_color; text.background.setAlpha(textPanel_->background_opacity);
            text.hasOutline=textPanel_->outline_enabled; text.outline=textPanel_->outline_color; text.outlineWidth=textPanel_->outline_width;
            text.hasShadow=textPanel_->shadow_enabled; text.shadow=textPanel_->shadow_color;
        }
        if (!scene_) return;
        if (tool!="text") scene_->update_style(color.isValid() ? std::optional<QColor>(color) : std::nullopt,
            state.contains("width") ? std::optional<double>(width) : std::nullopt,
            state.contains("opacity") ? std::optional<double>(opacity/255.0) : std::nullopt);
        if (tool=="pen") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::PenTool>(tool)) t->line_style=paintPanel_->line_style();
        if (tool=="rect") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::RectTool>(tool)) t->line_style=shapePanel_->line_style();
        if (tool=="ellipse") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::EllipseTool>(tool)) t->line_style=shapePanel_->line_style();
        if (tool=="arrow") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::ArrowTool>(tool)) t->arrow_style=arrowPanel_->arrow_style();
        if (tool=="highlighter") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::HighlighterTool>(tool)) t->linux_mode=highlightPanel_->current_highlighter_mode;
        if (tool=="number") { if (auto *t=scene_->tool_controller->get_tool_as<jietuba::NumberTool>(tool)) t->linux_style=numberPanel_->current_style(); numberPanel_->set_next_number(jietuba::NumberTool::get_next_number(scene_)); }
        if (tool=="mosaic") if (auto *t=scene_->tool_controller->get_tool_as<jietuba::MosaicTool>(tool)) {
            t->linux_mode=mosaicPanel_->draw_mode(); t->linux_style=mosaicPanel_->style(); t->linux_block_size=mosaicPanel_->block_size();
        }
    }
    jietuba::MagnifierOverlay::Options magnifierOptions() const {
        jietuba::MagnifierOverlay::Options options;
        if (settings_) {
            options.enabled=settings_->magnifierOption("enabled"); options.grid=settings_->magnifierOption("grid");
            options.selection=settings_->magnifierOption("selection"); options.hints=settings_->magnifierOption("hints"); options.zoom=settings_->magnifierZoom();
            options.copyKey=QKeySequence::fromString(settings_->shortcut("copy_color"),QKeySequence::PortableText).toString(QKeySequence::NativeText);
            const auto enabled=settings_->magnifierFormats();
            for (auto &format:options.formats) format.enabled=enabled.contains(format.name);
        }
        return options;
    }
    bool handleMagnifierShift(QKeyEvent *event) {
        if (event->key()!=Qt::Key_Shift || toolbar_->preview_layout_dialog_open() || editingCanvasText()
            || !magnifier_ || !magnifier_->should_render()) return false;
        // Shift+Ctrl/Alt/Meta 仍属于组合快捷键；左右 Shift 均由 Qt 规范成 Key_Shift。
        // Ctrl/Alt/Meta combinations remain shortcuts; Qt normalizes both Shift keys to Key_Shift.
        if (event->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier)) return false;
        // ShortcutOverride 只保留事件，不切换；长按自动重复也不再次切换。
        // Reserve ShortcutOverride without cycling, and ignore auto-repeat for the actual switch.
        if (event->type()==QEvent::KeyPress && !event->isAutoRepeat()) magnifier_->cycle_color_format();
        return true;
    }
    bool handlePrecisionKey(QKeyEvent *e) {
        if (toolbar_->preview_layout_dialog_open()) return false;
        // 方向键以原图一个像素微调取色点，避免高 DPI 下逻辑像素跳过源像素。
        // Nudge sampling by one native pixel instead of a HiDPI logical pixel.
        if (!editingCanvasText() && magnifier_ && magnifier_->should_render() && e->modifiers()==Qt::NoModifier) {
            QPoint delta;
            if (e->key()==Qt::Key_Left) delta.setX(-1); else if (e->key()==Qt::Key_Right) delta.setX(1);
            else if (e->key()==Qt::Key_Up) delta.setY(-1); else if (e->key()==Qt::Key_Down) delta.setY(1);
            if (!delta.isNull()) {
                auto pos=*magnifier_->cursor_scene_pos+delta;
                pos.setX(std::clamp(pos.x(),0.0,double(baseImage_.width()-1)));
                pos.setY(std::clamp(pos.y(),0.0,double(baseImage_.height()-1)));
                magnifier_->update_cursor(pos); e->accept(); return true;
            }
        }
        return false;
    }
    bool editingCanvasText() const override {
        auto *text=scene_ ? dynamic_cast<QGraphicsTextItem *>(scene_->focusItem()) : nullptr;
        return text && text->hasFocus() && text->textInteractionFlags().testFlag(Qt::TextEditable);
    }
    bool invokeShortcut(const QString &action) {
        if (!scene_) return false;
        if (action=="copy_color") return magnifier_ && magnifier_->should_render() && magnifier_->copy_color_info();
        if (action=="zoom_in" || action=="zoom_out") {
            if (!magnifier_ || !magnifier_->should_render()) return false;
            magnifier_->adjust_zoom(action=="zoom_in" ? 1 : -1); return true;
        }
        if (action=="cursor") {
            finishText(); if (view_->editor) view_->editor->stop_edit(); view_->tool.clear(); scene_->activate_tool("cursor");
            for (auto *button:toolbar_->tool_buttons) button->setChecked(false);
            hidePanels(); view_->setFocus(); if(recordingMode_) {setAttribute(Qt::WA_TransparentForMouseEvents); if(recordingChanged) recordingChanged();} return true;
        }
        if (action=="restore_region") {
            if (settings_) { const auto rect=settings_->lastRegion(baseImage_.size(),screen() ? screen()->name() : QString());
                if (!rect.isEmpty()) { scene_->selection_model->initialize_confirmed_rect(rect); view_->selectionChanged();
                    if (view_->selectionFinished) view_->selectionFinished(); view_->viewport()->update(); } }
            return true;
        }
        if (scene_->selection_model->is_empty() || !scene_->selection_model->is_confirmed()) return false;
        if (action=="delete") {
            auto *item=view_->editor ? dynamic_cast<jietuba::TrackedItem *>(view_->editor->active_layer()) : nullptr;
            if (item) { const bool number=dynamic_cast<jietuba::NumberItem *>(item); jietuba::ItemReference ref(item); view_->editor->stop_edit();
                if (number) scene_->undo_stack->push(new jietuba::RemoveNumberCommand(scene_,std::move(ref)));
                else scene_->undo_stack->push(new jietuba::RemoveItemCommand(scene_,std::move(ref)));
                view_->viewport()->update(); }
            return true;
        }
        QAbstractButton *button=toolbar_->tool_buttons.value(action);
        if (!button) {
            const QHash<QString,QAbstractButton *> actions{{"copy",toolbar_->confirm_btn},{"save",toolbar_->save_btn},{"quick_save",toolbar_->quick_save_btn},
                {"pin",toolbar_->pin_btn},{"undo",toolbar_->undo_btn},{"redo",toolbar_->redo_btn},{"translate",toolbar_->screenshot_translate_btn},
                {"ocr",toolbar_->text_recognize_btn},{"barcode",toolbar_->scan_code_btn},{"long_capture",toolbar_->long_screenshot_btn},{"gif",toolbar_->gif_btn}};
            button=actions.value(action);
        }
        // 隐藏到“更多”中的功能也可快捷触发，只有真正禁用的动作不可执行。
        // Folded/hidden toolbar actions remain callable; genuinely disabled actions do not.
        if (button && button->isEnabled()) { button->click(); return true; }
        return false;
    }
    void startCapture() {
        if (captureHandler_) { captureHandler_(); return; }
        if (capturePending_) return;
        if (scene_ && !scene_->selection_model->is_empty() &&
            QMessageBox::question(this, QStringLiteral("开始新截图"), QStringLiteral("开始新截图会替换当前选区，是否继续？")) != QMessageBox::Yes) return;
        capturePending_ = true;
        captureButton_->setEnabled(false);
        // 在窗口隐藏之前确定所在显示器；各后端都以同一输出名称为目标。
        // Resolve the output before hiding, so every backend captures the same screen.
        auto *screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
        const QString outputName = screen ? screen->name() : QString();
        // 给合成器一点时间移除预览窗口，避免在自己的截图中看到它。
        // Give the compositor time to remove the preview before requesting a frame.
        hide();
        QTimer::singleShot(300, this, [this, outputName] { if (capturePending_) capture_->capture(outputName); });
    }
    void finishCaptureRequest() {
        capturePending_ = false;
        captureButton_->setEnabled(true);
        if (view_->regionCapture) {
            // 失败时恢复最后成功的底图及选区，不留下透明的半次拖动状态。
            // Restore the last valid region after failure rather than leaving a half-finished transparent drag.
            scene_->background->show();
            setInitialRegion(regionFrame_);
            showPreparedOverlay();
            return;
        }
        show();
        activateWindow();
    }
    void hidePanels() {
        for (auto *container : panelContainers_) container->hide();
    }
    void syncDefaultPanel(const QString &tool) {
        if (!scene_) return;
        const auto &ctx = scene_->tool_controller->ctx;
        const int opacity = std::clamp(int(ctx.opacity * 255), 0, 255);
        if (tool == "pen" || tool == "rect" || tool == "ellipse") {
            auto *panel = tool == "pen" ? static_cast<jietuba::BaseSettingsPanel *>(paintPanel_) : shapePanel_;
            QSignalBlocker blocker(panel);
            panel->set_color(ctx.color); panel->set_size(int(ctx.stroke_width)); panel->set_opacity(opacity);
        } else if (tool == "arrow") {
            QSignalBlocker blocker(arrowPanel_);
            arrowPanel_->set_color(ctx.color); arrowPanel_->set_size(int(ctx.stroke_width)); arrowPanel_->set_opacity(opacity);
        }
    }
    void showPanel(const QString &tool) {
        if (toolbar_->preview_layout_dialog_open()) { hidePanels(); return; }
        if (toolbar_->more_popup() && toolbar_->more_popup()->isVisible()) { hidePanels(); return; }
        QWidget *panel = nullptr;
        if (tool == "pen") panel = paintPanel_;
        else if (tool == "highlighter") panel = highlightPanel_;
        else if (tool == "rect" || tool == "ellipse") panel = shapePanel_;
        else if (tool == "arrow") panel = arrowPanel_;
        else if (tool == "text") panel = textPanel_;
        else if (tool == "mosaic") panel = mosaicPanel_;
        else if (tool == "number") panel = numberPanel_;
        else if (tool == "spotlight") panel = spotlightPanel_;
        else if (tool == "watermark") panel = watermarkPanel_;
        // 撤销栈在拖动滑块时会持续变化；不要隐藏当前面板，否则会中断鼠标拖动。
        // Undo changes continuously during slider drags; never hide the active panel.
        QWidget *anchor=recordingMode_ ? recordingControls_.data() : toolbar_;
        for (auto it = panelContainers_.begin(); it != panelContainers_.end(); ++it)
            if (it.key() != panel || !anchor || !anchor->isVisible()) it.value()->hide();
        if (!panel || !anchor || !anchor->isVisible()) return;
        auto *container = panelContainers_.value(panel);
        if (!container->isVisible()) panel->adjustSize();
        const QSize available = anchor->parentWidget()->size();
        const int containerWidth = std::min(available.width(), panel->width());
        const int scrollbar = panel->width() > containerWidth ? container->horizontalScrollBar()->sizeHint().height() : 0;
        container->setFixedSize(containerWidth, std::min(available.height(), panel->height() + scrollbar));
        // 工具栏在选区下方时，面板也优先放下方；上方时则向上展开。
        // Expand away from the selection, preferring below when the toolbar is below.
        const int below = anchor->y() + anchor->height() + 5;
        const int above = anchor->y() - container->height() - 5;
        int y = toolbarBelowSelection_ ? below : above;
        const int other = toolbarBelowSelection_ ? above : below;
        if (y < 0 || y + container->height() > available.height()) y = other;
        y = std::clamp(y, 0, std::max(0, available.height() - container->height()));
        container->move(std::max(0, std::min(anchor->x(), available.width() - containerWidth)), y);
        if (!container->isVisible()) container->show();
        container->raise();
    }
    // 每次样式操作生成一条撤销命令；图形引用由 ItemReference 管理。
    // Each style change creates one undo command using a lifetime-safe item reference.
    void changeSelected(const std::function<void(PreviewStyle &)> &change) {
        if (!scene_ || !view_->editor) return;
        auto *item = view_->editor->active_layer();
        if (!item || !item->scene()) return;
        auto *tracked = dynamic_cast<jietuba::TrackedItem *>(item);
        if (!tracked) return;
        auto before = readStyle(item), after = before;
        change(after);
        if (!(before == after)) scene_->undo_stack->push(new PreviewStyleCommand(tracked, std::move(before), std::move(after)));
        view_->editor->refresh_handles();
        view_->viewport()->update();
    }
    void changePen(const std::function<void(QPen &)> &change, std::optional<QColor> color,
                   std::optional<double> width, std::optional<double> opacity) {
        if (!scene_) return;
        scene_->update_style(color, width, opacity);
        if (view_->editor && view_->editor->active_layer() && !dynamic_cast<jietuba::TextItem *>(view_->editor->active_layer()))
            changeSelected([&](PreviewStyle &s) { change(s.pen); });
    }
    void changeText(const std::function<void(PreviewStyle &)> &change) {
        const bool selected = view_->editor && dynamic_cast<jietuba::TextItem *>(view_->editor->active_layer());
        if (!selected) change(view_->newTextStyle);
        if (selected) {
            // 先提交正在输入的文字，保证撤销顺序是「文字内容 → 样式」。
            // Commit text before styling so undo reverts style before text creation.
            finishText();
            if (view_->editor->active_layer()) changeSelected(change);
        }
    }
    void changeNumberExtra(const QString &key, const QVariant &value) {
        if (!scene_ || !view_->editor) return;
        auto *item = dynamic_cast<jietuba::NumberItem *>(view_->editor->active_layer());
        if (!item || !item->scene()) return;
        auto before = item->extras_state(), after = before;
        after.insert(key, value);
        if (before != after) scene_->undo_stack->push(new jietuba::EditItemCommand(jietuba::ItemReference(item), before, after));
    }
    // 水印设置以 Linux 的独立配置文件保存；UI 操作失败时明确提示。
    // Persist watermark defaults through the Linux settings store and surface write errors.
    void persistWatermark() {
        if (!settings_) return;
        settings_->setWatermarkText(watermarkText_); settings_->setWatermarkStyle(watermarkStyle_.to_values());
        if (settings_->sync() != QSettings::NoError)
            QMessageBox::warning(this, QStringLiteral("设置保存失败"), QStringLiteral("无法写入水印设置：\n%1").arg(settings_->fileName()));
    }
    void syncWatermarkPanel() {
        if (!watermarkPanel_) return;
        QSignalBlocker blocker(watermarkPanel_);
        if (auto *item = scene_ ? jietuba::WatermarkItem::find(scene_) : nullptr) {
            watermarkPanel_->load_style(item->style()); watermarkPanel_->set_template(item->template_text(), item->created_at());
        } else {
            watermarkPanel_->load_style(watermarkStyle_); watermarkPanel_->set_template(watermarkText_);
        }
    }
    void addWatermark(const QString &text) {
        if (!scene_ || text.isEmpty() || jietuba::WatermarkItem::find(scene_)) return;
        auto *item = new jietuba::WatermarkItem(text, watermarkStyle_);
        scene_->undo_stack->push(new jietuba::AddItemCommand(scene_, jietuba::ItemReference(item), QStringLiteral("添加水印")));
        syncWatermarkPanel();
    }
    void editWatermark() {
        if (!scene_) return;
        auto *existing = jietuba::WatermarkItem::find(scene_);
        jietuba::WatermarkInputDialog dialog(existing ? existing->template_text() : watermarkText_, jietuba::default_watermark_time_format(), this);
        dialog.place_near(watermarkPanel_->text_field);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto text = dialog.template_text();
        if (text.isEmpty()) { clearWatermark(); return; }
        watermarkText_ = text;
        if (existing) {
            const WatermarkChangeCommand::State before{existing->template_text(), existing->created_at(), existing->style()};
            const WatermarkChangeCommand::State after{text, QDateTime::currentDateTime(), existing->style()};
            if (before.text != after.text) scene_->undo_stack->push(new WatermarkChangeCommand(existing, before, after, {}));
        } else addWatermark(text);
        syncWatermarkPanel(); persistWatermark();
    }
    void clearWatermark() {
        if (!scene_) return;
        watermarkText_.clear();
        if (auto *item = jietuba::WatermarkItem::find(scene_))
            scene_->undo_stack->push(new jietuba::RemoveItemCommand(scene_, jietuba::ItemReference(item), QStringLiteral("删除水印")));
        syncWatermarkPanel(); persistWatermark();
    }
    void changeWatermarkStyle(const QString &key, const QVariant &value) {
        watermarkStyle_ = watermarkStyle_.with_changes({{key, value}});
        if (scene_) if (auto *item = jietuba::WatermarkItem::find(scene_)) {
            const WatermarkChangeCommand::State before{item->template_text(), item->created_at(), item->style()};
            const WatermarkChangeCommand::State after{before.text, before.time, before.style.with_changes({{key, value}})};
            if (before.style != after.style) {
                // 拖动滑块时面板已是最新值，避免反向同步使弹窗关闭。
                // During slider drags the panel is already current; do not resync and hide its popup.
                updatingWatermark_ = true;
                scene_->undo_stack->push(new WatermarkChangeCommand(item, before, after, key));
                updatingWatermark_ = false;
            }
        }
        persistWatermark();
    }
    void selectItem(QGraphicsItem *item) {
        // 根据图形类型选择对应工具和原版面板，避免用户手动寻找样式设置。
        // Select the matching tool and original panel based on the item's type.
        const QString id = annotationTool(item);
        if (id.isEmpty()) return;
        if (view_->tool != id) {
            scene_->activate_tool(id);
            view_->tool = id;
        }
        for (auto it = toolbar_->tool_buttons.begin(); it != toolbar_->tool_buttons.end(); ++it) it.value()->setChecked(it.key() == id);
        // 同步面板时阻止信号，避免把读取样式误记为一次编辑。
        // Suppress panel signals while reading state to avoid recording a spurious edit.
        if (id == "pen") { QSignalBlocker blocker(paintPanel_); paintPanel_->set_state_from_item(item); }
        else if (id == "highlighter") {
            QSignalBlocker blocker(highlightPanel_);
            highlightPanel_->set_state_from_item(item);
            highlightPanel_->set_highlighter_mode(dynamic_cast<jietuba::RectItem *>(item) ? "rect" : "freehand");
            if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::HighlighterTool>("highlighter")) {
                tool->linux_mode = highlightPanel_->current_highlighter_mode;
            }
            scene_->update_style(highlightPanel_->current_color, double(highlightPanel_->current_size), highlightPanel_->current_opacity / 255.0);
        }
        else if (id == "rect" || id == "ellipse") { QSignalBlocker blocker(shapePanel_); shapePanel_->set_state_from_item(item); }
        else if (id == "arrow") {
            QSignalBlocker blocker(arrowPanel_);
            auto *arrow = static_cast<jietuba::ArrowItem *>(item);
            arrowPanel_->set_color(arrow->color); arrowPanel_->set_size(int(arrow->base_width));
            arrowPanel_->set_opacity(arrow->color.alpha()); arrowPanel_->set_arrow_style(arrow->arrow_style());
        } else if (id == "text") { QSignalBlocker blocker(textPanel_); textPanel_->set_state_from_item(static_cast<jietuba::TextItem *>(item)); }
        else if (id == "mosaic") {
            auto *mosaic = static_cast<jietuba::MosaicItem *>(item);
            QSignalBlocker blocker(mosaicPanel_);
            mosaicPanel_->set_draw_mode(mosaic->fill_mode() ? jietuba::MosaicTool::MODE_RECT : jietuba::MosaicTool::MODE_FREEHAND);
            mosaicPanel_->set_style(jietuba::MosaicTool::style_of(mosaic->kind()));
            mosaicPanel_->set_block_size(mosaic->block_size()); mosaicPanel_->set_size(int(mosaic->brush_width()));
            if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::MosaicTool>("mosaic")) {
                tool->linux_mode = mosaicPanel_->draw_mode(); tool->linux_style = mosaicPanel_->style(); tool->linux_block_size = mosaicPanel_->block_size();
            }
            scene_->update_style({}, mosaic->brush_width(), {});
        }
        else if (id == "number") {
            auto *number = static_cast<jietuba::NumberItem *>(item);
            QSignalBlocker blocker(numberPanel_);
            numberPanel_->set_color(number->color); numberPanel_->set_size(int(number->get_stroke_width().value_or(16)));
            numberPanel_->set_opacity(number->color.alpha()); numberPanel_->set_style(number->style);
            numberPanel_->set_next_number(jietuba::NumberTool::get_next_number(scene_));
            numberPanel_->set_label_section_visible(number->label() && number->label()->isVisible());
            numberPanel_->set_label_state(number->label_font_family(), number->label_size_level(), number->label_outline(), number->label_guide(), number->arrow_style());
            if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::NumberTool>("number")) tool->linux_style = number->style;
            scene_->update_style(number->color, number->get_stroke_width(), number->color.alphaF());
        }
        else if (id == "spotlight") {
            auto *spotlight = static_cast<jietuba::SpotlightItem *>(item);
            const auto opacity = spotlight->get_visual_opacity().value_or(jietuba::SpotlightCurtain::DEFAULT_DARKNESS);
            QSignalBlocker blocker(spotlightPanel_); spotlightPanel_->set_opacity(qRound(opacity * 255));
            scene_->update_style({}, {}, opacity);
        }
        if (id == "pen" || id == "rect" || id == "ellipse") {
            auto *panel = id == "pen" ? static_cast<jietuba::BaseSettingsPanel *>(paintPanel_)
                                      : static_cast<jietuba::BaseSettingsPanel *>(shapePanel_);
            scene_->update_style(panel->current_color, double(panel->current_size), panel->current_opacity / 255.0);
        } else if (id == "arrow") scene_->update_style(arrowPanel_->current_color, double(arrowPanel_->current_size), arrowPanel_->opacity_spin->value() / 100.0);
        showPanel(id);
        if(recordingMode_ && recordingChanged) recordingChanged();
    }
    void setImage(const QImage &image) {
        finishText();
        baseImage_ = image;
        delete magnifier_; magnifier_=nullptr;
        toolbar_->hide();
        hidePanels();
        // 切换图片或开始新会话时，工具栏不能保留上一会话的选中状态。
        // A new image/session must not retain checked tools from the previous one.
        for (auto *button : toolbar_->tool_buttons) button->setChecked(false);
        view_->setCanvas(nullptr);
        view_->setScene(nullptr);
        delete scene_;
        scene_ = new jietuba::CanvasScene(image, QRectF(QPointF(0, 0), image.size()), true);
        // 新画布从当前面板继承默认值，避免界面显示的样式与实际新笔画不一致。
        // Keep new-scene tool defaults in sync with the already visible panel values.
        if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::MosaicTool>("mosaic")) {
            tool->linux_mode = mosaicPanel_->draw_mode(); tool->linux_style = mosaicPanel_->style(); tool->linux_block_size = mosaicPanel_->block_size();
        }
        if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::NumberTool>("number")) tool->linux_style = numberPanel_->current_style();
        if (auto *tool = scene_->tool_controller->get_tool_as<jietuba::HighlighterTool>("highlighter")) {
            tool->linux_mode = highlightPanel_->current_highlighter_mode;
        }
        view_->setCanvas(scene_);
        if (pinHost_) {
            // 贴图的选区永久锁定在原始像素边界；窗口缩放只改变视图变换。
            // Lock the pin selection to native image bounds; zoom changes only the view transform.
            scene_->preset_selection(scene_->sceneRect());
            scene_->selection_item->hide();
            toolbar_->undo_btn->setEnabled(false);
            toolbar_->redo_btn->setEnabled(false);
            connect(scene_->undo_stack, &QUndoStack::canUndoChanged, toolbar_->undo_btn, &QPushButton::setEnabled);
            connect(scene_->undo_stack, &QUndoStack::canRedoChanged, toolbar_->redo_btn, &QPushButton::setEnabled);
        } else {
            magnifier_=new jietuba::MagnifierOverlay(this,scene_,view_,magnifierOptions());
            magnifier_->setObjectName(QStringLiteral("linuxCaptureMagnifier"));
            magnifier_->interaction_blocked=[this] {
                return toolbar_->preview_layout_dialog_open() || view_->drawing || view_->moving || editingCanvasText()
                    || (view_->editor && view_->editor->dragging_handle);
            };
            magnifier_->zoom_changed=[this](double zoom) { if (settings_) { settings_->setMagnifierZoom(zoom); settings_->sync(); } };
            view_->pointerChanged=[this](std::optional<QPointF> pos) {
                if (!magnifier_) return;
                if (pos) magnifier_->update_cursor(*pos); else magnifier_->clear_cursor();
            };
        }
        connect(scene_, &jietuba::CanvasScene::item_auto_select_requested, this, [this](QGraphicsItem *item) {
            if (view_->editor && view_->editor->start_edit(item)) selectItem(item);
        });
        connect(scene_->undo_stack, &QUndoStack::indexChanged, this, [this] {
            if(recordingMode_ && recordingChanged) recordingChanged();
            numberPanel_->set_next_number(jietuba::NumberTool::get_next_number(scene_));
            if (!updatingWatermark_) syncWatermarkPanel();
            // 用户正在拖动滑块时，面板本身已是最新值，不要反向写入控件。
            // During an active drag the panel already has the latest value; do not resync it.
            if (textPanel_->background_opacity_slider->isSliderDown() || textPanel_->shadow_opacity_slider->isSliderDown() ||
                textPanel_->outline_width_slider->isSliderDown()) return;
            if (view_->editor && view_->editor->active_layer() && view_->editor->active_layer()->scene())
                selectItem(view_->editor->active_layer());
        });
        view_->tool.clear();
        view_->setScene(scene_);
        view_->fit();
        view_->viewport()->setFocus();
        if (captureButton_) {
            captureButton_->setVisible(!overlay_);
            if (!overlay_) captureButton_->raise();
        }
    }
    void finishText() { if (scene_ && scene_->focusItem()) scene_->focusItem()->clearFocus(); }
    void resetSelection() {
        if (overlay_) { if (finished_) finished_(); return; }
        // 结束本次选区并清理标注与撤销记录，保留原图以便重新选择。
        // End the entire capture session, including annotations and undo history.
        // Keep the preview open and its original image available for a new selection.
        for (auto *button : toolbar_->tool_buttons) button->setChecked(false);
        setImage(baseImage_);
    }
    QImage result() override {
        finishText();
        if (!scene_ || scene_->selection_model->is_empty()) return {};
        // 导出前等待重建结果；失败时禁止输出原图，避免把失败当成擦除成功。
        // Refuse export if smart reconstruction failed rather than silently exposing the source.
        for (auto *item : scene_->items(scene_->selection_model->rect(), Qt::IntersectsItemBoundingRect)) {
            auto *mosaic = dynamic_cast<jietuba::MosaicItem *>(item);
            if (mosaic && mosaic->isVisible() && !mosaic->ensure_smart_erase_result()) {
                QMessageBox::warning(this, QStringLiteral("智能擦除失败"), QStringLiteral("无法重建所选区域，请调整擦除范围或撤销该操作后再导出。"));
                return {};
            }
        }
        auto image = jietuba::ExportService(scene_).export_image(scene_->selection_model->rect());
        if (compositeChildren && !image.isNull()) compositeChildren(image, outputCrop());
        return image;
    }
    void saveBaseIfRequested(const QString &path, const QImage &image) {
        if (settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::SaveOriginal))
            LinuxCaptureDelivery::saveOriginal(path,jietuba::ExportService(scene_).export_base_image_only(scene_->selection_model->rect()),image,this);
    }
    void copy() {
        const auto image=result(); if (image.isNull()) return;
        QString path;
        if (settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::AutoSave)) {
            path=LinuxCaptureDelivery::save(*settings_,image);
            if (!path.isEmpty()) saveBaseIfRequested(path,image);
        }
        if (!LinuxCaptureDelivery::copy(image,settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::FileUrl) ? path : QString())) {
            QMessageBox::warning(this,"复制失败","无法生成剪贴板图片。"); return;
        }
        if (settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::AutoSave) && path.isEmpty())
            QMessageBox::warning(this,"自动保存失败","图片已复制，但未能写入保存目录。");
        resetSelection();
    }
    void pin() {
        if (!pinHandler_) return;
        const auto image = result();
        if (image.isNull()) return;
        if (settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::SavePin)) {
            const auto path=LinuxCaptureDelivery::save(*settings_,image,"贴图_");
            if (path.isEmpty()) QMessageBox::warning(this,"贴图自动保存失败","无法写入保存目录。"); else saveBaseIfRequested(path,image);
        }
        // 选区在场景中使用截图像素；贴图窗口需要显示器内的逻辑坐标。
        // Convert scene pixels to output-local logical coordinates, including the viewport offset.
        QRectF target = view_->viewportTransform().mapRect(scene_->selection_model->rect());
        target.translate(view_->viewport()->mapTo(this, QPoint(0, 0)));
        pinHandler_(image, windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen(), target);
    }
    void startGifCapture() {
        if (!gifCaptureHandler_ || !scene_ || !scene_->selection_model->is_confirmed()) return;
        // 与长截图一样传递原生像素选区；录制的是后续实时桌面，不是这张冻结画布。
        // Pass native crop coordinates; recording captures the live desktop after this canvas closes.
        const QRect region = scene_->selection_model->rect().normalized().toAlignedRect().intersected(baseImage_.rect());
        if (region.width() < 16 || region.height() < 16) {
            QMessageBox::warning(this, QStringLiteral("无法录制 GIF"), QStringLiteral("选区至少需要 16 × 16 像素。")); return;
        }
        auto *screen = windowHandle() ? windowHandle()->screen() : nullptr;
        // GIF 同样沿用框选时的原生区域接口，不能混用按最大输出缩放生成的画布与整屏帧。
        // Keep GIF capture on the original logical area; its scale can differ from a full-output frame.
        if (screen) gifCaptureHandler_(screen,baseImage_.size(),region,view_->regionCapture ? regionRequested_ : QRect());
    }
    void startLongCapture() {
        if (!longCaptureHandler_ || !scene_ || !scene_->selection_model->is_confirmed()) return;
        // 全程使用采集图片的原始像素坐标，而非桌面的逻辑坐标；缩放和多屏不会改变裁剪区域。
        // Keep the selection in output-local native pixels for every later frame.
        const QRect region = scene_->selection_model->rect().normalized().toAlignedRect().intersected(baseImage_.rect());
        if (region.width() < 32 || region.height() < 48) {
            QMessageBox::warning(this, QStringLiteral("无法开始长截图"), QStringLiteral("选区至少需要 32 × 48 像素。")); return;
        }
        auto *screen = windowHandle() ? windowHandle()->screen() : nullptr;
        if (!screen) return;
        // 区域模式沿用原先的逻辑区域接口采集每一帧；不能把区域首帧与整屏后续帧混用。
        // Keep continuous capture on the same logical area API as the first frame.
        longCaptureHandler_(screen, baseImage_.size(), region, baseImage_.copy(region),
                            view_->regionCapture ? regionRequested_ : QRect());
    }
    void recognizeText() {
        finishText();
        if (!scene_ || !scene_->selection_model->is_confirmed() || scene_->selection_model->is_empty()) return;
        const QImage image = jietuba::ExportService(scene_).export_base_image_only(scene_->selection_model->rect());
        if (image.isNull()) { QMessageBox::warning(this, QStringLiteral("文字识别失败"), QStringLiteral("无法导出选区底图。")); return; }
        const bool direct=settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::CopyOcr);
        
        // 自动选择 OCR 引擎：优先使用 PP-OCR（如果可用）
        std::function<LinuxOcrResult(const QImage &)> recognizer;
        if (LinuxPpocrEngine::available()) {
            recognizer = [](const QImage &img) { return LinuxPpocrEngine::recognize(img); };
        }
        
        auto *window = new LinuxOcrResultWindow(image,nullptr,recognizer,direct);
        placeCaptureResult(window,screen(),outputCrop().toAlignedRect(),settings_ ? settings_->ocrPosition() : QStringLiteral("center"));
        if (!direct) window->show();
        resetSelection();
    }
    void scanCode() {
        finishText();
        if (!scene_ || !scene_->selection_model->is_confirmed() || scene_->selection_model->is_empty()) return;
        // 条码解码与 Windows 一样使用选区原图，避免标注遮盖二维码模块。
        // Decode the unannotated selection, so drawings cannot hide QR modules.
        const QImage image = jietuba::ExportService(scene_).export_base_image_only(scene_->selection_model->rect());
        if (image.isNull()) { QMessageBox::warning(this, QStringLiteral("扫码失败"), QStringLiteral("无法导出选区底图。")); return; }
        const bool direct=settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::CopyCode);
        auto *window = new LinuxBarcodeResultWindow(image,nullptr,{},direct);
        if (!direct) window->show();
        resetSelection();
    }
    void translateImage() {
        finishText();
        if (!scene_ || !scene_->selection_model->is_confirmed() || scene_->selection_model->is_empty()) return;
        if (!settings_) { QMessageBox::warning(this, QStringLiteral("截图翻译失败"), QStringLiteral("翻译设置尚未初始化。")); return; }
        // 翻译先 OCR 未标注原图，再交给用户配置的网络服务；不会上传整张截图。
        // OCR the unannotated selection locally; only recognized text reaches the configured service.
        const QImage image = jietuba::ExportService(scene_).export_base_image_only(scene_->selection_model->rect());
        if (image.isNull()) { QMessageBox::warning(this, QStringLiteral("截图翻译失败"), QStringLiteral("无法导出选区底图。")); return; }
        auto *window = new LinuxTranslationResultWindow(image, *settings_);
        window->show();
        resetSelection();
    }
    void quickSave() {
        const QImage image = result();
        if (image.isNull()) return;
        LinuxSettings fallback;
        const auto path=LinuxCaptureDelivery::save(settings_ ? *settings_ : fallback,image);
        if (path.isEmpty()) { QMessageBox::warning(this,"快速保存失败","无法写入保存目录。"); return; }
        saveBaseIfRequested(path,image);
        resetSelection();
    }
    void save() {
        const auto image = result();
        if (image.isNull()) return;
        // layer-shell 覆盖层不能作为普通文件窗口的父窗口：它仍会压在对话框上方。
        // Withdraw the exclusive overlay and use an independent dialog so it can receive input.
        // 取消或保存失败时恢复同一文档；成功后按原流程结束选区。
        // Restore the same document on cancellation/failure; finish the session on success.
        const bool suspended = overlay_ && isVisible();
        // 文件对话框的生命期必须短于恢复逻辑：嵌套的 exec() 返回时，原生表面可能还在释放焦点。
        // Destroy the modal before restoring focus. All mapped capture layers must yield, not only the owner.
        bool saved = false;
        if (suspended) { if (saveDialogHandler_) saveDialogHandler_(true); else withdrawOverlay(); }
        const auto restore = qScopeGuard([&] {
            if (saved) resetSelection();
            if (!suspended) return;
            if (saveDialogHandler_) saveDialogHandler_(false);
            else if (!saved) showPreparedOverlay();
        });
        QFileDialog dialog(overlay_ ? nullptr : this, QStringLiteral("保存图片"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilters({QStringLiteral("PNG 图片 (*.png)"), QStringLiteral("JPEG 图片 (*.jpg *.jpeg)"), QStringLiteral("BMP 图片 (*.bmp)")});
        const QString preferred = settings_ ? settings_->imageFormat() : QStringLiteral("png");
        dialog.selectNameFilter(preferred == "jpg" ? QStringLiteral("JPEG 图片 (*.jpg *.jpeg)") : preferred == "bmp" ? QStringLiteral("BMP 图片 (*.bmp)") : QStringLiteral("PNG 图片 (*.png)"));
        dialog.setDefaultSuffix(preferred);
        connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog](const QString &filter) {
            dialog.setDefaultSuffix(filter.startsWith("JPEG") ? "jpg" : filter.startsWith("BMP") ? "bmp" : "png");
        });
        // 仅提供默认目录和格式；用户仍可在对话框中修改最终位置与名称。
        // Defaults are suggestions; the save dialog still lets the user choose another path.
        const QString name = (settings_ ? LinuxCaptureDelivery::stem(*settings_) : QStringLiteral("截图")) + "." + preferred;
        dialog.selectFile(settings_ ? QDir(settings_->saveDirectory()).filePath(name) : name);
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
        const auto path = dialog.selectedFiles().constFirst();
        if (path.isEmpty()) return;
        saved = image.save(path);
        if (saved) saveBaseIfRequested(path,image);
        if (!saved) QMessageBox::warning(overlay_ ? nullptr : this, QStringLiteral("保存失败"), QStringLiteral("无法保存图片，请检查文件名和目录权限。"));
    }
    bool pinToolActive() const override { return !view_->tool.isEmpty(); }
    void endPinEditing() override {
        finishText();
        if (view_->editor) view_->editor->stop_edit();
        view_->tool.clear();
        scene_->activate_tool("cursor");
        for (auto *button : toolbar_->tool_buttons) button->setChecked(false);
        hidePanels();
        setAttribute(Qt::WA_TransparentForMouseEvents);
        view_->viewport()->update();
    }
    void positionToolbar() {
        if(recordingMode_) { layoutRecordingPanels(); return; }
        if (pinHost_) { showPanel(view_->tool); return; }

        // 布局重排或窗口缩放不得把工具栏抬到调整遮罩之上；关闭面板后再统一重新定位。
        // Never raise the toolbar over the editor during layout/resize; reposition after dismissal.
        if (toolbar_->preview_layout_dialog_open()) { hidePanels(); return; }
        if (!scene_ || !scene_->selection_model->is_confirmed() || scene_->selection_model->is_dragging() || scene_->selection_model->is_empty()) {
            toolbar_->hide(); hidePanels();
            const bool canCapture = scene_ && !scene_->selection_model->is_dragging() && scene_->selection_model->is_empty();
            captureButton_->setVisible(!overlay_ && canCapture);
            return;
        }
        const auto selection = view_->mapFromScene(scene_->selection_model->rect()).boundingRect();
        toolbar_->set_preview_available_width(width());
        const int x = std::max(0, std::min(width() - toolbar_->width(), selection.right() - toolbar_->width()));
        const int gap = 10;
        // 尽可能为工具栏、调整面板和二级弹窗预留空间。
        // Reserve space for the toolbar, settings panel and secondary popup when possible.
        int maxPanel = 0;
        for (auto *panel : panelContainers_.keys()) maxPanel = std::max(maxPanel, panel->sizeHint().height());
        int maxPopup = 0;
        for (auto *popup : findChildren<jietuba::HoverPopup *>()) maxPopup = std::max(maxPopup, popup->sizeHint().height());
        const int stack = toolbar_->height() + maxPanel + maxPopup + 20;
        const int below = selection.bottom() + gap;
        const int above = selection.top() - toolbar_->height() - gap;
        const int roomBelow = height() - below;
        const int roomAbove = selection.top() - gap;
        toolbarBelowSelection_ = roomBelow >= stack || (roomAbove < stack && roomBelow >= roomAbove && roomBelow >= toolbar_->height());
        int y = toolbarBelowSelection_ ? below : above;
        if (y < 0 || y + toolbar_->height() > height()) {
            // 全屏选区没有外部空间，只能把工具栏放进选区内的底部。
            // Full-window selections have no outside space; fall back inside at the bottom.
            toolbarBelowSelection_ = false;
            y = height() - toolbar_->height() - gap;
        }
        y = std::clamp(y, 0, std::max(0, height() - toolbar_->height()));
        toolbar_->set_preview_below_selection(toolbarBelowSelection_);
        toolbar_->move(x, y);
        toolbar_->show();
        toolbar_->raise();
        showPanel(view_->tool);
    }
    LinuxPinToolbar *pinHost_ = nullptr;
    bool recordingMode_=false;
    QPointer<QWidget> recordingControls_;
    QHash<QString,QVariantMap> toolDefaults_;
    jietuba::MagnifierOverlay *magnifier_ = nullptr;
    PreviewView *view_ = nullptr;
    jietuba::Toolbar *toolbar_ = nullptr;
    jietuba::PaintSettingsPanel *paintPanel_ = nullptr;
    jietuba::PaintSettingsPanel *highlightPanel_ = nullptr;
    jietuba::PaintSettingsPanel *spotlightPanel_ = nullptr;
    jietuba::ShapeSettingsPanel *shapePanel_ = nullptr;
    jietuba::ArrowSettingsPanel *arrowPanel_ = nullptr;
    jietuba::TextSettingsPanel *textPanel_ = nullptr;
    jietuba::MosaicSettingsPanel *mosaicPanel_ = nullptr;
    jietuba::NumberSettingsPanel *numberPanel_ = nullptr;
    jietuba::WatermarkSettingsPanel *watermarkPanel_ = nullptr;
    QHash<QWidget *, QScrollArea *> panelContainers_;
    bool toolbarBelowSelection_ = true;
    jietuba::CanvasScene *scene_ = nullptr;
    QImage baseImage_;
    QPointer<QScreen> regionOutput_;
    QRect regionGeometry_, regionRequested_, regionFrame_;
    CaptureRouter *capture_ = nullptr;
    QPushButton *captureButton_ = nullptr;
    bool capturePending_ = false;
    bool overlay_ = false;
    bool layerOverlay_ = false, refreshAfterChild_ = false;
    std::function<void()> captureHandler_, finished_;
    std::function<void(bool)> saveDialogHandler_;
    std::function<void(const QImage &, QScreen *, const QRectF &)> pinHandler_;
    std::function<void(QScreen *, const QSize &, const QRect &, const QImage &, const QRect &)> longCaptureHandler_;
    std::function<void(QScreen *, const QSize &, const QRect &,const QRect &)> gifCaptureHandler_;
    QString watermarkText_;
    jietuba::WatermarkStyle watermarkStyle_;
    bool updatingWatermark_ = false;
    LinuxSettings *settings_ = nullptr;
};

LinuxAnnotationCanvas *LinuxAnnotationCanvas::createForPin(LinuxPinWindow *pin, const QImage &image,
                                                          LinuxSettings &settings, LinuxPinToolbar *host) {
    QImage pixels = image;
    pixels.setDevicePixelRatio(1);
    return new CapturePreview(pixels, &settings, pin, host);
}
LinuxAnnotationCanvas *LinuxAnnotationCanvas::createForRecording(QWidget *surface,QWidget *controls,const QSize &pixels,LinuxSettings &settings) {
    QImage transparent(pixels,QImage::Format_ARGB32_Premultiplied); transparent.fill(Qt::transparent);
    auto *canvas=new CapturePreview(transparent,&settings);
    canvas->configureRecording(surface,controls);
    return canvas;
}

LinuxAnnotationCanvas *LinuxAnnotationCanvas::createForScreenAnnotation(QWidget *surface,jietuba::Toolbar *toolbar,const QSize &pixels,LinuxSettings &settings) {
    QImage transparent(pixels,QImage::Format_ARGB32_Premultiplied); transparent.fill(Qt::transparent);
    auto *canvas=new CapturePreview(transparent,&settings,nullptr,nullptr,toolbar);
    canvas->configureRecording(surface,toolbar);
    return canvas;
}

/**
 * 一次截图会话的协调器：先在各屏幕显示透明选区层，松开鼠标后撤下所有选区层，
 * 然后只采集选中的输出并进入原版画布。开始框选时不采集、不显示冻结图片或整屏遮罩。
 * 使用实际返回的截图尺寸换算逻辑选区，避免多屏不同缩放污染导出坐标。新截图可暂存上层编辑器；
 * 录屏由独立会话持有，只协调控件显隐，不暂停采集。关闭内层后原选区、标注和撤销记录一起恢复。
 * Coordinates live selection first, then capture and annotation on the chosen output.
 * All transparent selectors unmap before capture, preserving existing pins in the result.
 * Logical selections map through actual capture dimensions for mixed-DPI outputs. Nested stills park the parent
 * document and undo history. Independent recorders keep capturing while their controls yield to the editor.
 */
class CaptureSession final : public QObject {
public:
    CaptureSession(QWidget *launcher, LinuxSettings &settings, bool trayAvailable, QObject *parent = nullptr)
        : QObject(parent), launcher_(launcher), settings_(settings), trayAvailable_(trayAvailable), router_(this) {
        connect(&router_, &CaptureRouter::frameReady, this, [this](const QImage &image) {
            if (!busy_ || !selectors_.isEmpty() || logicalSelection_.isEmpty() || index_ >= screens_.size()) return;
            const auto &screen = screens_[index_];
            if (!screen.screen || screen.screen->geometry() != screen.geometry) { fail(QStringLiteral("截图期间显示器配置发生变化，请重试。")); return; }
            // 防止后端返回错误的旋转方向或尺寸时，把坐标错误的图片交给用户。
            // Reject inconsistent aspect ratios instead of exporting misaligned selections.
            const double scale = image.devicePixelRatio();
            if (image.isNull() || std::abs(image.width()-logicalSelection_.width()*scale)>2.0
                || std::abs(image.height()-logicalSelection_.height()*scale)>2.0) {
                fail(QStringLiteral("显示器 %1 返回的区域截图尺寸与选区不匹配。").arg(screen.name)); return;
            }
            images_.append(image);
            ++index_;
            captureNext();
        });
        connect(&router_, &CaptureRouter::failed, this, [this](const QString &reason) { if (busy_) fail(reason); });
        setCaptureBindings(settings.globalShortcutSlots("capture"));
        qApp->installEventFilter(this);
    }
    ~CaptureSession() override {
        qApp->removeEventFilter(this);
        const auto recordings = recordings_;
        for (auto recording : recordings) if (recording) {
            recording->disconnect(this);
            delete recording.data();
        }
        delete quickTranslation_.data(); delete annotation_.data();
        for (auto *selector : selectors_) delete selector;
        if (longCapture_) { longCapture_->disconnect(this); delete longCapture_.data(); }
        qDeleteAll(overlays_);
        for (const auto &level : suspendedEditors_) { qDeleteAll(level.windows); qDeleteAll(level.selectors); }
    }
    void setCaptureBindings(const QStringList &keys) {
        captureKeys_.clear();
        for (const auto &key : keys) {
            const auto sequence = QKeySequence::fromString(key, QKeySequence::NativeText);
            if (sequence.count() == 1 && sequence[0].key() != Qt::Key_unknown) captureKeys_.append(sequence[0]);
        }
    }
    void setLongCaptureStateHandler(std::function<void(bool)> handler) { longCaptureStateHandler_ = std::move(handler); }
    void stopLongCapture() { if (longCapture_) longCapture_->stopAutomaticCapture(); }
    void bindRecordingTray(QSystemTrayIcon *tray, QAction *record, QAction *toggle, QAction *stop) {
        connect(record, &QAction::triggered, this, &CaptureSession::startRecording);
        connect(toggle, &QAction::triggered, this, &CaptureSession::toggleGif);
        connect(stop, &QAction::triggered, this, &CaptureSession::stopGif);
        gifStateHandler_ = [tray, record, toggle, stop](const QString &toggleText, const QString &stopText, bool running) {
            record->setVisible(!running);
            toggle->setText(toggleText); toggle->setVisible(!toggleText.isEmpty());
            stop->setText(stopText); stop->setVisible(!stopText.isEmpty());
            tray->setToolTip(running ? QStringLiteral("录屏中：可继续截图，或从托盘控制录制") : QStringLiteral("截图吧"));
        };
        refreshRecordingActions();
    }
    void startRecording() {
        if (!longCapture_) start(QStringLiteral("record"));
    }
    void toggleGif() { if (auto *recording = activeRecording()) recording->toggle(); }
    void stopGif() { if (auto *recording = activeRecording()) recording->stop(); }
    void globalShortcutActivated(const QString &action, const QString &binding) {
        if (action != "capture") {
            if (busy_ || !overlays_.isEmpty() || activeRecording() || longCapture_) return;
            if (action == "pin_clipboard") pinClipboard();
            else if(action=="translate") showTranslation();
            else if(action=="screen_annotation") toggleAnnotation();
            else if (action == "ocr") start(action);
            return;
        }
        start(QStringLiteral("capture"), binding);
    }
    void showTranslation() {
        if(!quickTranslation_) quickTranslation_=new LinuxQuickTranslationWindow(settings_);
        quickTranslation_->showNearCursor();
    }
    void toggleAnnotation() {
        if(annotation_) {annotation_->close(); return;}
        if(busy_ || !overlays_.isEmpty() || activeRecording() || longCapture_) return;
        auto *screen=QGuiApplication::screenAt(QCursor::pos()); if(!screen) screen=QGuiApplication::primaryScreen();
        if(!screen) return;
        annotation_=new LinuxScreenAnnotation(screen,settings_); annotation_->showPrepared();
    }
    void start(const QString &action = QStringLiteral("capture"), const QString &binding = {}) {
        if (auto *modal = QApplication::activeModalWidget()) { modal->raise(); modal->activateWindow(); return; }
        if (!busy_ && overlays_.isEmpty() && longCapture_ && !recordingInProgress()) {
            if (action == "capture") stopLongCapture();
            return;
        }
        if (busy_) {
            // 先找正在拖动的屏幕，否则用光标所在屏幕；已发出像素请求时只等待该次回复。
            // Prefer the output being dragged, then the pointer's output. Coalesce repeats during an in-flight grab.
            if (action == "capture" && !selectors_.isEmpty()) {
                auto *target = selectors_.first();
                const auto *pointerScreen = QGuiApplication::screenAt(QCursor::pos());
                for (auto *selector : selectors_) {
                    if (selector->isDragging()) { target = selector; break; }
                    if (selector->screen() == pointerScreen) target = selector;
                }
                if (target->isDragging()) {
                    for (auto *selector : selectors_) selector->setParked(true);
                    suspendedEditors_.append({{}, {}, std::exchange(selectors_, {}), screens_, captureAction_});
                    captureCurrentOutput(target->screen());
                } else { captureAction_ = action; target->captureCurrent(); }
            }
            return;
        }
        if (!overlays_.isEmpty()) {
            if (action != "capture" && action != "record") return;
            // 保留原选框、工具栏和标注作为真实桌面像素；只转交输入，不隐藏父窗口。
            // Keep the parent crop, toolbar and annotations in the desktop composition; transfer input only.
            const auto editors = overlays_;
            for (auto *editor : editors) {
                if (!editor->canSuspend()) return;
                if (!binding.isEmpty() && editor->triggerLocalBinding(binding)) return;
            }
            QScreen *dragScreen = nullptr;
            for (auto *editor : editors) if (editor->parkForCapture()) dragScreen = editor->screen();
            suspendedEditors_.append({std::exchange(overlays_, {}), {}, {}, {}, {}});
            if (dragScreen) { captureCurrentOutput(dragScreen); return; }
        }
        screens_.clear(); images_.clear(); index_ = 0;
        const auto selectedScreens = settings_.captureAllScreens() ? QGuiApplication::screens() : QList<QScreen *>{QGuiApplication::primaryScreen()};
        for (auto *screen : selectedScreens) {
            if (!screen || screen->geometry().isEmpty()) continue;
            screens_.append({screen, screen->name(), screen->geometry()});
        }
        if (screens_.isEmpty()) { fail(QStringLiteral("没有可用的显示器。")); return; }
        captureAction_ = action;
        logicalSelection_ = {};
        busy_ = true;
        syncRecordingWindows();
        // 设置窗口属于用户正在操作的桌面内容，截图时必须保留，不能自动隐藏或关闭。
        // Settings are part of the user's desktop and must stay visible while selecting/capturing.
        const auto generation = ++generation_;
        QTimer::singleShot(0, this, [this, generation] {
            if (busy_ && generation == generation_) beginSelection();
        });
    }
private:
    void captureCurrentOutput(QScreen *screen) {
        if (!screen) { fail(QStringLiteral("当前显示器已断开。")); return; }
        captureAction_ = "capture"; busy_ = true; index_ = 0; images_.clear();
        screens_ = {{screen, screen->name(), screen->geometry()}};
        logicalSelection_ = QRect(QPoint(), screen->size());
        syncRecordingWindows();
        const auto generation = ++generation_;
        QTimer::singleShot(60, this, [this, generation] { if (busy_ && generation == generation_) captureNext(); });
    }
    void syncEditorLevels() {
        // 最新文档在前，旧文档只显示；录制控件不属于这个截图栈。
        // Keep the newest screenshot in front. Recorder controls are outside this stack.
        for (auto *w : visualEditors_) w->setNativeOverlayLevel(overlays_.contains(w));
    }
    void syncRecordingWindows() {
        // 即使外层恰好编码完毕，进度条/预览也等当前截图结束再出现，避免抢走鼠标。
        // Even if an outer recording finishes encoding, its progress/preview must not steal input from a nested capture.
        for (const auto &recording : recordings_)
            if (recording) recording->setScreenshotActive(busy_ || !overlays_.isEmpty());
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) return false;
        auto *key = static_cast<QKeyEvent *>(event);
        if (!captureKeys_.contains(key->keyCombination()) || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
        auto *widget = qobject_cast<QWidget *>(watched);
        auto *surface = qobject_cast<QWindow *>(watched);
        const auto receivesKey = [&](QWidget *window) {
            return window && window->isVisible() && ((widget && widget->window() == window)
                || (surface && window->windowHandle() == surface));
        };
        if (!std::any_of(selectors_.cbegin(), selectors_.cend(), receivesKey)
            && !std::any_of(overlays_.cbegin(), overlays_.cend(), receivesKey)
            && !std::any_of(recordings_.cbegin(), recordings_.cend(), [&](const auto &r) { return r && receivesKey(r->controlWindow()); })) return false;
        key->accept();
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
            globalShortcutActivated(QStringLiteral("capture"), QKeySequence(key->keyCombination()).toString(QKeySequence::NativeText));
        return true;
    }
    bool recordingInProgress() const {
        return std::any_of(recordings_.cbegin(), recordings_.cend(), [](const auto &r) { return r && r->isCapturing(); });
    }
    LinuxGifCaptureWindow *activeRecording() const {
        // 托盘控制最近仍有动作的会话；内层结束后自动回到外层，预览不会抢占控制。
        // Tray actions target the newest controllable session, falling back past completed previews.
        const bool running = recordingInProgress();
        for (auto it = recordings_.crbegin(); it != recordings_.crend(); ++it)
            if (*it && (!running || (*it)->isCapturing()) && recordingActions_.contains(it->data())) return it->data();
        return nullptr;
    }
    void refreshRecordingActions() {
        const auto actions = recordingActions_.value(activeRecording());
        if (gifStateHandler_) gifStateHandler_(actions.first, actions.second, recordingInProgress());
    }
    bool pinClipboardContent() {
        const auto *mime = QApplication::clipboard()->mimeData();
        if (!mime) return false;
        QImage image = QApplication::clipboard()->image();
        QString contentText;
        if (image.isNull()) {
            QString text;
            if (mime->hasUrls()) {
                QStringList files;
                for (const auto &url : mime->urls()) files << url.toDisplayString(QUrl::PreferLocalFile);
                text = files.join('\n');
            } else text = mime->text();
            // 剪贴板 HTML 只作本地排版，不能为贴图读取外部资源。
            // Clipboard markup supplies formatting only; never resolve embedded resource URLs.
            class ClipboardPage final : public QTextDocument { QVariant loadResource(int,const QUrl &) override { return {}; } } page;
            page.setDocumentMargin(12);
            if (!mime->hasUrls() && mime->hasHtml()) {
                page.setHtml(mime->html());
                if (text.isEmpty()) text=page.toPlainText();
                if (!settings_.pinFlag("text_formatting")) page.setPlainText(text);
            } else page.setPlainText(text);
            if (text.isEmpty()) return false;
            contentText=text;
            page.setTextWidth(std::clamp(page.idealWidth(), 80.0, 720.0));
            const QSize size = page.size().toSize();
            if (size.height() > 16384) return false;
            image = QImage(size, QImage::Format_RGB32);
            if (image.isNull()) return false;
            image.fill(Qt::white);
            QPainter painter(&image);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, Qt::black);
            page.documentLayout()->draw(&painter, context);
        }
        QByteArray key;
        if(contentText.isEmpty()) {
            QImage pixels=image.convertToFormat(QImage::Format_RGBA8888);
            key=QByteArray::number(pixels.width())+":"+QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(pixels.constBits()),pixels.sizeInBytes()),QCryptographicHash::Sha256);
        } else key="text:"+QCryptographicHash::hash(contentText.toUtf8(),QCryptographicHash::Sha256);
        if(settings_.pinFlag("point_out_repeats")) for(auto *w:QApplication::allWidgets()) {
            auto *pin=qobject_cast<LinuxPinWindow *>(w); if(pin && pin->isVisible() && pin->property("clipboardIdentity").toByteArray()==key) {pin->pointOut();return true;}
        }
        if (settings_.captureFlag(LinuxSettings::CaptureFlag::SavePin) && LinuxCaptureDelivery::save(settings_,image,"贴图_").isEmpty()) QMessageBox::warning(launcher_,"贴图自动保存失败","无法写入保存目录。");
        auto *pin = new LinuxPinWindow(image, settings_);
        pin->setProperty("clipboardIdentity",key);
        pin->showOnScreen(QGuiApplication::screenAt(QCursor::pos()));
        return true;
    }
    void pinClipboard() {
        // 快捷键没有可贴内容时静默结束，不弹窗、不抢走其他应用的焦点。
        // A global paste attempt without usable content is a no-op and never steals focus.
        pinClipboardContent();
    }
    struct Screen { QPointer<QScreen> screen; QString name; QRect geometry; };
    void clearSelectors() {
        const auto closing = std::exchange(selectors_, {});
        for (auto *selector : closing) { selector->withdraw(); selector->deleteLater(); }
    }
    void cancelSelection() {
        if (!busy_) return;
        ++generation_;
        busy_ = false;
        clearSelectors();
        screens_.clear(); images_.clear();
        restoreEditor();
    }
    void beginSelection() {
        for (const auto &output : screens_) {
            if (!output.screen || output.screen->geometry() != output.geometry) { cancelSelection(); return; }
            auto *selector = new LinuxRegionSelector(output.screen);
            selector->configureAssist(settings_.captureFlag(LinuxSettings::CaptureFlag::Crosshair),settings_.smartSelectionMode(),settings_.captureFlag(LinuxSettings::CaptureFlag::SnapAnimation));
            selectors_.append(selector);
            connect(selector, &LinuxRegionSelector::cancelled, this, [this, selector] { if (selectors_.contains(selector)) cancelSelection(); });
            connect(selector, &LinuxRegionSelector::selected, this, [this, output, selector](const QRectF &region) {
                if (!busy_ || !selectors_.contains(selector)) return;
                if (!output.screen || output.screen->geometry() != output.geometry) { cancelSelection(); return; }
                logicalSelection_ = region.toAlignedRect().intersected(QRect(QPoint(),output.geometry.size()));
                screens_ = {output};
                clearSelectors();
                // 让透明边框与提示先提交撤场，再采集；保留桌面上原有贴图，不排除全部同进程窗口。
                // Let selector unmapping reach the compositor before capture, while keeping existing pins visible.
                const auto generation = generation_;
                QTimer::singleShot(60, this, [this, generation] {
                    if (busy_ && generation == generation_) captureNext();
                });
            });
        }
        for (auto *selector : selectors_) selector->showPrepared();
    }
    void captureNext() {
        // 用户已完成框选且透明层已撤下；此时才请求屏幕像素，保留原有贴图。
        // Request pixels only after live selection has finished and unmapped; include existing pins.
        if (index_ < screens_.size()) {
            const auto &output=screens_[index_];
            router_.captureRegion(output.name,logicalSelection_.toRect().translated(output.geometry.topLeft()),
                                  output.geometry,CallerWindowPolicy::Include, captureAction_ != "record" && settings_.captureFlag(LinuxSettings::CaptureFlag::Cursor));
            return;
        }
        // 每个顶层窗口明确绑定对应 QScreen；Wayland 合成器负责放置全屏窗口。
        // Bind each fullscreen surface to its output instead of guessing global positions.
        for (int i = 0; i < screens_.size(); ++i) {
            if (!screens_[i].screen || screens_[i].screen->geometry() != screens_[i].geometry) { fail(QStringLiteral("截图期间显示器配置发生变化，请重试。")); return; }
        }
        // 独立识别/翻译快捷键直接交付选区底图，不再短暂映射整屏编辑窗口。
        // OCR/translation consume the chosen crop directly, avoiding an intermediate fullscreen editor.
        if (captureAction_ == "ocr" || captureAction_ == "translate") {
            const QImage image = images_.first();
            const bool direct=captureAction_=="ocr" && settings_.captureFlag(LinuxSettings::CaptureFlag::CopyOcr);
            QWidget *result = captureAction_ == "ocr" ? static_cast<QWidget *>(new LinuxOcrResultWindow(image,nullptr,{},direct))
                : static_cast<QWidget *>(new LinuxTranslationResultWindow(image, settings_));
            if(captureAction_=="ocr") placeCaptureResult(result,screens_.first().screen,logicalSelection_.toRect(),settings_.ocrPosition());
            busy_ = false;
            screens_.clear(); images_.clear();
            syncRecordingWindows();
            if (!direct) result->show();
            return;
        }
        if (captureAction_ == "record") {
            const auto screen = screens_.first().screen;
            const auto outputSize = screens_.first().geometry.size();
            const auto &image = images_.first();
            const QSize pixels = (QSizeF(outputSize) * image.devicePixelRatio()).toSize();
            const QRect logical = logicalSelection_.toRect();
            const auto mapped = LinuxRegionSelector::nativeRegion(logical, outputSize, pixels);
            // 区域截图的实际像素尺寸用来校准录制区域，不构建截图编辑画布。
            // Calibrate the recording crop from the actual area frame without opening the screenshot editor.
            const QRect region = QRect(mapped.topLeft(), image.size()).intersected(QRect(QPoint(), pixels));
            busy_ = false;
            screens_.clear();
            images_.clear();
            syncRecordingWindows();
            prepareRecording(screen, pixels, region, logical);
            restoreEditor();
            return;
        }
        busy_ = false;
        for (int i = 0; i < screens_.size(); ++i) {
            auto *overlay = new CapturePreview(images_[i], &settings_);
            overlay->setOverlayMode([this, overlay] { finishEditor(overlay); });
            overlay->setSaveDialogHandler([this](bool open) { setSaveDialogOpen(open); });
            overlay->compositeChildren = [this, overlay](QImage &image, const QRectF &crop) {
                // 以输出逻辑坐标裁交集，转换回当前导出的原生像素；不合成自身边框，也不递归调用 result()。
                // Blend visible upper surfaces in output coordinates. Never bake the exporting editor's own chrome.
                if (crop.isEmpty()) return;
                QPainter painter(&image);
                for (int i = visualEditors_.indexOf(overlay) + 1; i < visualEditors_.size(); ++i) {
                    auto *above = visualEditors_[i];
                    if (!above->isVisible() || above->screen() != overlay->screen()) continue;
                    const QRect clip = crop.toAlignedRect().intersected(above->rect());
                    if (clip.isEmpty()) continue;
                    const auto surface = above->grab(clip);
                    const qreal sx = image.width()/crop.width(), sy = image.height()/crop.height();
                    painter.drawPixmap(QRectF((clip.x()-crop.x())*sx,(clip.y()-crop.y())*sy,clip.width()*sx,clip.height()*sy),
                                       surface,QRectF(QPointF(),surface.size()));
                }
            };
            overlay->runOnSelection(captureAction_);
            overlay->setPinHandler([this, overlay](const QImage &image, QScreen *screen, const QRectF &selection) {
                QPointer<QScreen> target(screen);
                auto *pin = new LinuxPinWindow(image, settings_);
                finishEditor(overlay);
                pin->showOnScreen(target, selection);
            });
            overlay->setLongCaptureHandler([this](QScreen *screen, const QSize &pixels, const QRect &region, const QImage &first, const QRect &logicalRegion) {
                if (longCapture_ || !screen || first.isNull()) return;
                auto *window = new LinuxScrollCaptureWindow(screen, pixels, region, first, {}, &settings_, logicalRegion);
                if (!window->hasOverlay() && !trayAvailable_) {
                    delete window;
                    QMessageBox::warning(launcher_, QStringLiteral("无法显示长截图控制栏"),
                        QStringLiteral("选区外没有可用空间或桌面不支持覆盖层，且系统托盘不可用。请缩小选区后重试。"));
                    return;
                }
                longCapture_ = window;
                // 先结束所有屏幕的选区，确保之后的屏幕帧中没有旧截图覆盖层。
                // Close every frozen overlay before resuming capture on the real desktop.
                finish(window);
                connect(window, &LinuxScrollCaptureWindow::captureStateChanged, this, [this](bool active) {
                    if (longCaptureStateHandler_) longCaptureStateHandler_(active);
                });
                connect(window, &QObject::destroyed, this, [this] {
                    if (longCaptureStateHandler_) longCaptureStateHandler_(false);
                    restoreEditor();
                });
                // 有浮动控制栏或托盘入口时直接开始；否则保留可见的结果控制窗口。
                // Start when an overlay or tray provides a reliable way back to the result.
                window->startAutomaticCapture();
            });
            overlay->setGifCaptureHandler([this](QScreen *screen, const QSize &pixels, const QRect &region,const QRect &logicalRegion) {
                prepareRecording(screen, pixels, region, logicalRegion);
            });
            overlay->configureRegionCapture(screens_[i].screen,logicalSelection_.toRect(),images_[i]);
            overlay->prepareOverlayOnScreen(screens_[i].screen);
            visualEditors_.append(overlay);
            overlays_.append(overlay);
        }
        // 所有输出均准备好再一起映射，避免构造第二块屏幕画布时第一块已经显示。
        // Prepare every output before mapping any overlay to reduce staggered multi-screen presentation.
        syncEditorLevels();
        for (auto *overlay : overlays_) overlay->showPreparedOverlay();
        screens_.clear(); images_.clear();
    }
    void prepareRecording(QScreen *screen, const QSize &pixels, const QRect &region, const QRect &logical) {
        if (longCapture_ || !screen) return;
        if (region.width() < 16 || region.height() < 16) {
            QMessageBox::warning(launcher_, QStringLiteral("无法录制"), QStringLiteral("选区至少需要 16 × 16 像素。"));
            return;
        }
        auto *recording = new LinuxGifCaptureWindow(screen, pixels, region, settings_, trayAvailable_, this, logical);
        recordings_.append(recording);
        connect(recording, &LinuxGifCaptureWindow::controlsChanged, this, [this, recording](const QString &toggle, const QString &stop) {
            if (toggle.isEmpty() && stop.isEmpty()) recordingActions_.remove(recording);
            else recordingActions_.insert(recording, {toggle, stop});
            refreshRecordingActions();
        });
        connect(recording, &QObject::destroyed, this, [this, recording] {
            recordingActions_.remove(recording);
            recordings_.removeIf([recording](const auto &entry) { return !entry || entry.data() == recording; });
            refreshRecordingActions();
            restoreEditor();
        });
        finish(recording);
        // 截图工具栏和托盘共用同一录制状态机；待选区退场后显示工具栏，由用户开始录制。
        // Both entry points use the same recorder. Unmap selection UI before showing its controls.
        QTimer::singleShot(150, recording, &LinuxGifCaptureWindow::showControls);
    }
    void finishEditor(CapturePreview *owner) {
        if (overlays_.contains(owner)) finish();
    }
    void setSaveDialogOpen(bool open) {
        // 普通文件窗口不能盖住独占的 layer-shell 表面。临时撤下所有实际可见的取景框，
        // 随后按原层级（父先、子后）映射，只向当前文档交还输入。已保存的文档不在列表中。
        // Unmap every visible capture surface for the modal. Remap surviving owners bottom-up, with just one input owner.
        saveDialogOpen_ = open;
        auto editor = [&](CapturePreview *w, bool active) {
            if (open && w->isVisible()) { dialogWindows_.append(w); w->setEditorInput(false); w->hide(); }
            else if (!open && dialogWindows_.contains(w)) { w->show(); w->setEditorInput(active); }
        };
        for (const auto &level : suspendedEditors_) for (auto *selector : level.selectors) {
            if (open && selector->isVisible()) { dialogWindows_.append(selector); selector->hide(); }
            else if (!open && dialogWindows_.contains(selector)) { selector->show(); selector->setParked(true); }
        }
        for (auto *w : visualEditors_) editor(w, overlays_.contains(w));
        if (!open) { dialogWindows_.clear(); restoreEditor(); }
    }
    void finish(QObject *continuation = nullptr) {
        if (overlays_.isEmpty()) return;
        const auto closing = std::exchange(overlays_, {});
        // 延迟销毁：回调可能来自某个画布自己的鼠标或键盘事件。
        // Defer destruction because this callback can originate in an overlay event.
        for (auto *overlay : closing) { visualEditors_.removeOne(overlay); overlay->withdrawOverlay(); overlay->deleteLater(); }
        // 若内层转去录屏/长截图，应在它结束后恢复上层，不能让旧编辑器挡住新控件。
        // When a nested editor hands off to recording or stitching, keep its parent parked until that UI closes.
        if (continuation && !suspendedEditors_.isEmpty() && !suspendedEditors_.last().continuation)
            suspendedEditors_.last().continuation = continuation;
        restoreEditor();
    }
    void restoreEditor() {
        if (saveDialogOpen_) return; // 等模态窗口销毁后再交还输入。 / Restore only after modal teardown.
        while (!busy_ && overlays_.isEmpty() && !suspendedEditors_.isEmpty() && !suspendedEditors_.last().continuation) {
            const auto previous = suspendedEditors_.takeLast();
            if (!previous.selectors.isEmpty()) {
                const bool valid = std::all_of(previous.screens.cbegin(), previous.screens.cend(), [](const Screen &s) {
                    return s.screen && s.screen->geometry() == s.geometry;
                });
                if (!valid) { qDeleteAll(previous.selectors); continue; }
                selectors_ = previous.selectors; screens_ = previous.screens; captureAction_ = previous.action;
                index_ = 0; images_.clear(); busy_ = true;
                for (auto *selector : selectors_) selector->setParked(false);
            }
            for (auto *editor : previous.windows) {
                if (editor->outputStillValid()) { overlays_.append(editor); syncEditorLevels(); editor->resumeAfterCapture(); }
                else { visualEditors_.removeOne(editor); editor->deleteLater(); }
            }
        }
        syncEditorLevels();
        syncRecordingWindows();
    }
    void fail(const QString &message) {
        ++generation_;
        clearSelectors();
        busy_ = false; screens_.clear(); images_.clear();
        QMessageBox::warning(launcher_, QStringLiteral("截图失败"), message);
        restoreEditor();
    }
    QWidget *launcher_;
    LinuxSettings &settings_;
    bool trayAvailable_ = false, busy_ = false;
    qsizetype index_ = 0;
    quint64 generation_ = 0;
    QRectF logicalSelection_;
    QList<LinuxRegionSelector *> selectors_;
    QList<Screen> screens_;
    QList<QImage> images_;
    QList<CapturePreview *> overlays_;
    // 新截图完成后，按创建顺序逆序恢复编辑器或尚未提交的拖框。
    // Resume the previous editor or unfinished selector in strictly last-in-first-out order.
    struct ParkedEditors {
        QList<CapturePreview *> windows;
        QPointer<QObject> continuation;
        QList<LinuxRegionSelector *> selectors;
        QList<Screen> screens;
        QString action;
    };
    QList<ParkedEditors> suspendedEditors_;
    QList<CapturePreview *> visualEditors_; // 底层到顶层 / Bottom to top
    bool saveDialogOpen_ = false;
    QList<QPointer<QWidget>> dialogWindows_;
    QList<QKeyCombination> captureKeys_;
    QPointer<LinuxQuickTranslationWindow> quickTranslation_;
    QPointer<LinuxScreenAnnotation> annotation_;
    QPointer<LinuxScrollCaptureWindow> longCapture_;
    QList<QPointer<LinuxGifCaptureWindow>> recordings_;
    QHash<LinuxGifCaptureWindow *, QPair<QString, QString>> recordingActions_;
    std::function<void(const QString &, const QString &, bool)> gifStateHandler_;
    std::function<void(bool)> longCaptureStateHandler_;
    QString captureAction_;
    CaptureRouter router_;
};

int main(int argc, char **argv) {
    // 原生工具窗口只创建自身表面，禁止 Qt 将同级画布也隐式提升为原生子窗口。
    // Native siblings paint outside their parent's opacity effect and keep independent input regions.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    // 启动时完成一次有超时的能力探测，不让首次按截图键承担探测延迟。
    // Warm the bounded capability cache at startup, outside the capture hot path.
    LinuxWayland::supportsLayerShell();
    // 与安装的 desktop 文件对应，供 KDE 正确显示应用身份。
    // Match the installed desktop entry for KDE application identity.
    app.setDesktopFileName(GlobalShortcut::applicationId());
    LinuxSettings settings;
    jietuba::I18nManager::load_language(settings.language());
    jietuba::get_ui_theme()->init(nullptr, &app);
    jietuba::get_ui_theme()->set_mode(settings.theme(), false);
    jietuba::get_theme().set_selection_border_width(settings.borderWidth());
    LinuxSettingsWindow window(settings);
    const bool hasTray = QSystemTrayIcon::isSystemTrayAvailable();
    app.setQuitOnLastWindowClosed(!hasTray);
    CaptureSession session(&window, settings, hasTray);
    QObject::connect(&window, &LinuxSettingsWindow::captureRequested, &session, [&session] { session.start(); });
    QSystemTrayIcon tray(QIcon(QStringLiteral(":/svg/托盘.svg")));
    QMenu trayMenu;
    const auto trayText = jietuba::make_tr("SystemTray");
    auto *captureAction = trayMenu.addAction(trayText("Screenshot"));
    auto *recordAction = trayMenu.addAction(trayText("Screen Recording"));
    auto *finishLongAction = trayMenu.addAction(QStringLiteral("完成长截图"));
    finishLongAction->setVisible(false);
    auto *gifToggleAction = trayMenu.addAction(QString()); gifToggleAction->setVisible(false);
    auto *gifStopAction = trayMenu.addAction(QString()); gifStopAction->setVisible(false);
    auto *translateAction=trayMenu.addAction(trayText("Translation"));
    auto *annotateAction=trayMenu.addAction(trayText("Screen Annotation"));
    auto *showAction = trayMenu.addAction(trayText("Settings"));
    trayMenu.addSeparator();
    auto *quitAction = trayMenu.addAction(trayText("Exit"));
    QObject::connect(captureAction, &QAction::triggered, &session, [&session] { session.start(); });
    QObject::connect(finishLongAction, &QAction::triggered, &session, [&session] { session.stopLongCapture(); });
    session.bindRecordingTray(&tray, recordAction, gifToggleAction, gifStopAction);
    session.setLongCaptureStateHandler([&tray, finishLongAction](bool active) {
        finishLongAction->setVisible(active);
        // 通知气泡本身可能覆盖截图区域，使用托盘提示文字而不在采集时弹出通知。
        // A notification could pollute the selected region; update the tray tooltip instead.
        tray.setToolTip(active ? QStringLiteral("长截图进行中：可从长截图工具栏或托盘完成") : QStringLiteral("截图吧"));
    });
    QObject::connect(translateAction,&QAction::triggered,&session,&CaptureSession::showTranslation);
    QObject::connect(annotateAction,&QAction::triggered,&session,&CaptureSession::toggleAnnotation);
    QObject::connect(showAction, &QAction::triggered, &window, &LinuxSettingsWindow::showShortcutsPage);
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);
    tray.setContextMenu(&trayMenu);
    tray.setToolTip(QStringLiteral("截图吧"));
    QObject::connect(&tray, &QSystemTrayIcon::activated, &session, [&session](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) session.start();
    });
    if (hasTray) tray.show();
    GlobalShortcut shortcut;
    window.setShortcut(&shortcut);
    QObject::connect(&shortcut, &GlobalShortcut::activated, &session, &CaptureSession::globalShortcutActivated);
    QObject::connect(&shortcut, &GlobalShortcut::bindingChanged, &session, [&session, &shortcut] {
        session.setCaptureBindings(shortcut.slotBindings(QStringLiteral("capture")));
    });
    QObject::connect(&shortcut, &GlobalShortcut::statusChanged, &tray, [&tray, hasTray](const QString &message) {
        if (hasTray) tray.showMessage(QStringLiteral("截图吧"), message, QSystemTrayIcon::Warning);
    });
    // 桌面可能要求用户配置或拒绝快捷键；无论结果如何托盘都保持可用。
    // Desktop shortcut setup may be denied; the tray remains usable in either case.
    QTimer::singleShot(0, &shortcut, [&shortcut] { shortcut.start(); });
    // 没有托盘时保留一个可访问的主窗口，避免后台启动后没有入口。
    // Keep a visible entry point on desktops without a tray.
    if (settings.showMainWindowOnStartup() || !hasTray) window.showCapturePage();
    return app.exec();
}
