#pragma once

#include <QDBusUnixFileDescriptor>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <gst/app/gstappsink.h>

/**
 * Wayland 单屏采集会话 / Wayland single-monitor capture session.
 *
 * 通过 XDG ScreenCast portal 依次创建会话、请求用户选屏，并取得只允许访问
 * 已授权画面的 PipeWire 文件描述符；GStreamer 负责将视频帧转换为 QImage。
 * 已建立的会话保留到用户撤销授权、发生错误或对象销毁，可供多次截图复用。
 *
 * Creates an XDG ScreenCast session, lets the user choose a monitor, and consumes
 * the authorized PipeWire stream through GStreamer. A live session is reused for
 * successive still frames. All public calls and signals run on the Qt GUI thread;
 * the GStreamer streaming threads never access QWidget or emit frame callbacks.
 *
 * 当前只选一个显示器；图像尺寸是视频帧的实际像素，不推断全局桌面坐标。
 * Only one monitor is selected. Frame sizes are pixel sizes, not desktop coordinates.
 */
class WaylandCapture final : public QObject {
    Q_OBJECT
public:
    explicit WaylandCapture(QObject *parent = nullptr);
    ~WaylandCapture() override;

    // 建立或复用会话并取得一张新帧；不能同时发起多个请求。
    // Start/reuse a session and capture a fresh frame; concurrent calls are ignored.
    void capture();
    void stop();
    // 供跨桌面路由核对用户授权的显示器，防止多屏时把另一块屏幕误当成目标输出。
    // Expose portal monitor metadata so callers can reject a mismatched authorized output.
    QVariantMap streamProperties() const {return streamProperties_;}
    void setCursorIncluded(bool included) {if(includeCursor_!=included) {stop(); includeCursor_=included;}}

signals:
    void frameReady(const QImage &image);
    void cancelled();
    void failed(const QString &reason);
    void sessionEnded();
    void sessionReady();

private slots:
    void portalResponse(uint response, const QVariantMap &results);
    void portalSessionClosed(const QVariantMap &details);
    void pollStream();

private:
    enum class Phase { Idle, Creating, Selecting, Starting, Opening, Ready };
    void request(const QString &method, QList<QVariant> arguments, QVariantMap options);
    void openRemote();
    bool startPipeline(QString *error);
    void requestFrame();
    void fail(const QString &error);
    void clearRequest();
    void closeSession();
    void stopPipeline();

    Phase phase_ = Phase::Idle;
    QString requestPath_;
    QString sessionPath_;
    uint nodeId_ = 0;
    qulonglong serial_ = 0;
    QDBusUnixFileDescriptor remoteFd_;
    GstElement *pipeline_ = nullptr;
    GstAppSink *sink_ = nullptr; // 管线拥有；/ Owned by the pipeline.
    QTimer pollTimer_;
    QTimer requestTimeout_;
    QTimer frameTimeout_;
    bool awaitingFrame_ = false;
    bool includeCursor_=false;
    QVariantMap streamProperties_;
    // 会话代数用于丢弃关闭后才到达的异步回复。
    // Generation invalidates asynchronous replies after a session is stopped.
    quint64 generation_ = 0;
};
