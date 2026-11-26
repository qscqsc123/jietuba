#pragma once
#include "capture/capture_router.h"
#include "scroll_stitch.h"
#include "scroll_auto_input.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QWidget>
#include <functional>

class QScreen;
class LinuxSettings;
class LinuxScrollCaptureOverlay;

/**
 * Wayland 长截图会话，无独立结果对话框。
 * 采集和拼接解耦：按序缓存有限数量的中间帧，拼接任务串行，完成操作等待队列排空。
 * 所有导出操作来自浮动工具栏；确认直接复制，保存取消后回到当前滚动会话。
 * 区域截图入口固定逻辑采集矩形，后续帧直接采集该区域；旧整屏入口保留独立裁剪路径。
 *
 * Bounded capture/stitch pipeline with ordered processing. Toolbar actions drain
 * accepted frames before exporting; no intermediate result dialog is opened.
 * Area sessions reuse their original logical capture rectangle; full-output sessions crop separately.
 */
class LinuxScrollCaptureWindow final : public QWidget {
    Q_OBJECT
public:
    using CaptureRequest = std::function<void()>;
    LinuxScrollCaptureWindow(QScreen *screen, const QSize &screenPixels, const QRect &pixelRegion,
                             const QImage &first, CaptureRequest request = {}, LinuxSettings *settings = nullptr,
                             const QRect &logicalRegion = {});
    ~LinuxScrollCaptureWindow() override;
    bool hasOverlay() const;
    void startAutomaticCapture();
    void stopAutomaticCapture();
    bool isCapturing() const { return capturing_; }
    bool isHorizontal() const { return horizontal_; }
    QImage resultImage() const; // 仅在拼接空闲时调用 / Call only when matching is idle.
public slots:
    void toggleDirection();
    void cropCurrentView(const QString &side);
    void toggleAutoScroll();
signals:
    void captureStateChanged(bool capturing);
    void frameProcessed(int frames, bool matched);
private slots:
    void acceptFrame(const QImage &image);
    void acceptRegionFrame(const QImage &image);
private:
    enum class Action { None, Copy, Save, Ocr, Pin, Direction, CropBefore, CropAfter, AutoStart };
    void finishWith(Action action);
    void completeAction();
    void requestFrame();
    void acceptCapturedFrame(const QImage &image, bool regionFrame);
    void scheduleFrame(int delay);
    void startNextMatch();
    void matchFinished();
    void setStatus(const QString &text);
    void refreshPreview();
    void copyResult();
    void saveResult();
    QImage orient(const QImage &image) const;
    void pauseInteraction(bool active);
    void stopAutoScroll();
    void nextAutoStep();
    void autoFrameUnchanged();
    void settlePreview();
    QPointer<LinuxScrollCaptureOverlay> overlay_;
    QPointer<QScreen> screen_;
    QString output_;
    QRect screenGeometry_;
    QSize screenPixels_;
    QRect region_;
    QRect logicalRegion_; // 输出内逻辑区域；空值表示旧整屏路径 / Output-local area; empty selects the legacy output path.
    LinuxScrollStitch stitch_;
    CaptureRouter router_;
    QFutureWatcher<LinuxScrollStitch::Result> matcher_;
    CaptureRequest captureRequest_;
    LinuxSettings *settings_ = nullptr;
    QTimer captureTimer_;
    QTimer autoTimer_;
    QTimer settleTimer_;
    LinuxScrollAutoInput autoInput_;
    QQueue<QImage> queue_;
    qint64 queuedBytes_ = 0;
    int frames_ = 1;
    bool pending_ = false;
    bool matching_ = false;
    bool capturing_ = false;
    Action action_ = Action::None;
    QImage submitted_, accepted_, matchingFrame_;
    bool settled_ = true;
    bool horizontal_ = false;
    bool autoMode_ = false;
    bool uiBlocked_ = false;
    bool discardPending_ = false;
    bool viewMatched_ = true;
    int autoIdleFrames_ = 0;
};
