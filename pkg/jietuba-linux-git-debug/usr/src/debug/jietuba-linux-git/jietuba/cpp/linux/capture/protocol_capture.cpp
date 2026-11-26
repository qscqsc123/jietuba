#include "protocol_capture.h"
#include "ext-image-copy-capture-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"
#include <wayland-client.h>
#include <QDeadlineTimer>
#include <QImage>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
struct Output { wl_output *object = nullptr; QString name; };

/**
 * 工作线程独占的 Wayland 连接。独立连接避免与 Qt 事件队列竞争；所有等待
 * 都有期限并定期检查取消。共享内存和 Wayland 对象在析构时统一释放。
 *
 * Worker-owned Wayland connection. It never dispatches Qt's connection and all
 * waits are bounded. Shared memory and protocol resources are released on exit.
 */
struct ProtocolContext {
    explicit ProtocolContext(const std::atomic_bool &flag) : cancelled(flag) {}
    ~ProtocolContext() {
        if (buffer) wl_buffer_destroy(buffer);
        if (mapped != MAP_FAILED) munmap(mapped, mappedSize);
        if (fd >= 0) close(fd);
        if (display) wl_display_disconnect(display);
    }
    const std::atomic_bool &cancelled;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    ext_image_copy_capture_manager_v1 *copyManager = nullptr;
    ext_output_image_capture_source_manager_v1 *sourceManager = nullptr;
    zwlr_screencopy_manager_v1 *wlrManager = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;
    wl_buffer *buffer = nullptr;
    int fd = -1;
    void *mapped = MAP_FAILED;
    size_t mappedSize = 0;
    uint32_t width = 0, height = 0, stride = 0, format = UINT32_MAX;
    uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL, flags = 0;
    bool syncDone = false, constraintsDone = false, complete = false, failed = false;
    QString error;

    bool wait(const std::function<bool()> &condition) {
        QDeadlineTimer deadline(5000);
        while (!condition() && !failed && !cancelled.load() && !deadline.hasExpired()) {
            if (wl_display_dispatch_pending(display) < 0 || (wl_display_flush(display) < 0 && errno != EAGAIN)) { error = QStringLiteral("Wayland 连接中断"); return false; }
            pollfd pfd{wl_display_get_fd(display), POLLIN, 0};
            const int result = poll(&pfd, 1, 100);
            if (result < 0 && errno != EINTR) { error = QStringLiteral("等待 Wayland 事件失败"); return false; }
            if (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { error = QStringLiteral("Wayland 连接已关闭"); return false; }
            if (result > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(display) < 0) { error = QStringLiteral("Wayland 协议错误"); return false; }
        }
        if (cancelled.load()) error = QStringLiteral("已取消");
        else if (deadline.hasExpired() && !condition()) error = QStringLiteral("Wayland 截图超时");
        else if (failed && error.isEmpty()) error = QStringLiteral("合成器拒绝或无法完成截图");
        return condition() && !failed && !cancelled.load();
    }
    bool roundtrip() {
        syncDone = false;
        auto *callback = wl_display_sync(display);
        if (!callback) { error = QStringLiteral("无法同步 Wayland 状态"); return false; }
        static const wl_callback_listener listener{[](void *data, wl_callback *cb, uint32_t) {
            static_cast<ProtocolContext *>(data)->syncDone = true; wl_callback_destroy(cb);
        }};
        wl_callback_add_listener(callback, &listener, this);
        const bool ok = wait([&] { return syncDone; });
        if (!syncDone) wl_callback_destroy(callback);
        return ok;
    }
    bool createBuffer() {
        // 仅接受可安全映射的 32 位共享内存格式；无法映射时交给下一后端。
        // Accept only known 32-bit shared-memory formats; otherwise try the next backend.
        if (!shm || !width || !height || width > 32768 || height > 32768 || format == UINT32_MAX ||
            stride < uint64_t(width) * 4 || uint64_t(stride) * height > 512ULL * 1024 * 1024) {
            error = QStringLiteral("合成器没有提供受支持的共享内存格式或尺寸"); return false;
        }
        mappedSize = size_t(stride) * height;
        fd = memfd_create("jietuba-capture", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(mappedSize)) != 0) { error = QStringLiteral("无法创建共享内存"); return false; }
        mapped = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) { error = QStringLiteral("无法映射共享内存"); return false; }
        auto *pool = wl_shm_create_pool(shm, fd, static_cast<int>(mappedSize));
        if (!pool) { error = QStringLiteral("无法创建 Wayland 内存池"); return false; }
        buffer = wl_shm_pool_create_buffer(pool, 0, int(width), int(height), int(stride), format);
        wl_shm_pool_destroy(pool);
        if (!buffer) { error = QStringLiteral("无法创建 Wayland 图片缓冲区"); return false; }
        return true;
    }
    QImage image() const {
        QImage::Format qtFormat;
        switch (format) {
        case WL_SHM_FORMAT_XRGB8888: qtFormat = QImage::Format_RGB32; break;
        case WL_SHM_FORMAT_ARGB8888: qtFormat = QImage::Format_ARGB32_Premultiplied; break;
        case WL_SHM_FORMAT_XBGR8888: qtFormat = QImage::Format_RGBX8888; break;
        case WL_SHM_FORMAT_ABGR8888: qtFormat = QImage::Format_RGBA8888_Premultiplied; break;
        default: return {};
        }
        QImage copy(static_cast<const uchar *>(mapped), int(width), int(height), int(stride), qtFormat);
        return copy.copy();
    }
};

