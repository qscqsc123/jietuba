#include "x11_capture.h"
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <cstring>
#endif

bool X11Capture::available() {
#ifdef Q_OS_LINUX
    return QGuiApplication::platformName() == QLatin1String("xcb");
#else
    return false;
#endif
}

CaptureResult X11Capture::capture(const QString &output, const std::atomic_bool &cancelled) {
#ifndef Q_OS_LINUX
    Q_UNUSED(output); Q_UNUSED(cancelled);
    return {{}, QStringLiteral("X11"), QStringLiteral("X11 support not compiled")};
#else
    if (!available()) {
        return {{}, QStringLiteral("X11"), QStringLiteral("Not running on X11/Xorg")};
    }
    
    // 查找指定输出对应的屏幕几何
    QRect geometry;
    for (auto *screen : QGuiApplication::screens()) {
        if (screen->name() == output) {
            geometry = screen->geometry();
            break;
        }
    }
    
    if (geometry.isEmpty()) {
        return {{}, QStringLiteral("X11"), QStringLiteral("Screen '%1' not found").arg(output)};
    }
    
    return captureX11Region(geometry, cancelled);
#endif
}

CaptureResult X11Capture::captureRegion(const QString &output, const QRect &region, 
                                       const QRect &outputGeometry, const std::atomic_bool &cancelled) {
#ifndef Q_OS_LINUX
    Q_UNUSED(output); Q_UNUSED(region); Q_UNUSED(outputGeometry); Q_UNUSED(cancelled);
    return {{}, QStringLiteral("X11"), QStringLiteral("X11 support not compiled")};
#else
    if (!available()) {
        return {{}, QStringLiteral("X11"), QStringLiteral("Not running on X11/Xorg")};
    }
    
    if (region.isEmpty() || !outputGeometry.contains(region)) {
        return {{}, QStringLiteral("X11"), QStringLiteral("Invalid region coordinates")};
    }
    
    return captureX11Region(region, cancelled);
#endif
}

CaptureResult X11Capture::captureX11Region(const QRect &region, const std::atomic_bool &cancelled) {
#ifndef Q_OS_LINUX
    Q_UNUSED(region); Q_UNUSED(cancelled);
    return {{}, QStringLiteral("X11"), QStringLiteral("X11 support not compiled")};
#else
    const QString name = QStringLiteral("X11 XGetImage");
    
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        return {{}, name, QStringLiteral("无法打开 X11 显示连接")};
    }
    
    Window root = DefaultRootWindow(display);
    
    // 检查取消
    if (cancelled.load()) {
        XCloseDisplay(display);
        return {{}, name, QStringLiteral("已取消")};
    }
    
    // 使用 XGetImage 捕获指定区域
    XImage *ximage = XGetImage(display, root, region.x(), region.y(), 
                               region.width(), region.height(), AllPlanes, ZPixmap);
    
    if (!ximage) {
        XCloseDisplay(display);
        return {{}, name, QStringLiteral("XGetImage 失败")};
    }
    
    // 转换 XImage 到 QImage
    QImage image(region.width(), region.height(), QImage::Format_RGB32);
    
    // X11 图像格式转换（处理不同的位深度和字节序）
    if (ximage->bits_per_pixel == 32) {
        // 32-bit color
        for (int y = 0; y < region.height(); ++y) {
            if (cancelled.load()) {
                XDestroyImage(ximage);
                XCloseDisplay(display);
                return {{}, name, QStringLiteral("已取消")};
            }
            
            QRgb *scanLine = reinterpret_cast<QRgb*>(image.scanLine(y));
            const char *xline = ximage->data + y * ximage->bytes_per_line;
            
            for (int x = 0; x < region.width(); ++x) {
                const unsigned long pixel = *reinterpret_cast<const unsigned long*>(xline + x * 4);
                // X11 通常是 BGR 格式
                const int b = (pixel & ximage->blue_mask) >> 0;
                const int g = (pixel & ximage->green_mask) >> 8;
                const int r = (pixel & ximage->red_mask) >> 16;
                scanLine[x] = qRgb(r, g, b);
            }
        }
    } else if (ximage->bits_per_pixel == 24) {
        // 24-bit color
        for (int y = 0; y < region.height(); ++y) {
            if (cancelled.load()) {
                XDestroyImage(ximage);
                XCloseDisplay(display);
                return {{}, name, QStringLiteral("已取消")};
            }
            
            QRgb *scanLine = reinterpret_cast<QRgb*>(image.scanLine(y));
            const char *xline = ximage->data + y * ximage->bytes_per_line;
            
            for (int x = 0; x < region.width(); ++x) {
                const unsigned char *pixel = reinterpret_cast<const unsigned char*>(xline + x * 3);
                scanLine[x] = qRgb(pixel[2], pixel[1], pixel[0]); // BGR to RGB
            }
        }
    } else {
        // 不支持的格式，回退到 Qt 的屏幕截图
        XDestroyImage(ximage);
        XCloseDisplay(display);
        
        // 使用 Qt 的回退方案
        for (auto *screen : QGuiApplication::screens()) {
            if (screen->geometry().contains(region)) {
                QPixmap pixmap = screen->grabWindow(0, region.x() - screen->geometry().x(),
                                                   region.y() - screen->geometry().y(),
                                                   region.width(), region.height());
                if (!pixmap.isNull()) {
                    return {pixmap.toImage(), QStringLiteral("X11 (Qt fallback)"), {}};
                }
            }
        }
        return {{}, name, QStringLiteral("不支持的位深度: %1 bpp").arg(ximage->bits_per_pixel)};
    }
    
    XDestroyImage(ximage);
    
    // 添加光标（如果需要）
    if (includeCursor_) {
        int xfixes_event_base, xfixes_error_base;
        if (XFixesQueryExtension(display, &xfixes_event_base, &xfixes_error_base)) {
            XFixesCursorImage *cursor = XFixesGetCursorImage(display);
            if (cursor) {
                const int cursorX = cursor->x - cursor->xhot - region.x();
                const int cursorY = cursor->y - cursor->yhot - region.y();
                
                // 只在光标位于捕获区域内时绘制
                if (cursorX > -int(cursor->width) && cursorX < region.width() &&
                    cursorY > -int(cursor->height) && cursorY < region.height()) {
                    
                    QImage cursorImage(cursor->width, cursor->height, QImage::Format_ARGB32);
                    for (unsigned int y = 0; y < cursor->height; ++y) {
                        QRgb *line = reinterpret_cast<QRgb*>(cursorImage.scanLine(y));
                        for (unsigned int x = 0; x < cursor->width; ++x) {
                            line[x] = cursor->pixels[y * cursor->width + x];
                        }
                    }
                    
                    QPainter painter(&image);
                    painter.drawImage(cursorX, cursorY, cursorImage);
                }
                
                XFree(cursor);
            }
        }
    }
    
    XCloseDisplay(display);
    
    if (image.isNull()) {
        return {{}, name, QStringLiteral("生成的图像无效")};
    }
    
    return {image, name, {}};
#endif
}
