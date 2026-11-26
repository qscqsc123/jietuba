#pragma once
#include <QObject>
#include <QTimer>
#include <QTemporaryDir>
#include <QElapsedTimer>
struct _GstElement;
struct _GstBus;
/**
 * Linux 系统声音录制：使用 PulseAudio 播放设备的 monitor，兼容 PipeWire-Pulse，不回退到麦克风。
 * GStreamer 管线暂停时停止取样，恢复后继续同一 WAV；EOS 异步排空并完成文件头。
 * 可注入 audiotestsrc 做隔离测试；不修改系统声音路由或启动外部音频服务。
 * Records the default playback monitor via PulseAudio/PipeWire-Pulse, never a microphone fallback.
 * Pipeline pause excludes paused samples; asynchronous EOS drains and finalizes WAV output.
 * An injectable test source isolates tests without changing audio routing or services.
 */
class LinuxRecordingAudio final : public QObject {
    Q_OBJECT
public:
    explicit LinuxRecordingAudio(QObject *parent=nullptr,const QString &sourceFactory=QStringLiteral("pulsesrc"));
    ~LinuxRecordingAudio() override;
    bool start();
    void setPaused(bool paused);
    void finish();
    void cancel();
signals:
    void finished(const QString &wav);
    void failed(const QString &reason);
private:
    void pollBus();
    void release();
    _GstElement *pipeline_=nullptr;
    _GstBus *bus_=nullptr;
    QTimer poll_;
    QTemporaryDir directory_;
    QString sourceFactory_,path_;
    bool finishing_=false;
    QElapsedTimer finishWait_;
};
