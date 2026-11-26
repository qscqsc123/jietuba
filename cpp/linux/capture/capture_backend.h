#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <atomic>
#include <cmath>

// 调用方窗口策略：普通截图包含贴图，连续采集默认隐藏本程序窗口以免录入控制条。
// Caller-window policy: still captures include pins; continuous capture hides our controls by default.
// 只有提供此选项的后端（目前为 KWin ScreenShot2）使用它，其余后端保留原有输出采集行为。
// Only supporting backends (currently KWin ScreenShot2) apply it; others keep their output capture behavior.
enum class CallerWindowPolicy { Hide, Include };

/**
 * 截图后端统一结果；失败时携带可显示的诊断，不把黑图当作成功。
 * Common screenshot result. Errors are explicit; a black frame is not a fallback.
 */
struct CaptureResult {
    QImage image;
    QString backend;
    QString error;
};

/**
 * 单显示器静态截图接口。每次请求在工作线程执行，调用方传入 Qt 屏幕名和
 * 原子取消标志。实现必须有超时，并在返回前释放 D-Bus / Wayland / FD 资源。
 *
 * Synchronous single-output backend executed on a worker thread. Implementations
 * must honor cancellation and timeouts and release protocol and FD resources.
 */
class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual CaptureResult capture(const QString &output, const std::atomic_bool &cancelled) = 0;
    // 默认兼容路径：协议只提供整输出时按实际像素尺寸裁剪，绝不把整屏图交给区域编辑器。
    // Backends without a native area request crop their output using actual pixel dimensions.
    virtual CaptureResult captureRegion(const QString &output, const QRect &region, const QRect &outputGeometry,
                                        const std::atomic_bool &cancelled) {
        if (region.isEmpty() || !outputGeometry.contains(region)) return {{}, {}, QStringLiteral("区域截图坐标无效")};
        auto result = capture(output, cancelled);
        if (result.image.isNull()) return result;
        const double sx = double(result.image.width()) / outputGeometry.width();
        const double sy = double(result.image.height()) / outputGeometry.height();
        const QRect local = region.translated(-outputGeometry.topLeft());
        const int x1 = int(std::floor(local.x() * sx + 1e-7)), y1 = int(std::floor(local.y() * sy + 1e-7));
        const int x2 = int(std::ceil((local.x() + local.width()) * sx - 1e-7));
        const int y2 = int(std::ceil((local.y() + local.height()) * sy - 1e-7));
        result.image = result.image.copy(QRect(x1, y1, x2-x1, y2-y1));
        result.image.setDevicePixelRatio(sx);
        return result;
    }
};
