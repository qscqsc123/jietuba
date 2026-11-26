#pragma once
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>
#include "gif/video_settings.h"

/**
 * GIF/MP4 帧仓库与异步编码器，独立于桌面采集和窗口。
 * 对应 Windows 的 FrameStore/export_gif：帧以无损 PNG 暂存在私有临时目录，
 * 不把整段录制的原始像素堆在内存中。单个写盘任务提供背压，忙时拒收新帧；
 * 时间戳使用真实的有效录制时间，因此低帧率不会让导出的动画变快。
 * 停止时排空写盘任务；默认逐帧生成局部调色板，画面切换时不争抢整段的 256 色。
 * 可选全局调色板以减小体积；两种方式均流式编码，最后原子替换目标文件。
 * 不执行 shell，不访问屏幕；失败/取消时保留原帧以便重新保存。
 * MP4 共用时间戳、无损帧和原子保存；硬件编码失败自动用相同源帧回退软件 H.264。
 *
 * Disk-backed lossless frame store and asynchronous GIF encoder, analogous to the
 * Windows FrameStore/export_gif. One pending write bounds memory; timestamps retain
 * actual active recording time. Per-frame palettes prioritize fidelity; optional
 * global palettes reduce size. Both stream frames without buffering the entire
 * raw recording. Atomic destination writes preserve existing files on
 * failure. No shell or desktop access; export errors leave frames available to retry.
 * MP4 reuses timestamps, lossless frames and atomic saves, retrying software H.264 after hardware failure.
 */
class LinuxGifFrames final : public QObject {
    Q_OBJECT
public:
    enum class Quality { High, Compact };
    struct Frame { QString name; qint64 milliseconds; };
    explicit LinuxGifFrames(QObject *parent = nullptr);
    ~LinuxGifFrames() override;
    bool valid() const;
    bool append(const QImage &image, qint64 milliseconds);
    void finish(qint64 duration);
    bool exportGif(const QString &path, int first, int last, double speed = 1.0, double scale = 1.0, Quality quality = Quality::High);
    bool exportMp4(const QString &path,const jietuba::RecordingOptions &options,const QString &audioPath={});
    void cancelExport();
    bool reset();
    const QList<Frame> &frames() const { return frames_; }
    QImage image(int index) const;
    qint64 duration() const { return duration_; }
    bool exporting() const { return stage_ != Stage::Idle; }
    static QString ffmpegPath();
signals:
    void ready();
    void failed(const QString &reason);
    void exportProgress(const QString &message, int percent);
    void exported(const QString &path);
private:
    enum class Stage { Idle, Palette, Gif, Mp4, Copy, Cancelling };
    struct WriteResult { Frame frame; qint64 bytes = 0; bool ok = false; };
    void writeFinished();
    void processFinished(int code, QProcess::ExitStatus status);
    void exportFailed(const QString &reason);
    void encode();
    void readProgress();
    bool prepareExport(const QString &path,int first,int last,double speed,double scale);
    std::shared_ptr<QTemporaryDir> directory_;
    QFutureWatcher<WriteResult> writer_;
    QFutureWatcher<QString> copier_;
    QProcess process_;
    QList<Frame> frames_;
    QSize size_;
    bool writing_ = false, finishing_ = false, ready_ = false;
    qint64 duration_ = 0, bytes_ = 0;
    QString target_, diagnostic_;
    QString progressMessage_;
    QByteArray progressBuffer_;
    qint64 exportDurationUs_ = 0;
    int exportPercent_ = -1;
    Stage stage_ = Stage::Idle;
    double exportScale_ = 1;
    Quality quality_ = Quality::High;
    int finalDelay_ = 1;
    bool video_=false;
    jietuba::RecordingOptions videoOptions_;
    QString audioPath_,codec_,device_,resultFile_=QStringLiteral("result.gif");
};
