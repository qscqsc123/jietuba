#pragma once
#include "capture_backend.h"
#include <QString>
#include <QRect>

/**
 * X11 截图后端，使用 XGetImage + XFixes 光标 + Xrandr 多显示器支持。
 * 覆盖传统桌面环境（XFCE, MATE, Cinnamon 等）及所有运行在 Xorg 的桌面。
 *
 * X11 capture backend using XGetImage, XFixes for cursor, and Xrandr for multi-monitor.
 * Covers traditional desktop environments and all desktops running on Xorg.
 */
class X11Capture final : public CaptureBackend {
public:
    explicit X11Capture(bool cursor = false) : includeCursor_(cursor) {}
    
    CaptureResult capture(const QString &output, const std::atomic_bool &cancelled) override;
    CaptureResult captureRegion(const QString &output, const QRect &region, 
                               const QRect &outputGeometry, const std::atomic_bool &cancelled) override;
    
    static bool available();  // 检查是否运行在 X11 下
    
private:
    CaptureResult captureX11Region(const QRect &region, const std::atomic_bool &cancelled);
    bool includeCursor_;
};
