#pragma once
#include "capture_backend.h"
#include <QFutureWatcher>
#include <QObject>
#include <memory>
class WaylandCapture;

/**
 * 截图路由器：在工作线程按 KWin → ext-image-copy-capture → wlr-screencopy
 * 尝试独立后端；均不可用时通过标准 ScreenCast portal 请求用户授权，覆盖 GNOME 等桌面。
 * 每次只处理一个请求，销毁或取消时等待工作线程释放 Wayland 和 D-Bus 资源。
 *
 * Routes single-output captures through KWin, ext-image-copy-capture, then
 * wlr-screencopy on a worker thread, then the standard authorized ScreenCast portal.
 * Portal sessions reuse their PipeWire stream. Cancellation releases all native/session resources.
 */
class CaptureRouter final : public QObject {
    Q_OBJECT
public:
    explicit CaptureRouter(QObject *parent = nullptr);
    ~CaptureRouter() override;
    // 按请求传递而非保存全局开关，防止普通截图改变后续长截图/GIF 的过滤策略。
    // Pass policy per request so a still capture cannot change subsequent scroll/GIF filtering.
    void capture(const QString &outputName, CallerWindowPolicy policy = CallerWindowPolicy::Hide,bool cursor=false);
    void captureRegion(const QString &outputName, const QRect &globalLogicalRegion, const QRect &outputGeometry,
                       CallerWindowPolicy policy = CallerWindowPolicy::Include,bool cursor=false);
    void stop();
    void setLogSuccessfulCaptures(bool enabled) { logSuccessfulCaptures_ = enabled; }
    bool excludesCallerWindows() const {return excludedCaller_;}
signals:
    void frameReady(const QImage &image);
    void failed(const QString &reason);
private:
    void start(const QString &outputName, CallerWindowPolicy policy, const QRect &region, const QRect &outputGeometry,bool cursor);
    void capturePortal();
    void portalFailure(const QString &reason);
    bool logSuccessfulCaptures_ = true;
    QFutureWatcher<CaptureResult> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::unique_ptr<WaylandCapture> portal_;
    QString portalOutput_,requestedOutput_,nativeErrors_;
    QRect requestedRegion_,requestedGeometry_;
    bool portalActive_=false,portalPending_=false,requestedCursor_=false,portalCursor_=false,excludedCaller_=false;
    bool nativeHide_=false;
};
