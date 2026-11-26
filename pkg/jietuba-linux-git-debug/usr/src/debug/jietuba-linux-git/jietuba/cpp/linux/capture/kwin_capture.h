#pragma once
#include "capture_backend.h"
#include <QVariant>

/**
 * KWin ScreenShot2 截图后端。KWin 使用调用方身份及桌面文件决定是否授权；
 * 本类请求指定屏幕或全局逻辑选区，并安全读取返回的管道，不更改系统权限或尝试绕过授权。
 *
 * Captures an output or native area with KWin ScreenShot2. Authorization is controlled by KWin;
 * this backend requests access normally and never changes or bypasses its policy.
 */
class KWinCapture final : public CaptureBackend {
public:
    explicit KWinCapture(CallerWindowPolicy policy = CallerWindowPolicy::Hide,bool cursor=false) : callerWindowPolicy_(policy),includeCursor_(cursor) {}
    CaptureResult capture(const QString &output, const std::atomic_bool &cancelled) override;
    CaptureResult captureRegion(const QString &output, const QRect &region, const QRect &outputGeometry,
                                const std::atomic_bool &cancelled) override;
private:
    CaptureResult request(const QString &method, QList<QVariant> arguments, const std::atomic_bool &cancelled);
    CallerWindowPolicy callerWindowPolicy_;
    bool includeCursor_=false;
};
