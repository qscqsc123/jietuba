#pragma once
#include "gif_frames.h"
#include "recording_result_preview.h"
#include "file_manager.h"
#include "capture/capture_router.h"
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <functional>
#include <QMap>
#include "recording_audio.h"
class LinuxAnnotationCanvas;
class QComboBox;
class QDialog;
class QLabel;
class QPushButton;
class QMenu;
class QHBoxLayout;
class QScreen;
class QFrame;
class LinuxSettings;
namespace jietuba { class ClickMenuButton; }

/**
 * Linux GIF/MP4 控制条，使用原版录屏的圆角背景、SVG 和按钮尺寸。
 * 与 Windows RecordToolbar 一样按时间、格式、帧率、录制、绘图、撤销、拖动、关闭排列。
 * 共用视觉常量、ClickMenuButton、工具列表和时间按钮样式；绘图按钮操作共享原版画布。
 * 时间按钮负责倒计时菜单，业务状态由 LinuxGifCaptureWindow 单向更新。
 * MP4 额外显示声音、指针和共享视频设置表单；两种格式完成后共用图标操作条和静止首帧。
 * 可嵌入 layer-shell 覆盖层，也可在不支持该协议时用于录制前的普通窗口。
 * GIF/MP4 controls sharing the Windows rounded panel, SVGs and button metrics/order.
 * Shared metrics, ClickMenuButton, tool order and time-button style match Windows.
 * Drawing buttons drive the shared original canvas. The coordinator owns state; this widget
 * embeds in a layer-shell surface or a pre-recording fallback dialog.
 * MP4 adds audio/cursor/shared video settings; both formats share icon actions and a paused first frame.
 */
class LinuxGifToolbar final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxGifToolbar(LinuxSettings &settings, QWidget *parent = nullptr);
    int fps() const;
    int delay() const;
    void setState(const QString &time, bool recording, bool paused, bool busy);
    void setPopupBelow(bool below);
    void setTrayOnly(bool trayOnly);
    void setDrawingState(const QString &tool,bool canUndo,bool canRedo,bool available);
    QString recordingMode() const {return format_;}
    jietuba::RecordingOptions videoOptions() const {return videoOptions_;}
    void showRecordingResult(const QString &path);
    void showRecordingControls();
    void setResultPlaying(bool playing);
    void setResultMessage(const QString &text,const QString &detail={});
    void setResultBusy(bool busy);
    static QRect position(const QSize &screen, const QRect &selection, const QSize &toolbar);
signals:
    void startStopRequested();
    void pauseRequested();
    void closeRequested();
    void moveRequested(const QPoint &delta);
    void moveStarted();
    void moveFinished();
    void toolRequested(const QString &tool);
    void undoRequested();
    void redoRequested();
    void optionsChanged();
    void resultPlayRequested();
    void resultFolderRequested();
    void resultCopyRequested();
    void resultSaveRequested();
    void resultRerecordRequested();
protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void showDelayMenu();
    void refreshOptions();
    void showAudioMenu();
    void showVideoSettings();
    LinuxSettings &settings_;
    QPushButton *time_;
    jietuba::ClickMenuButton *fps_, *mode_;
    QPushButton *record_, *pause_;
    QLabel *moveHandle_;
    QMap<QString,QPushButton *> tools_;
    QPushButton *undo_=nullptr,*redo_=nullptr;
    QList<QWidget *> regionalControls_;
    QLabel *trayHint_ = nullptr;
    bool drawingAvailable_=false,busy_=false;
    QPointer<QMenu> delayMenu_;
    std::optional<QPoint> dragPosition_;
    int delay_ = 0;
    bool popupBelow_ = true;
    bool trayOnly_ = false;
    QString format_;
    jietuba::RecordingOptions videoOptions_;
    QPushButton *audio_=nullptr,*cursor_=nullptr,*videoSettings_=nullptr;
    QWidget *controlsContainer_=nullptr,*resultContainer_=nullptr;
    QLabel *resultLabel_=nullptr;
    QPushButton *resultPlay_=nullptr;
    QPushButton *resultSave_=nullptr;
    QHBoxLayout *controlsLayout_=nullptr;
    QPointer<QDialog> videoDialog_;
};

