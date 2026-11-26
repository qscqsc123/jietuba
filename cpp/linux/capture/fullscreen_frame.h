#pragma once
#include <QImage>
#include <QSize>
#include <cmath>

/**
 * 校验整屏帧并保留原始像素。QScreen 的尺寸是逻辑坐标，后端整屏图片不一定带 DPR；
 * 用本次输出的逻辑宽高与实际像素共同校验比例，允许分数缩放造成的两像素舍入误差。
 * 交给像素画布前清除 DPR 标签，避免 QPixmap 再缩小一遍；不重采样图片，也不混用其他屏幕的比例。
 * Validate one complete output frame against its logical extent, allowing fractional-scale rounding.
 * Backends need not tag native output pixels with DPR. Strip the tag for pixel-coordinate canvases,
 * without resampling or borrowing a scale from another monitor.
 */
inline QImage linuxFullscreenFrame(const QImage &image,const QSize &logical) {
    if(image.isNull() || logical.isEmpty()) return {};
    const double w=logical.width(),h=logical.height();
    const double scale=(image.width()*w+image.height()*h)/(w*w+h*h);
    if(std::abs(image.width()-w*scale)>2 || std::abs(image.height()-h*scale)>2) return {};
    QImage pixels=image;
    pixels.setDevicePixelRatio(1);
    return pixels;
}
