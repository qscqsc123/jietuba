#include "capture/protocol_capture.h"
#include "capture/capture_router.h"
#include "ext-image-copy-capture-v1-server.h"
#include "ext-image-capture-source-v1-server.h"
#include "wlr-screencopy-unstable-v1-server.h"
#include <wayland-server.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QColor>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <thread>
#include <sys/stat.h>

namespace {
constexpr int width = 64, height = 48, stride = width * 4;
std::atomic_int extCursorRequests{0},wlrCursorRequests{0};
void destroy(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
void fill(wl_resource *resource) {
    auto *buffer = wl_shm_buffer_get(resource);
    if (!buffer) return;
    wl_shm_buffer_begin_access(buffer);
    auto *pixels = static_cast<unsigned char *>(wl_shm_buffer_get_data(buffer));
    const int row = wl_shm_buffer_get_stride(buffer);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto *p = pixels + y * row + x * 4;
        p[0] = 10; p[1] = 20; p[2] = y; p[3] = 255;
    }
    wl_shm_buffer_end_access(buffer);
}

const struct wl_output_interface outputImpl{destroy};
void bindOutput(wl_client *client, void *, uint32_t version, uint32_t id) {
    auto *r = wl_resource_create(client, &wl_output_interface, std::min(version, 4u), id);
    wl_resource_set_implementation(r, &outputImpl, nullptr, nullptr);
    wl_output_send_geometry(r, 0, 0, 100, 100, WL_OUTPUT_SUBPIXEL_UNKNOWN, "test", "test", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT, width, height, 60000);
    if (wl_resource_get_version(r) >= 2) { wl_output_send_scale(r, 1); wl_output_send_done(r); }
    if (wl_resource_get_version(r) >= 4) { wl_output_send_name(r, "TEST-1"); wl_output_send_description(r, "Fake output"); wl_output_send_done(r); }
}

const struct ext_image_capture_source_v1_interface sourceImpl{destroy};
const struct ext_output_image_capture_source_manager_v1_interface sourceManagerImpl{
    [](wl_client *c, wl_resource *, uint32_t id, wl_resource *) {
        auto *r = wl_resource_create(c, &ext_image_capture_source_v1_interface, 1, id);
        wl_resource_set_implementation(r, &sourceImpl, nullptr, nullptr);
    }, destroy
};
void bindSource(wl_client *c, void *, uint32_t, uint32_t id) {
    auto *r = wl_resource_create(c, &ext_output_image_capture_source_manager_v1_interface, 1, id);
    wl_resource_set_implementation(r, &sourceManagerImpl, nullptr, nullptr);
}
struct Frame { wl_resource *buffer = nullptr; };
const struct ext_image_copy_capture_frame_v1_interface extFrameImpl{
    destroy,
    [](wl_client *, wl_resource *r, wl_resource *b) { static_cast<Frame *>(wl_resource_get_user_data(r))->buffer = b; },
    [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
    [](wl_client *, wl_resource *r) {
        auto *f = static_cast<Frame *>(wl_resource_get_user_data(r));
        fill(f->buffer);
        ext_image_copy_capture_frame_v1_send_transform(r, WL_OUTPUT_TRANSFORM_NORMAL);
        ext_image_copy_capture_frame_v1_send_ready(r);
    }
};
const struct ext_image_copy_capture_session_v1_interface extSessionImpl{
    [](wl_client *c, wl_resource *, uint32_t id) {
        auto *r = wl_resource_create(c, &ext_image_copy_capture_frame_v1_interface, 1, id);
        wl_resource_set_implementation(r, &extFrameImpl, new Frame, [](wl_resource *r) { delete static_cast<Frame *>(wl_resource_get_user_data(r)); });
    }, destroy
};
const struct ext_image_copy_capture_manager_v1_interface extManagerImpl{
    [](wl_client *c, wl_resource *, uint32_t id, wl_resource *, uint32_t options) {
        if(options==EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) ++extCursorRequests;
        auto *r = wl_resource_create(c, &ext_image_copy_capture_session_v1_interface, 1, id);
        wl_resource_set_implementation(r, &extSessionImpl, nullptr, nullptr);
        ext_image_copy_capture_session_v1_send_buffer_size(r, width, height);
        ext_image_copy_capture_session_v1_send_shm_format(r, WL_SHM_FORMAT_XRGB8888);
        ext_image_copy_capture_session_v1_send_done(r);
    },
    [](wl_client *, wl_resource *, uint32_t, wl_resource *, wl_resource *) {}, destroy
};
void bindExt(wl_client *c, void *, uint32_t, uint32_t id) {
    auto *r = wl_resource_create(c, &ext_image_copy_capture_manager_v1_interface, 1, id);
    wl_resource_set_implementation(r, &extManagerImpl, nullptr, nullptr);
}

const struct zwlr_screencopy_frame_v1_interface wlrFrameImpl{
    [](wl_client *, wl_resource *r, wl_resource *b) {
        fill(b);
        zwlr_screencopy_frame_v1_send_flags(r, ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
        zwlr_screencopy_frame_v1_send_ready(r, 0, 0, 0);
    }, destroy, [](wl_client *, wl_resource *, wl_resource *) {}
};
const struct zwlr_screencopy_manager_v1_interface wlrManagerImpl{
    [](wl_client *c, wl_resource *manager, uint32_t id, int32_t cursor, wl_resource *) {
        if(cursor==1) ++wlrCursorRequests;
        auto *r = wl_resource_create(c, &zwlr_screencopy_frame_v1_interface, wl_resource_get_version(manager), id);
        wl_resource_set_implementation(r, &wlrFrameImpl, nullptr, nullptr);
        zwlr_screencopy_frame_v1_send_buffer(r, WL_SHM_FORMAT_XRGB8888, width, height, stride);
        if (wl_resource_get_version(r) >= 3) zwlr_screencopy_frame_v1_send_buffer_done(r);
    },
    [](wl_client *, wl_resource *, uint32_t, int32_t, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}, destroy
};
void bindWlr(wl_client *c, void *, uint32_t version, uint32_t id) {
    auto *r = wl_resource_create(c, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u), id);
    wl_resource_set_implementation(r, &wlrManagerImpl, nullptr, nullptr);
}
}

/**
 * 最小模拟合成器：在私有 Unix socket 上公布屏幕与两种截图协议，并返回
 * 可预测像素。测试 ext 的共享内存读取、wlr 的上下翻转和未支持接口回退。
 *
 * Minimal private Wayland compositor. Exercises both shared-memory protocols,
 * output identification and wlr vertical inversion without reading the real desktop.
 */
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir runtime;
    if (!runtime.isValid() || chmod(runtime.path().toLocal8Bit().constData(), 0700) != 0) return 1;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    auto *display = wl_display_create();
    if (!display || wl_display_init_shm(display) != 0) return 2;
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) return 3;
    qputenv("WAYLAND_DISPLAY", socket);
    const bool wlrOnly = app.arguments().contains(QStringLiteral("--wlr-only"));
    qputenv("XDG_CURRENT_DESKTOP", "test-compositor");
    wl_global_create(display, &wl_output_interface, 4, nullptr, bindOutput);
    if (!wlrOnly) {
        wl_global_create(display, &ext_output_image_capture_source_manager_v1_interface, 1, nullptr, bindSource);
        wl_global_create(display, &ext_image_copy_capture_manager_v1_interface, 1, nullptr, bindExt);
    }
    wl_global_create(display, &zwlr_screencopy_manager_v1_interface, 3, nullptr, bindWlr);
    std::atomic_bool done{false}, cancelled{false};
    std::thread compositor([&] { while (!done.load()) { wl_event_loop_dispatch(wl_display_get_event_loop(display), 20); wl_display_flush_clients(display); } });
    ExtImageCopyCapture ext;
    auto first = ext.capture("TEST-1", cancelled);
    WlrScreencopyCapture wlr;
    auto second = wlr.capture("TEST-1", cancelled);
    // 使用相同私有协议服务器验证录屏的指针开关，绝不访问真实显示器。
    // Exercise cursor flags against this private protocol server, never the real compositor.
    ExtImageCopyCapture cursorExt(true); const auto extWithCursor=cursorExt.capture("TEST-1",cancelled);
    WlrScreencopyCapture cursorWlr(true); const auto wlrWithCursor=cursorWlr.capture("TEST-1",cancelled);
    // 使用不同像素判断路由器是否优先 ext，以及 ext 缺失时是否回退 wlr。
    // Distinct pixel values verify ext priority and fallback to wlr when ext is absent.
    CaptureRouter router;
    QEventLoop loop;
    QImage routed;
    QString routeError;
    QObject::connect(&router, &CaptureRouter::frameReady, &loop, [&](const QImage &image) { routed = image; loop.quit(); });
    QObject::connect(&router, &CaptureRouter::failed, &loop, [&](const QString &error) { routeError = error; loop.quit(); });
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    router.capture(QStringLiteral("TEST-1"));
    loop.exec();
    const QImage defaultRouted=routed;
    router.capture(QStringLiteral("TEST-1"),CallerWindowPolicy::Hide,true);
    loop.exec();
    done.store(true); compositor.join();
    wl_display_destroy_clients(display); wl_display_destroy(display);
    if ((!wlrOnly && (first.image.size() != QSize(width, height) || first.image.pixelColor(3, 4) != QColor(4, 20, 10))) || (wlrOnly && (first.error.isEmpty() || !first.image.isNull()))) { std::fprintf(stderr, "ext failed: %s\n", qPrintable(first.error)); return 4; }
    if (second.image.size() != QSize(width, height) || second.image.pixelColor(3, 4) != QColor(height - 1 - 4, 20, 10)) { std::fprintf(stderr, "wlr failed: %s\n", qPrintable(second.error)); return 5; }
    if((!wlrOnly && (extWithCursor.image.isNull() || extCursorRequests.load()!=2))
        || wlrWithCursor.image.isNull() || wlrCursorRequests.load()!=(wlrOnly ? 2 : 1)) {std::fprintf(stderr,"cursor protocol flags failed\n"); return 7;}
    if(defaultRouted.isNull()) return 8;
    if (routed.size() != QSize(width, height) || routed.pixelColor(3, 4) != QColor(wlrOnly ? height - 1 - 4 : 4, 20, 10)) { std::fprintf(stderr, "router failed: %s\n", qPrintable(routeError)); return 6; }
    std::fprintf(stderr, "ext and wlr Wayland capture passed\n");
    return 0;
}