/**
 * 单输出、可移动的固定尺寸选区 GIF/MP4 状态机，对应原版 GifRecordWindow + FrameRecorder。
 * 选区保持输出本地原生像素坐标，屏幕缩放只影响浮动 UI，不影响导出尺寸。
 * 所有截图覆盖层先关闭，再抓取实时桌面；一次仅有一个采集请求和一个帧写盘。
 * 区域入口逐帧使用相同逻辑矩形，区域返回值不做二次裁剪；失败保留具体后端/尺寸诊断。
 * 移动选区时丢弃旧位置在途帧；标注场景保持原生像素坐标，移动只改变采集原点。
 * 画笔、形状、文字、橡皮擦、样式面板和撤销栈复用截图画布，只导出图层到录制帧。
 * 使用真实时间戳并扣除暂停时间；停止丢弃仍在途的采集，等待已接收的帧写完。
 * layer-shell 控件/边框严格在选区外；全屏或缺少协议时改用托盘控制，避免录入自己。
 * GIF/MP4 停止后均自动保存，中央偏下同时显示静止首帧和完成工具栏；复制/打开文件夹后关闭。
 * MP4 复用同一无损帧仓库，音频 EOS 和图像写盘均排空后编码 H.264/AAC；视频自动保存。
 * 现阶段两种格式均限制 60 秒、2 GiB/3600 帧缓存，避免长时间 PNG 录制耗尽磁盘。
 *
 * Coordinates fixed-region recording, analogous to Windows GifRecordWindow and
 * FrameRecorder. Native output-local pixels survive fractional scaling. Capture
 * and disk backpressure bound memory; real timestamps exclude pauses. In-flight
 * captures are discarded on stop and accepted writes drain before preview.
 * Area sessions reuse exact logical bounds without a second crop; failures preserve their original diagnostics.
 * Moving invalidates old-location requests while preserving annotation coordinates. Shared annotation tools,
 * panels and undo history render into frames without including editor handles or recording controls.
 * Layer-shell controls stay outside the crop; full-screen/unsupported desktops
 * use tray controls. Both formats save automatically and show the same saved-file
 * result toolbar and a paused first frame below screen center. Copy/folder actions close the session.
 * No Windows input hooks or capture APIs are used.
 * MP4 joins drained frame/audio jobs before H.264/AAC export and automatic save.
 * Both formats currently cap recording at 60 seconds and frame storage at 2 GiB/3600 frames.
 */
class LinuxGifCaptureWindow final : public QObject {
    Q_OBJECT
public:
    using CaptureRequest=std::function<void()>;
    LinuxGifCaptureWindow(QScreen *screen, const QSize &nativeSize, const QRect &region,
                          LinuxSettings &settings, bool trayAvailable, QObject *parent = nullptr,
                          const QRect &logicalRegion = {}, CaptureRequest request = {});
    ~LinuxGifCaptureWindow() override;
    void showControls();
    void toggle();
    void stop();
    void cancel();
    QWidget *controlWindow() const { return overlay_ ? overlay_.data() : fallback_.data(); }
    bool isCapturing() const { return state_ == State::Countdown || state_ == State::Recording || state_ == State::Paused; }
    void setScreenshotActive(bool active);
    // 移动的是固定尺寸录制区域；在途旧区域帧通过 epoch 丢弃，标注保留相对位置。
    // Move the fixed-size capture region, dropping in-flight old-location frames while retaining annotation coordinates.
    void beginRegionMove();
    void moveRegion(const QPoint &delta);
    void endRegionMove();
    void rerecord();
    QRect logicalCaptureRegion() const;
signals:
    // 托盘两个动作的文字使用实际录制格式；空文字隐藏对应动作。
    // Both tray labels use the active recording format; empty hides the action.
    void controlsChanged(const QString &toggleAction, const QString &stopAction);
    void frameCaptured(const QSize &nativeSize);
private slots:
    void acceptFrame(const QImage &image);
private:
    enum class State { Ready, Countdown, Recording, Paused, Finishing, Preview };
    void begin();
    void beginCapture();
    void capture();
    void updateControls();
    void updateControlWindowVisibility();
    void failCapture(const QString &reason);
    void synchronizeDrawing();
    void finishRecording();
    void showFinishingProgress(const QString &message, int percent);
    void hideFinishingProgress();
    void showRecordingResult(const QString &path);
    void toggleResultPreview();
    void createResultPreview();
    void positionRecordingControls();
    void exportTrimmedGif(bool copy);
    void copyRecording(const QString &path, bool includeFileUrl);
    void finishTrimExport(const QString &error);
    bool eventFilter(QObject *watched, QEvent *event) override;
    qint64 elapsed() const;
    QPointer<QScreen> screen_;
    QSize nativeSize_;
    QRect region_, screenGeometry_, logicalRegion_;
    CaptureRequest captureRequest_;
    QString captureError_; // 具体采集错误必须优先于“空录制”提示 / Preserve the actual capture failure over a generic empty-recording message.
    QString output_;
    LinuxSettings &settings_;
    bool trayAvailable_ = false, pending_ = false;
    State state_ = State::Ready;
    quint64 epoch_ = 0, requestEpoch_ = 0;
    QElapsedTimer clock_;
    qint64 pausedAt_ = 0, pausedTotal_ = 0, lastRequest_ = 0, requestTime_ = 0;
    int countdown_ = 0;
    QTimer frameTimer_, countdownTimer_, uiTimer_;
    CaptureRouter router_;
    LinuxGifFrames frames_;
    QPointer<QWidget> overlay_, fallback_;
    QPointer<QFrame> finishingProgress_;
    QPointer<LinuxRecordingResultPreview> preview_;
    LinuxGifToolbar *toolbar_ = nullptr;
    QPointer<LinuxAnnotationCanvas> drawing_;
    bool movingRegion_=false;
    bool screenshotActive_ = false;
    LinuxRecordingAudio audioRecorder_;
    LinuxFileManager fileManager_;
    bool recordingMp4_=false,framesReady_=false,audioReady_=false;
    QString audioPath_,resultPath_;
    std::unique_ptr<QTemporaryDir> clipboardExport_;
    bool exportingTrim_ = false;
    jietuba::RecordingOptions recordingVideoOptions_;
};
