#pragma once

#include <QImage>
#include <gst/gst.h>

// 把 RGBA 视频样本复制成独立的 QImage，支持带填充的行跨度。
// Copy an RGBA sample into an independent QImage, respecting padded row strides.
QImage imageFromGstSample(GstSample *sample);