bool supported(uint32_t f) { return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888; }

const wl_output_listener outputListener{
    [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {},
    [](void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {},
    [](void *, wl_output *) {}, [](void *, wl_output *, int32_t) {},
    [](void *data, wl_output *, const char *name) { static_cast<Output *>(data)->name = QString::fromUtf8(name); },
    [](void *, wl_output *, const char *) {}
};
const wl_registry_listener registryListener{
    [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto *c = static_cast<ProtocolContext *>(data);
        if (std::strcmp(interface, wl_shm_interface.name) == 0)
            c->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        else if (std::strcmp(interface, wl_output_interface.name) == 0) {
            auto output = std::make_unique<Output>();
            output->object = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
            wl_output_add_listener(output->object, &outputListener, output.get());
            c->outputs.push_back(std::move(output));
        } else if (std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
            c->copyManager = static_cast<ext_image_copy_capture_manager_v1 *>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
        else if (std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
            c->sourceManager = static_cast<ext_output_image_capture_source_manager_v1 *>(wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        else if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
            c->wlrManager = static_cast<zwlr_screencopy_manager_v1 *>(wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
    }, [](void *, wl_registry *, uint32_t) {}
};

wl_output *connectAndFindOutput(ProtocolContext &c, const QString &requested) {
    c.display = wl_display_connect(nullptr);
    if (!c.display) { c.error = QStringLiteral("无法连接 Wayland 合成器"); return nullptr; }
    c.registry = wl_display_get_registry(c.display);
    if (!c.registry) { c.error = QStringLiteral("无法读取 Wayland 协议列表"); return nullptr; }
    wl_registry_add_listener(c.registry, &registryListener, &c);
    // 第一轮发现接口，第二轮等待绑定后的 wl_output 名称。
    // First discover globals; then wait for the output names emitted after binding.
    if (!c.roundtrip() || !c.roundtrip()) return nullptr;
    for (const auto &output : c.outputs) if (!requested.isEmpty() && output->name == requested) return output->object;
    if (c.outputs.size() == 1) return c.outputs.front()->object;
    c.error = QStringLiteral("无法将当前 Qt 屏幕与 Wayland 输出匹配");
    return nullptr;
}
}

CaptureResult ExtImageCopyCapture::capture(const QString &outputName, const std::atomic_bool &cancelled) {
    const QString name = QStringLiteral("ext-image-copy-capture-v1");
    ProtocolContext c(cancelled);
    auto *output = connectAndFindOutput(c, outputName);
    if (!output) return {{}, name, c.error};
    if (!c.copyManager || !c.sourceManager || !c.shm) return {{}, name, QStringLiteral("合成器未公布 ext-image-copy-capture 或共享内存接口")};
    auto *source = ext_output_image_capture_source_manager_v1_create_source(c.sourceManager, output);
    auto *session = source ? ext_image_copy_capture_manager_v1_create_session(c.copyManager, source,
        includeCursor_ ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0) : nullptr;
    if (!session) return {{}, name, QStringLiteral("无法创建屏幕采集会话")};
    static const ext_image_copy_capture_session_v1_listener sessionListener{
        [](void *d, ext_image_copy_capture_session_v1 *, uint32_t w, uint32_t h) { auto *c = static_cast<ProtocolContext *>(d); c->width = w; c->height = h; },
        [](void *d, ext_image_copy_capture_session_v1 *, uint32_t f) { auto *c = static_cast<ProtocolContext *>(d); if (supported(f) && c->format == UINT32_MAX) c->format = f; },
        [](void *, ext_image_copy_capture_session_v1 *, wl_array *) {},
        [](void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {},
        [](void *d, ext_image_copy_capture_session_v1 *) { static_cast<ProtocolContext *>(d)->constraintsDone = true; },
        [](void *d, ext_image_copy_capture_session_v1 *) { static_cast<ProtocolContext *>(d)->failed = true; }
    };
    ext_image_copy_capture_session_v1_add_listener(session, &sessionListener, &c);
    if (!c.wait([&] { return c.constraintsDone; })) return {{}, name, c.error};
    if (c.width > UINT32_MAX / 4) return {{}, name, QStringLiteral("图像宽度无效")};
    c.stride = c.width * 4;
    if (!c.createBuffer()) return {{}, name, c.error};
    auto *frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (!frame) return {{}, name, QStringLiteral("无法创建截图帧")};
    static const ext_image_copy_capture_frame_v1_listener frameListener{
        [](void *d, ext_image_copy_capture_frame_v1 *, uint32_t t) { static_cast<ProtocolContext *>(d)->transform = t; },
        [](void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {},
        [](void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
        [](void *d, ext_image_copy_capture_frame_v1 *) { static_cast<ProtocolContext *>(d)->complete = true; },
        [](void *d, ext_image_copy_capture_frame_v1 *, uint32_t reason) { auto *c = static_cast<ProtocolContext *>(d); c->failed = true; c->error = QStringLiteral("合成器拒绝截图帧（原因 %1）").arg(reason); }
    };
    ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, &c);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, c.buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, int32_t(c.width), int32_t(c.height));
    ext_image_copy_capture_frame_v1_capture(frame);
    if (!c.wait([&] { return c.complete; })) return {{}, name, c.error};
    // 旋转屏幕的方向必须可靠校正；当前版本不冒险输出错误方向，改用下一后端。
    // Do not return incorrectly oriented pixels for transforms not yet supported.
    if (c.transform != WL_OUTPUT_TRANSFORM_NORMAL) return {{}, name, QStringLiteral("暂不支持该输出的旋转变换")};
    auto image = c.image();
    return image.isNull() ? CaptureResult{{}, name, QStringLiteral("无法读取截图像素")} : CaptureResult{image, name, {}};
}

CaptureResult WlrScreencopyCapture::capture(const QString &outputName, const std::atomic_bool &cancelled) {
    const QString name = QStringLiteral("wlr-screencopy");
    ProtocolContext c(cancelled);
    auto *output = connectAndFindOutput(c, outputName);
    if (!output) return {{}, name, c.error};
    if (!c.wlrManager || !c.shm) return {{}, name, QStringLiteral("合成器未公布 wlr-screencopy 或共享内存接口")};
    auto *frame = zwlr_screencopy_manager_v1_capture_output(c.wlrManager, includeCursor_ ? 1 : 0, output);
    if (!frame) return {{}, name, QStringLiteral("无法创建截图帧")};
    const uint32_t version = wl_proxy_get_version(reinterpret_cast<wl_proxy *>(frame));
    static const zwlr_screencopy_frame_v1_listener listener{
        [](void *d, zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
            auto *c = static_cast<ProtocolContext *>(d); c->format = supported(format) ? format : UINT32_MAX;
            c->width = w; c->height = h; c->stride = stride;
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy *>(f)) < 3) c->constraintsDone = true;
        },
        [](void *d, zwlr_screencopy_frame_v1 *, uint32_t f) { static_cast<ProtocolContext *>(d)->flags = f; },
        [](void *d, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) { static_cast<ProtocolContext *>(d)->complete = true; },
        [](void *d, zwlr_screencopy_frame_v1 *) { static_cast<ProtocolContext *>(d)->failed = true; },
        [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
        [](void *d, zwlr_screencopy_frame_v1 *) { static_cast<ProtocolContext *>(d)->constraintsDone = true; }
    };
    Q_UNUSED(version);
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, &c);
    if (!c.wait([&] { return c.constraintsDone; }) || !c.createBuffer()) return {{}, name, c.error};
    zwlr_screencopy_frame_v1_copy(frame, c.buffer);
    if (!c.wait([&] { return c.complete; })) return {{}, name, c.error};
    auto image = c.image();
    if (c.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        image = image.flipped(Qt::Vertical);
#else
        image = image.mirrored(false, true);
#endif
    }
    return image.isNull() ? CaptureResult{{}, name, QStringLiteral("无法读取截图像素")} : CaptureResult{image, name, {}};
}
