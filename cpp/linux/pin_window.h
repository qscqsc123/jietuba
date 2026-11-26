#pragma once
#include <QImage>
#include <QCursor>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>
#include <QTimer>
#include <memory>
#include <optional>
class LinuxSettings;
class LinuxPinToolbar;
class LinuxAnnotationCanvas;
class QScreen;
class QGraphicsOpacityEffect;
class QPushButton;
class QLabel;
namespace jietuba { class PinControlButtons; }
#ifdef JIETUBA_HAS_LAYER_SHELL
namespace LayerShellQt { class Window; }
#endif

/**
 * 贴图缩放锚点：中央宽高各 1/2（面积 1/4）区域固定光标下的图像比例位置；
 * 外围四象限固定最近的两条边。一次连续滚轮序列共享锚点，防止缩放途中切换规则。
 *
 * The central quarter fixes the image point under the pointer. Outer quadrants
 * fix the nearest two edges. One wheel gesture keeps one anchor without switching modes mid-zoom.
 */
struct PinZoomAnchor {
    enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight, Pointer };
    Corner corner = Corner::TopLeft;
    QPointF outputPoint;
    QPointF ratio;
    static PinZoomAnchor fromPointer(const QPointF &topLeft, const QSize &size, const QPointF &pointer);
    QPointF topLeftFor(const QSize &size) const;
};

/**
 * Linux 贴图窗口：显示包含截图标注的最终图片，保持像素数据独立于截图画布。
 * 支持拖动、滚轮缩放、复制、保存及关闭；窗口关闭时自动销毁。
 * 支持 layer-shell 时以选区为初始位置；剪贴板贴图位于目标屏幕中央。
 * layer-shell 表面固定覆盖输出，只有贴图子控件移动/缩放，输入区域仅包含贴图。
 * 贴图与工具栏使用实际子控件坐标，中央以光标缩放，外围固定相应窗口角。
 *
 * A self-owned Linux pin with drag, wheel zoom, clipboard and save. On layer-shell
 * compositors it starts at the selection (or output center for clipboard pins) and shares output-local
 * coordinates with its toolbar. A fixed layer surface hosts a movable/resizable child, keeping native
 * configure delays out of pin geometry. Zoom fixes the pointer or nearest corner; xdg-shell falls back
 * to compositor-controlled placement and movement.
 */
class LinuxPinWindow final : public QWidget {
    Q_OBJECT
public:
    LinuxPinWindow(const QImage &image, LinuxSettings &settings, QWidget *parent = nullptr);
    ~LinuxPinWindow() override;
    void showOnScreen(QScreen *screen, const QRectF &selection = {});
    QImage image() const;
    void paintBorder(QPainter &painter) const;
    double zoom() const { return zoom_; }
    double pinOpacity() const { return opacity_; }
    // 工具栏独立表面的快捷键仍操作所属贴图，不分叉复制/缩放等行为。
    // The separate toolbar surface routes shortcuts back to its owning pin.
    bool triggerShortcut(const QString &action);
    // 使用固定原生表面上的鼠标绝对位置之差；不累加随子控件移动而改变的局部坐标。
    // Use pointer deltas on the fixed native surface, never accumulated moving-child local coordinates.
    bool beginDrag(const QPointF &globalPosition);
    void dragTo(const QPointF &globalPosition);
    void endDrag();
    void closeByKeyboard();
    void pointOut();
    static void refreshOpenAppearance(LinuxSettings &settings);
    bool beginEdgeResize(QPointF local,QPointF global);

    void setClickThrough(bool on);
    void applyPinWheel(QWheelEvent *event,QPointF pinPoint);

public slots:
    void copyImage();
    void saveImage();
    void toggleToolbar();
protected:
    bool eventFilter(QObject *watched,QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
private:
    bool lockBlocks(const QString &kind);
    Qt::Edges resizeEdgesAt(QPointF local) const;
    void updateResizeCursor(QWidget *target,Qt::Edges edges);
    void hint(const QString &message);
    void syncSurface();
    void applyAppearance();
    void setPinOpacity(double value);
    void startOcr(bool notify=false);
    PinZoomAnchor zoomAnchor(QPointF pointer) const;
    void stepZoom(double step,bool up,const PinZoomAnchor &anchor);
    void resizeForZoom();
    void updateLayerPosition();
    void changeZoom(double factor, const PinZoomAnchor &anchor);
    void resetZoom();
    QRectF toolbarPinRect() const;
    void syncToolbar();
    LinuxAnnotationCanvas *canvas_ = nullptr;
    QImage image_;
    LinuxSettings &settings_;
    double zoom_ = 1.0;
    double screenScale_ = 1.0;
    QSizeF baseSize_;
    QSize requestedSize_;
    QPointer<QScreen> outputScreen_;
    QPointer<QWidget> surface_; // 固定原生表面，贴图是其中的普通子控件 / Fixed native host; the pin is a regular child.
    QPointF layerPosition_;
    QPointF dragAnchor_;
    QPointF dragOrigin_;
    PinZoomAnchor wheelAnchor_;
    bool wheelAnchorValid_ = false;
    QElapsedTimer wheelGesture_;
    bool locked_=false, through_=false, rounded_=false, ocrStarted_=false, ocrPending_=false;
    double opacity_=1, previousOpacity_=1;
    QString ocrText_;
    QPointer<QWidget> throughBar_;
    QGraphicsOpacityEffect *opacityEffect_=nullptr;
    QPointer<QLabel> hintLabel_;
    Qt::Edges resizeEdges_;
    QPointer<QWidget> resizeCursorWidget_;
    QCursor resizeOriginalCursor_;
    QRectF resizeStart_;
    bool dragging_ = false;
    bool borderEnabled_ = true;
    QPointer<LinuxPinToolbar> toolbarHost_;
    std::unique_ptr<jietuba::PinControlButtons> controls_;
    bool pinHovered_=false, toolbarOpen_=false, syncingToolbar_=false;
    int wheelRemainder_=0;
    Qt::KeyboardModifiers wheelMode_=Qt::NoModifier;
#ifdef JIETUBA_HAS_LAYER_SHELL
    LayerShellQt::Window *layer_ = nullptr;
#endif
};
