#include "capture_router.h"
#include "kwin_capture.h"
#include "protocol_capture.h"
#include "x11_capture.h"
#include "../wayland_capture.h"
#include "portal_frame.h"
#include <QGuiApplication>
#include <QScreen>
#include <QtConcurrentRun>
#include <QDebug>

CaptureRouter::CaptureRouter(QObject *parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<CaptureResult>::finished, this, [this] {
        const auto result = watcher_.result();
        if (!cancelled_ || cancelled_->load()) return;
        if (!result.image.isNull()) {
            excludedCaller_=result.backend.startsWith("KWin") && nativeHide_;
            cancelled_.reset(); if (logSuccessfulCaptures_) qInfo().noquote() << "截图后端:" << result.backend; emit frameReady(result.image);
        } else {nativeErrors_=result.error; capturePortal();}
    });
}
CaptureRouter::~CaptureRouter() { stop(); watcher_.waitForFinished(); }
void CaptureRouter::stop() {
    if (cancelled_) cancelled_->store(true);
    portalPending_=portalActive_=false; if(portal_) portal_->stop();
}
void CaptureRouter::capture(const QString &outputName, CallerWindowPolicy policy,bool cursor) {
    start(outputName, policy, {}, {},cursor);
}
void CaptureRouter::captureRegion(const QString &outputName, const QRect &region, const QRect &outputGeometry,
                                  CallerWindowPolicy policy,bool cursor) {
    if (region.isEmpty() || !outputGeometry.contains(region)) { emit failed(QStringLiteral("区域截图坐标无效")); return; }
    start(outputName, policy, region, outputGeometry,cursor);
}
void CaptureRouter::start(const QString &outputName, CallerWindowPolicy policy, const QRect &region, const QRect &outputGeometry,bool cursor) {
    if (watcher_.isRunning() || portalPending_) return;
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    auto cancellation = cancelled_;
    requestedOutput_=outputName; requestedRegion_=region; requestedGeometry_=outputGeometry; requestedCursor_=cursor; nativeHide_=policy==CallerWindowPolicy::Hide;
    if(requestedGeometry_.isEmpty() && qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        for(auto *screen:QGuiApplication::screens()) if(screen->name()==outputName) {requestedGeometry_=screen->geometry(); break;}
    if(portalActive_ && portalOutput_==outputName && portalCursor_==cursor) {capturePortal(); return;}
    if(portal_) portal_->stop(); portalActive_=false;
    const bool kde = QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains("KDE", Qt::CaseInsensitive);
    const bool isX11 = X11Capture::available();
    watcher_.setFuture(QtConcurrent::run([outputName, cancellation, kde, isX11, policy, region, outputGeometry,cursor] {
        QStringList errors;
        auto attempt = [&](CaptureBackend &backend) {
            auto result = region.isEmpty() ? backend.capture(outputName, *cancellation)
                : backend.captureRegion(outputName, region, outputGeometry, *cancellation);
            if (result.image.isNull()) errors << result.backend + QStringLiteral(": ") + result.error;
            return result;
        };
        // X11 优先：直接捕获，无需授权，兼容性最好
        // X11 first: direct capture, no authorization needed, best compatibility
        if (isX11 && !cancellation->load()) {
            X11Capture backend(cursor);
            auto result = attempt(backend);
            if (!result.image.isNull()) return result;
        }
        // Wayland 协议按优先级尝试
        // Wayland protocols by priority
        if (kde && !cancellation->load()) {
            KWinCapture backend(policy,cursor);
            auto result = attempt(backend);
            if (!result.image.isNull()) return result;
        }
        if (!cancellation->load()) {
            ExtImageCopyCapture backend(cursor);
            auto result = attempt(backend);
            if (!result.image.isNull()) return result;
        }
        if (!cancellation->load()) {
            WlrScreencopyCapture backend(cursor);
            auto result = attempt(backend);
            if (!result.image.isNull()) return result;
        }
        return CaptureResult{{}, {}, QStringLiteral("没有可用的无交互截图后端：\n%1").arg(errors.join('\n'))};
    }));
}
void CaptureRouter::capturePortal() {
    if(!cancelled_ || cancelled_->load()) return;
    // 门户授权需要实际图形会话；纯后台协议测试/工具不能意外拉起桌面授权服务。
    // Portal authorization needs a GUI application; headless protocol clients must not spawn desktop dialogs.
    if(!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        cancelled_.reset(); emit failed(nativeErrors_+QStringLiteral("\n屏幕共享门户需要图形应用上下文。")); return;
    }
    if(!portal_) {
        portal_=std::make_unique<WaylandCapture>();
        connect(portal_.get(),&WaylandCapture::failed,this,&CaptureRouter::portalFailure);
        connect(portal_.get(),&WaylandCapture::cancelled,this,[this] {portalFailure(QStringLiteral("已取消屏幕共享授权。"));});
        connect(portal_.get(),&WaylandCapture::sessionEnded,this,[this] {
            portalActive_=false; if(portalPending_) portalFailure(QStringLiteral("屏幕共享会话已结束。"));
        });
        connect(portal_.get(),&WaylandCapture::frameReady,this,[this](const QImage &image) {
            if(!portalPending_ || !cancelled_ || cancelled_->load()) return;
            const bool multiple=qobject_cast<QGuiApplication *>(QCoreApplication::instance()) && QGuiApplication::screens().size()>1;
            QString error; const auto frame=linuxPortalFrame(image,portal_->streamProperties(),requestedGeometry_,requestedRegion_,multiple,&error);
            if(frame.isNull()) {portal_->stop(); portalFailure(error); return;}
            portalPending_=false; portalActive_=true; excludedCaller_=false; cancelled_.reset();
            if(logSuccessfulCaptures_) qInfo().noquote()<<"截图后端: XDG ScreenCast / PipeWire";
            emit frameReady(frame);
        });
    }
    // 标准门户由桌面选择实现，不能通过发行版名称猜测 GNOME/KDE/niri 的混合路由。
    // Let desktop portal routing select its implementation across mixed compositor environments.
    portalOutput_=requestedOutput_; portalCursor_=requestedCursor_; portalPending_=true;
    portal_->setCursorIncluded(requestedCursor_); portal_->capture();
}
void CaptureRouter::portalFailure(const QString &reason) {
    if(!portalPending_) return;
    portalPending_=portalActive_=false; cancelled_.reset();
    emit failed(nativeErrors_+QStringLiteral("\nXDG ScreenCast portal：")+reason);
}
