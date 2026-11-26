#pragma once
#include "wayland_surface_support.h"
#include <QCursor>
#include <QScreen>

// region 为输出内逻辑坐标，负屏幕原点和分数缩放无需二次换算。
// Place in output-local logical coordinates; plain xdg toplevels remain compositor-positioned.
inline void placeCaptureResult(QWidget *w, QScreen *s, QRect region, const QString &mode) {
    if (!s) return;
    const QRect bounds=s->availableGeometry(); w->resize(w->size().boundedTo(bounds.size()));
    region.translate(s->geometry().topLeft());
    QPoint at=bounds.center()-QPoint(w->width()/2,w->height()/2);
    if (mode=="cursor") at=QCursor::pos()+QPoint(12,12);
    else if (mode=="region" && !region.isEmpty()) {
        at=region.topRight()+QPoint(10,0); if (at.x()+w->width()>bounds.right()+1) at.setX(region.left()-w->width()-10);
    }
    at.setX(qBound(bounds.left(),at.x(),qMax(bounds.left(),bounds.right()+1-w->width())));
    at.setY(qBound(bounds.top(),at.y(),qMax(bounds.top(),bounds.bottom()+1-w->height())));
    w->setScreen(s);
#ifdef JIETUBA_HAS_LAYER_SHELL
    if (LinuxWayland::supportsLayerShell()) {
        w->winId(); auto *layer=LayerShellQt::Window::get(w->windowHandle());
        layer->setScope("jietuba-ocr"); layer->setScreen(s); layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorLeft);
        const QPoint offset=at-s->geometry().topLeft(); layer->setMargins({offset.x(),offset.y(),0,0}); layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    }
#endif
    w->move(at);
}
