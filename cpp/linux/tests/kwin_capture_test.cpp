#include "capture/kwin_capture.h"
#include "capture/capture_router.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrentRun>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <thread>

/**
 * 模拟 KWin ScreenShot2：异步向管道写入大于管道容量的像素，确保客户端在
 * 等待 D-Bus 回复的同时读取数据；还验证拒绝访问时的明确错误。
 * 用绿色像素模拟同进程贴图，检查普通截图保留它、连续采集仍过滤它，且每次请求策略独立。
 * Fake KWin ScreenShot2 writing more than a pipe buffer, testing concurrent reads
 * and explicit authorization errors without capturing the user's desktop.
 * Green pixels model a caller-owned pin: stills include it, streams filter it, and policies do not leak across requests.
 */
class FakeKWin : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.ScreenShot2")
public:
    int screenCalls=0, areaCalls=0;
public slots:
    QVariantMap CaptureScreen(const QString &screen, const QVariantMap &options, const QDBusUnixFileDescriptor &pipe) {
        ++screenCalls;
        if (screen == "denied") { sendErrorReply(QDBusError::AccessDenied, "test permission denied"); return {}; }
        if (!options.contains("hide-caller-windows") || options.value("hide-caller-windows").metaType() != QMetaType::fromType<bool>() ||
            !options.value("native-resolution").toBool() || options.value("include-cursor").toBool()) {
            sendErrorReply(QDBusError::InvalidArgs, "missing or invalid capture options"); return {};
        }
        const bool includePin = !options.value("hide-caller-windows").toBool();
        const int fd = dup(pipe.fileDescriptor());
        if (fd < 0) { sendErrorReply(QDBusError::Failed, "dup failed"); return {}; }
        // 一组 512×512 RGBA 像素超过普通管道容量；只有客户端持续读取才能写完。
        // This RGBA payload exceeds pipe capacity and requires the client to drain it.
        std::thread([fd, includePin] {
            QByteArray bytes(512 * 512 * 4, 0);
            for (int i = 0; i < bytes.size(); i += 4) { bytes[i] = char(255); bytes[i + 3] = char(255); }
            // 不隐藏调用方窗口时，合成器原图中已经有贴图；客户端无需再次绘制贴图。
            // Including caller windows puts the pin in the compositor frame; no client-side overlay is needed.
            if (includePin) for (int y = 100; y < 200; ++y) for (int x = 80; x < 180; ++x) {
                const int offset = (y * 512 + x) * 4;
                bytes[offset] = 0; bytes[offset + 1] = char(255);
            }
            qsizetype offset = 0;
            while (offset < bytes.size()) {
                const auto n = write(fd, bytes.constData() + offset, size_t(bytes.size() - offset));
                if (n > 0) offset += n;
                else if (n < 0 && (errno == EAGAIN || errno == EINTR)) { pollfd p{fd, POLLOUT, 0}; if (poll(&p, 1, 3000) <= 0) break; }
                else break;
            }
            close(fd);
        }).detach();
        return {{"type", "raw"}, {"width", uint(512)}, {"height", uint(512)}, {"stride", uint(2048)}, {"format", uint(QImage::Format_RGBA8888)}};
    }
    // 严格使用 KWin 正式 XML 的 iiuua{sv}h 签名，禁止用实现头文件的 int 宽高模拟总线。
    // Match KWin's wire XML exactly, so signed width/height fail here just as on the real desktop.
    QVariantMap CaptureArea(int x,int y,uint width,uint height,const QVariantMap &options,const QDBusUnixFileDescriptor &pipe) {
        ++areaCalls;
        // UI 集成测试会同时请求不同的选区；沿用同一条真实 FD 管道，仅桌面像素由夹具提供。
        // Concurrent UI tests use arbitrary crops and the real FD transport; only the desktop pixels are synthetic.
        const bool serveAreas = QCoreApplication::arguments().contains("--serve-areas");
        if (QCoreApplication::arguments().contains("--no-area")) {
            sendErrorReply(QDBusError::UnknownMethod,"CaptureArea is unavailable"); return {};
        }
        if (QCoreApplication::arguments().contains("--area-denied")) {
            sendErrorReply(QDBusError::AccessDenied,"area permission denied"); return {};
        }
        if (message().signature()!=QStringLiteral("iiuua{sv}h")
            || !width || !height || width>4096 || height>4096 || (!serveAreas && QRect(x,y,int(width),int(height))!=QRect(-700,40,200,120))
            || !options.value("native-resolution").toBool()
            || !options.contains("hide-caller-windows") || options.value("hide-caller-windows").metaType()!=QMetaType::fromType<bool>()
            || options.value("include-cursor").metaType()!=QMetaType::fromType<bool>()) {
            sendErrorReply(QDBusError::InvalidArgs,"incorrect native area request"); return {};
        }
        const int fd=dup(pipe.fileDescriptor());
        if (fd<0) { sendErrorReply(QDBusError::Failed,"dup failed"); return {}; }
        QImage image(serveAreas ? QSize(int(width),int(height)) : QSize(300,180),QImage::Format_RGBA8888);
        image.fill(options.value("hide-caller-windows").toBool() ? Qt::red : Qt::blue);
        if(options.value("include-cursor").toBool()) image.setPixelColor(image.width()-1,image.height()-1,Qt::yellow);
        QByteArray bytes(reinterpret_cast<const char *>(image.constBits()),image.sizeInBytes());
        std::thread([fd,bytes] {
            qsizetype offset=0;
            while (offset<bytes.size()) {
                const auto n=write(fd,bytes.constData()+offset,size_t(bytes.size()-offset));
                if (n>0) offset+=n;
                else if (n<0 && (errno==EAGAIN || errno==EINTR)) { pollfd p{fd,POLLOUT,0}; if (poll(&p,1,3000)<=0) break; }
                else break;
            }
            close(fd);
        }).detach();
        return {{"type","raw"},{"width",uint(image.width())},{"height",uint(image.height())},{"stride",uint(image.bytesPerLine())},
                {"format",uint(QImage::Format_RGBA8888)},{"scale",serveAreas ? 1.0 : 1.5}};
    }

};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // 仅连接测试启动器创建的私有总线，KDE 环境变量用于走真实路由器的 KWin 优先分支。
    // The test runner provides a private bus; KDE selects the real router's KWin-first branch.
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    // 拒绝访问分支仍会尝试协议后端，必须隔离真实 Wayland 会话，确保测试不会采集用户桌面。
    // Error paths also try protocol backends; isolate Wayland so tests cannot capture the real desktop.
    qputenv("WAYLAND_DISPLAY", "jietuba-kwin-test-no-compositor");
    FakeKWin server;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.kde.KWin.ScreenShot2") || !bus.registerObject("/org/kde/KWin/ScreenShot2", &server, QDBusConnection::ExportAllSlots)) return 1;
    if (app.arguments().contains("--serve-areas")) return app.exec();
    std::atomic_bool cancelled{false};
    QFutureWatcher<CaptureResult> watcher;
    CaptureRouter router;
    int routed = 0, screensBeforeArea=0;
    QObject::connect(&router, &CaptureRouter::failed, &app, [&](const QString &error) {
        if (app.arguments().contains("--area-denied") && routed==2 && server.areaCalls==1
            && server.screenCalls==screensBeforeArea && error.contains("area permission denied")) {
            app.exit(0); return;
        }
        std::fprintf(stderr, "router capture failed: %s\n", qPrintable(error)); app.exit(5);
    });
    QObject::connect(&router, &CaptureRouter::frameReady, &app, [&](const QImage &image) {
        if(routed==4 || routed==5) {
            // MP4 指针开关必须送到 KWin；后续截图不能继承上次的 true。
            // Pass the MP4 cursor flag to KWin without leaking it into the following default request.
            if(server.areaCalls!=routed-1 || server.screenCalls!=screensBeforeArea || image.size()!=QSize(300,180)
                || image.pixelColor(299,179)!=QColor(routed==4 ? Qt::yellow : Qt::red)) {app.exit(10); return;}
            if(routed==4) {routed=5; router.captureRegion("test-output",QRect(-700,40,200,120),QRect(-800,0,800,800),CallerWindowPolicy::Hide); return;}
            std::fprintf(stderr,"KWin native area, DPI, policy and per-request cursor passed\n"); app.exit(0); return;
        }
        if (routed==3) {
            // 连续区域采集隐藏控制层，且不退回整屏、重复裁剪或遗失 DPR。
            // Continuous area capture hides controls without switching to full-output capture or losing DPI.
            if (server.areaCalls!=2 || server.screenCalls!=screensBeforeArea || image.size()!=QSize(300,180)
                || image.devicePixelRatio()!=1.5 || image.pixelColor(299,179)!=QColor(Qt::red)) { app.exit(9); return; }
            routed=4;
            router.captureRegion("test-output",QRect(-700,40,200,120),QRect(-800,0,800,800),CallerWindowPolicy::Hide,true);
            return;
        }
        if (routed==2) {
            if (app.arguments().contains("--no-area")) {
                // 512×512 输出映射到 800×800 逻辑屏幕：外扩取整后区域为 128×78。
                // Preserve real pixels and caller-owned pins when cropping the fallback output.
                if (server.areaCalls!=1 || server.screenCalls!=screensBeforeArea+1 || image.size()!=QSize(128,78)
                    || image.devicePixelRatio()!=0.64 || image.pixelColor(0,0)!=QColor(Qt::red)
                    || image.pixelColor(36,76)!=QColor(Qt::green)) { app.exit(8); return; }
                app.exit(0); return;
            }
            if (server.areaCalls!=1 || server.screenCalls!=screensBeforeArea || image.size()!=QSize(300,180)
                || image.devicePixelRatio()!=1.5 || image.pixelColor(299,179)!=QColor(Qt::blue)) { app.exit(7); return; }
            routed=3;
            router.captureRegion("test-output",QRect(-700,40,200,120),QRect(-800,0,800,800),CallerWindowPolicy::Hide);
            return;
        }
        const bool includePin = routed++ == 0;
        if (image.size() != QSize(512, 512) || image.pixelColor(511, 511) != QColor(Qt::red) ||
            image.pixelColor(100, 150) != QColor(includePin ? Qt::green : Qt::red)) { app.exit(6); return; }
        if (includePin) {
            // 包含贴图后再次使用默认策略，保证长截图/GIF 不继承普通截图的设置。
            // A following default request must not inherit the still capture's Include policy.
            router.capture("test-output");
        } else {
            screensBeforeArea=server.screenCalls;
            router.captureRegion("test-output",QRect(-700,40,200,120),QRect(-800,0,800,800));
        }
    });
    int stage = 0;
    QObject::connect(&watcher, &QFutureWatcher<CaptureResult>::finished, &app, [&] {
        auto result = watcher.result();
        if (stage++ == 0) {
            if (!result.image.isNull() || !result.error.contains("permission denied")) { app.exit(2); return; }
            watcher.setFuture(QtConcurrent::run([&] { KWinCapture backend; return backend.capture("test-output", cancelled); }));
        } else {
            if (result.image.size() != QSize(512, 512) || result.image.pixelColor(511, 511) != QColor(Qt::red)) { app.exit(3); return; }
            router.capture("test-output", CallerWindowPolicy::Include);
        }
    });
    QTimer::singleShot(9000, &app, [&] { app.exit(4); });
    watcher.setFuture(QtConcurrent::run([&] { KWinCapture backend; return backend.capture("denied", cancelled); }));
    return app.exec();
}
#include "kwin_capture_test.moc"
