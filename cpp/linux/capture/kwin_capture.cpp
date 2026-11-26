#include "kwin_capture.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>

CaptureResult KWinCapture::capture(const QString &output, const std::atomic_bool &cancelled) {
    const QString name = QStringLiteral("KWin ScreenShot2");
    if (output.isEmpty()) return {{}, name, QStringLiteral("没有可识别的屏幕名称")};
    return request(QStringLiteral("CaptureScreen"), {output}, cancelled);
}
CaptureResult KWinCapture::captureRegion(const QString &output, const QRect &region, const QRect &outputGeometry,
                                       const std::atomic_bool &cancelled) {
    if (region.isEmpty() || !outputGeometry.contains(region))
        return {{}, QStringLiteral("KWin ScreenShot2 CaptureArea"), QStringLiteral("区域截图坐标无效")};
    // 以正式 D-Bus XML 为准：x/y 是 int32（允许负坐标屏幕），宽/高必须是 uint32。
    // C++ 实现头文件中的 int 不能代表总线签名；iiii 会被真实 KWin 拒绝为 UnknownMethod。
    // Follow the wire XML: signed x/y, unsigned width/height, all in logical coordinates.
    // The C++ implementation's parameter types are not the bus contract: iiii does not match iiuu.
    auto result = request(QStringLiteral("CaptureArea"),
        {region.x(), region.y(), QVariant::fromValue(quint32(region.width())), QVariant::fromValue(quint32(region.height()))}, cancelled);
    if (!cancelled.load() && result.image.isNull()
        && result.error.startsWith(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod:"))) {
        // 只有原生区域方法不存在时才使用已工作的屏幕接口裁剪；权限拒绝或数据错误不可掩盖。
        // Fall back only for a missing method, never for authorization or image-data failures.
        result = CaptureBackend::captureRegion(output, region, outputGeometry, cancelled);
        result.backend += QStringLiteral(" (CaptureArea unavailable; cropped region)");
    }
    return result;
}
CaptureResult KWinCapture::request(const QString &method, QList<QVariant> arguments, const std::atomic_bool &cancelled) {
    const QString name = QStringLiteral("KWin ScreenShot2 ") + method;
    if (!QDBusConnection::sessionBus().isConnected()) return {{}, name, QStringLiteral("无法连接 D-Bus 会话")};
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0)
        return {{}, name, QStringLiteral("创建截图管道失败: %1").arg(QString::fromLocal8Bit(std::strerror(errno)))};
    const int readFd = descriptors[0];
    // Qt 的 D-Bus 描述符对象复制 FD；原始写端必须及时关闭，避免读取端永远等不到 EOF。
    // Qt duplicates the descriptor. Close our original write end to avoid holding the pipe open.
    QDBusUnixFileDescriptor writer(descriptors[1]);
    close(descriptors[1]);
    if (!writer.isValid()) { close(readFd); return {{}, name, QStringLiteral("无法复制截图管道")}; }
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin.ScreenShot2"),
        QStringLiteral("/org/kde/KWin/ScreenShot2"), QStringLiteral("org.kde.KWin.ScreenShot2"), method);
    // ScreenShot2 v5 默认排除同进程的所有窗口，贴图也属于本进程，不能依赖默认值。
    // 普通截图显式包含贴图，由合成器处理缩放、遮挡和边框；连续采集仍隐藏录制控制条。
    // ScreenShot2 v5 excludes every caller-owned window by default, including pins.
    // Include them for stills so KWin handles scaling/occlusion/borders; keep controls hidden for streams.
    // 旧版 KWin 忽略不认识的选项，继续按其原有行为采集。
    // Older KWin versions ignore unknown options and retain their existing capture behavior.
    arguments << QVariantMap{{QStringLiteral("native-resolution"), true}, {QStringLiteral("include-cursor"), includeCursor_},
                            {QStringLiteral("hide-caller-windows"), callerWindowPolicy_ == CallerWindowPolicy::Hide}}
              << QVariant::fromValue(writer);
    message.setArguments(arguments);
    auto pending = QDBusConnection::sessionBus().asyncCall(message, 5000);
    // 撤销本地副本，KWin 接收其自己的 FD；读取与 D-Bus 响应并行，避免管道写满死锁。
    // Drop local copies and drain the pipe concurrently with D-Bus to avoid a full-pipe deadlock.
    message = QDBusMessage();
    arguments.clear();
    writer = QDBusUnixFileDescriptor();
    QDBusPendingCallWatcher watcher(pending);
    QEventLoop loop;
    QSocketNotifier notifier(readFd, QSocketNotifier::Read);
    QTimer timer;
    timer.setInterval(50);
    QByteArray bytes;
    QVariantMap metadata;
    QString error;
    bool replyReceived = false, eof = false;
    qint64 needed = -1;
    constexpr qint64 maxBytes = 512LL * 1024 * 1024;
    auto checkDone = [&] {
        if (!error.isEmpty() || (replyReceived && needed >= 0 && bytes.size() >= needed) || (eof && replyReceived)) loop.quit();
    };
    QObject::connect(&notifier, &QSocketNotifier::activated, &loop, [&] {
        char chunk[65536];
        for (;;) {
            const auto n = read(readFd, chunk, sizeof(chunk));
            if (n > 0) {
                if (bytes.size() + n > maxBytes) { error = QStringLiteral("截图数据超过大小限制"); break; }
                bytes.append(chunk, n);
            } else if (n == 0) { eof = true; notifier.setEnabled(false); break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { error = QStringLiteral("读取截图管道失败"); break; }
        }
        checkDone();
    });
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, [&](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QVariantMap> reply = *call;
        replyReceived = true;
        if (reply.isError()) error = reply.error().name() + QStringLiteral(": ") + reply.error().message();
        else {
            metadata = reply.value();
            const auto width = metadata.value("width").toULongLong(), height = metadata.value("height").toULongLong();
            const auto stride = metadata.value("stride").toULongLong();
            if (metadata.value("type").toString() != "raw" || !width || !height || width > 32768 || height > 32768 ||
                !stride || stride > quint64(maxBytes) || height > quint64(maxBytes) / stride)
                error = QStringLiteral("KWin 返回无效的图片尺寸或行跨度");
            else needed = qint64(stride * height);
        }
        checkDone();
    });
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (cancelled.load() || ++ticks >= 120) { error = cancelled.load() ? QStringLiteral("已取消") : QStringLiteral("KWin 截图超时"); loop.quit(); }
    });
    timer.start();
    loop.exec();
    notifier.setEnabled(false);
    close(readFd);
    if (!error.isEmpty()) return {{}, name, error};
    if (needed < 0 || bytes.size() < needed) return {{}, name, QStringLiteral("KWin 返回的图片数据不完整")};
    const int width = metadata.value("width").toInt(), height = metadata.value("height").toInt(), stride = metadata.value("stride").toInt();
    const auto format = static_cast<QImage::Format>(metadata.value("format").toInt());
    // 不信任外部元数据：限制支持的格式并验证一行实际容量。
    // Validate untrusted metadata and row capacity before constructing an image.
    switch (format) {
    case QImage::Format_RGB32: case QImage::Format_ARGB32: case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888: case QImage::Format_RGBA8888: case QImage::Format_RGBA8888_Premultiplied:
        if (qint64(stride) < qint64(width) * 4) return {{}, name, QStringLiteral("KWin 图片行跨度无效")};
        break;
    default: return {{}, name, QStringLiteral("KWin 返回暂不支持的图片格式 %1").arg(int(format))};
    }
    const QImage image(reinterpret_cast<const uchar *>(bytes.constData()), width, height, stride, format);
    if (image.isNull()) return {{}, name, QStringLiteral("KWin 图片无法解码")};
    QImage result = image.copy();
    if (method == QStringLiteral("CaptureArea")) {
        const double scale = metadata.value(QStringLiteral("scale"), 1.0).toDouble();
        if (!std::isfinite(scale) || scale <= 0 || scale > 16) return {{}, name, QStringLiteral("KWin 返回无效缩放比例")};
        result.setDevicePixelRatio(scale);
    }
    return {result, name, {}};
}
