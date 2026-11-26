#pragma once
#include <QLabel>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <utility>
class QAudioOutput;
class QMediaPlayer;
class LinuxGifFrames;
namespace jietuba { class RangeSlider; }

/**
 * 无损帧显示控件：按显示器实际 DPR 适配可用空间，保持原始宽高比且不放大原帧。
 * 与 GIF/MP4 解码器分离，调整窗口只改变显示尺寸，不改变文件或解码后的原生像素。
 * Frame display fitted at the monitor DPR, preserving aspect ratio without upscaling.
 * Resizing affects presentation only, never the saved file or decoded native pixels.
 */
class LinuxGifPreviewLabel final : public QLabel {
public:
    explicit LinuxGifPreviewLabel(QWidget *parent = nullptr);
    void setFrame(const QImage &frame);
    static QPixmap fittedPixmap(const QImage &frame, const QSize &logicalSize, qreal dpr);
protected:
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;
private:
    void fit();
    QImage frame_;
};

/**
 * GIF/MP4 共用的录制结果预览，打开时不弹出保存窗口。
 * GIF 从磁盘帧仓库按需读取；MP4 使用 QMediaPlayer/QVideoSink 播放保存的文件。
 * GIF 使用原版双手柄，按所选范围和帧延时回放；MP4 保留普通进度条。
 * 打开时只解码并显示第一帧，保持暂停且不发声，播放必须由用户明确触发。
 * 视频帧转换到 QWidget 中绘制，避免额外原生视频窗口破坏 Wayland layer-shell 的层级。
 * 本类只负责播放，保存、复制、文件夹和重新录制仍由录制协调器管理。
 * Unified GIF/MP4 preview without an initial save dialog. Read GIF source frames on demand;
 * QMediaPlayer/QVideoSink plays the saved MP4 including recorded audio.
 * GIF uses the original two-handle timeline and plays only the selected range; MP4 keeps its seek bar.
 * Opening decodes the first frame while paused and silent; playback requires a user action.
 * QWidget rendering keeps video on the existing Wayland surface, without native video subwindows.
 * The recording coordinator retains ownership of saving, clipboard and re-record actions.
 */
class LinuxRecordingResultPreview final : public QWidget {
    Q_OBJECT
public:
    LinuxRecordingResultPreview(const QString &path, QWidget *parent, const LinuxGifFrames *gifFrames);
    ~LinuxRecordingResultPreview() override;
    void togglePlayback();
    void pausePlayback();
    bool isPlaying() const;
    std::pair<int, int> trimRange() const;
    QImage currentFrame() const { return currentFrame_; }
    struct ResultLayout {QRect preview; QRect toolbar;};
    static ResultLayout resultLayout(const QRect &available,const QSize &content,const QSize &toolbar);
signals:
    void playingChanged(bool playing);
    void framePresented(const QSize &nativeSize);
private:
    void seekGif(int frame);
    void advanceGif();
    void updateGifStatus();
    int gifFrameDelay() const;
    void present(const QImage &frame);
    void reportError(const QString &reason);
    LinuxGifPreviewLabel *image_;
    QLabel *status_;
    QLabel *trimStart_ = nullptr;
    QLabel *trimEnd_ = nullptr;
    jietuba::RangeSlider *trimSlider_ = nullptr;
    QTimer gifTimer_;
    const LinuxGifFrames *gifFrames_;
    int gifFrame_ = -1;
    QMediaPlayer *player_ = nullptr;
    QAudioOutput *audio_ = nullptr;
    QImage currentFrame_;
    bool failed_ = false;
};
