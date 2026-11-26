#pragma once
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>
#include <QElapsedTimer>
#include <algorithm>
#ifdef JIETUBA_HAS_LAYER_SHELL
#include <LayerShellQt/Window>
#include <wayland-client.h>
#include <poll.h>
#include <cstring>
#endif

/**
 * 贴图和截图覆盖层共用的 Wayland 能力探测。
 * 只读取 registry，不更改桌面状态；结果在进程内缓存，避免每次截图重新连接、等待。
 * 不支持 layer-shell 或未安装可选依赖时，调用方保留普通全屏窗口回退。
 * Shared, cached Wayland capability probe for pins and capture overlays.
 * Reads the registry without changing desktop state; callers retain an xdg fallback.
 */
namespace LinuxWayland {
#ifdef JIETUBA_HAS_LAYER_SHELL
inline bool probeLayerShell() {
    if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) return false;
    wl_display *display = wl_display_connect(nullptr);
    if (!display) return false;
    struct Probe { bool found = false; bool done = false; } probe;
    wl_registry *registry = wl_display_get_registry(display);
    static const wl_registry_listener listener = {
        [](void *data, wl_registry *, uint32_t, const char *name, uint32_t version) {
            if (std::strcmp(name, "zwlr_layer_shell_v1") == 0 && version >= 4) static_cast<Probe *>(data)->found = true;
        }, [](void *, wl_registry *, uint32_t) {}
    };
    wl_registry_add_listener(registry, &listener, &probe);
    wl_callback *sync = wl_display_sync(display);
    static const wl_callback_listener callback = { [](void *data, wl_callback *, uint32_t) { static_cast<Probe *>(data)->done = true; } };
    wl_callback_add_listener(sync, &callback, &probe);
    QElapsedTimer timer; timer.start();
    while (!probe.done && timer.elapsed() < 300) {
        if (wl_display_dispatch_pending(display) < 0) break;
        if (probe.done) break;
        if (wl_display_prepare_read(display) != 0) continue;
        if (wl_display_flush(display) < 0) { wl_display_cancel_read(display); break; }
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        int ready = poll(&fd, 1, std::max(0, int(300 - timer.elapsed())));
        if (ready <= 0 || !(fd.revents & POLLIN)) { wl_display_cancel_read(display); break; }
        if (wl_display_read_events(display) < 0) break;
    }
    wl_callback_destroy(sync); wl_registry_destroy(registry); wl_display_disconnect(display);
    return probe.done && probe.found;
}
#endif
// 保持父层原样绘制，只把原生鼠标与键盘输入交给子层。空 mask 在 Qt 中代表全窗口，故使用窗外区域。
// Keep parent pixels mapped. Give input to the child; an empty Qt mask means the entire window, not no input.
inline void setCaptureInput(QWidget *window, bool layer, bool active) {
    auto *surface = window->windowHandle();
    if (!surface) return;
    surface->setMask(active ? QRegion() : QRegion(-2,-2,1,1));
#ifdef JIETUBA_HAS_LAYER_SHELL
    if (layer) LayerShellQt::Window::get(surface)->setKeyboardInteractivity(active
        ? LayerShellQt::Window::KeyboardInteractivityExclusive : LayerShellQt::Window::KeyboardInteractivityNone);
#else
    Q_UNUSED(layer);
#endif
}
inline bool supportsLayerShell() {
#ifdef JIETUBA_HAS_LAYER_SHELL
    static const bool supported = probeLayerShell();
    return supported;
#else
    return false;
#endif
}
} // namespace LinuxWayland
