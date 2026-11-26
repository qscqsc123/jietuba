#pragma once
#include "capture_backend.h"

/**
 * ext-image-copy-capture-v1 截图后端：连接单独的 Wayland 客户端，查找当前
 * 输出和捕获协议，以共享内存读取一帧。协议未公布或权限被拒绝时返回错误。
 * Captures one output through ext-image-copy-capture using a private Wayland connection.
 */
class ExtImageCopyCapture final : public CaptureBackend {
public:
    explicit ExtImageCopyCapture(bool cursor=false):includeCursor_(cursor) {}
    CaptureResult capture(const QString &output, const std::atomic_bool &cancelled) override;
private:
    bool includeCursor_=false;
};

/**
 * wlr-screencopy 截图后端：作为兼容较旧 wlroots 合成器的最后一层回退，
 * 优先使用共享内存格式；不可用、超时或失败时明确报告，不返回虚假图片。
 * Legacy wlroots-compatible fallback using shared-memory capture and explicit errors.
 */
class WlrScreencopyCapture final : public CaptureBackend {
public:
    explicit WlrScreencopyCapture(bool cursor=false):includeCursor_(cursor) {}
    CaptureResult capture(const QString &output, const std::atomic_bool &cancelled) override;
private:
    bool includeCursor_=false;
};
