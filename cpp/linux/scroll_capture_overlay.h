#pragma once
#include <QImage>
#include <QRect>
#include <QWidget>

class QLabel;
class QScrollArea;
class QScreen;
namespace jietuba { class FloatingToolbar; }
class QGraphicsOpacityEffect;
class QPropertyAnimation;

/**
 * 长图预览：保持可读宽度，超长时跟随最新内容显示尾部，而非不断缩成细线。
 * 鼠标进入时淡化、离开后恢复，便于看清被预览覆盖的桌面内容。
 * Readable tail-following preview with an animated hover fade.
 */
class LinuxScrollPreview final : public QWidget {
public:
    explicit LinuxScrollPreview(QWidget *parent = nullptr);
    void setImage(const QImage &image, int frameHeight, bool horizontal = false, int position = -1);
    qreal previewOpacity() const;
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    QImage image_;
    int frameHeight_ = 0;
    int position_ = -1;
    bool horizontal_ = false;
    QGraphicsOpacityEffect *opacity_ = nullptr;
    QPropertyAnimation *fade_ = nullptr;
};

/**
 * 长截图浮动工具栏及选区外预览。边框、预览均避开采集区域，选区内部输入穿透。
 * 无足够空间或不支持 layer-shell 时由调用方保留托盘回退。
 * Floating actions and preview outside the crop; input inside the selection passes through.
 */
class LinuxScrollCaptureOverlay final : public QWidget {
    Q_OBJECT
public:
    static QRect selectionInLogicalPixels(const QSize &outputSize, const QSize &nativeSize, const QRect &nativeRegion);
    static QRect toolbarPosition(const QSize &outputSize, const QRect &selection);
    static QRect previewPosition(const QSize &outputSize, const QRect &selection, const QRect &toolbar);
    static LinuxScrollCaptureOverlay *create(QScreen *screen, const QSize &nativeSize, const QRect &region,
                                             const QRect &logicalRegion = {});
    void setPreview(const QImage &image, int frames, int frameHeight = 0, bool horizontal = false, int position = -1);
    void setDirection(bool horizontal);
    void setAutoScrolling(bool active);
    void setControlsEnabled(bool enabled);
    void setStatus(const QString &text);
signals:
    void directionRequested();
    void cropRequested(const QString &side);
    void autoScrollRequested();
    void interactionActive(bool active);
    void finishRequested();
    void cancelRequested();
    void saveRequested();
    void ocrRequested();
    void pinRequested();
private:
    LinuxScrollCaptureOverlay(QScreen *screen, const QRect &selection, const QRect &toolbar);
    void updateInputMask();
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    QLabel *notice_ = nullptr;
    QRect selection_;
    QRect toolbar_;
    QRect previewSlot_;
    QScrollArea *toolbarHost_ = nullptr;
    jietuba::FloatingToolbar *actions_ = nullptr;
    LinuxScrollPreview *thumbnail_ = nullptr;
};
