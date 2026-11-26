#include "frame_conversion.h"
#include <gst/gst.h>
#include <gst/video/video.h>
#include <QColor>
#include <cstdio>
#include <cstring>

// 使用带额外行填充的真实 GStreamer 缓冲区，验证颜色、行跨度和拷贝后的生命周期。
// Verify color, padded stride and ownership using a real GStreamer buffer.
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    constexpr int width = 5, height = 3, stride = 32;
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, width, height);
    GstCaps *caps = gst_video_info_to_caps(&info);
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, stride * height, nullptr);
    if (!caps || !buffer) return 1;
    gsize offsets[GST_VIDEO_MAX_PLANES] = {};
    gint strides[GST_VIDEO_MAX_PLANES] = {stride};
    if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA,
                                        width, height, 1, offsets, strides)) return 2;
    GstMapInfo mapped;
    if (!gst_buffer_map(buffer, &mapped, GST_MAP_WRITE)) return 3;
    std::memset(mapped.data, 0, mapped.size);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto *pixel = mapped.data + y * stride + x * 4;
        pixel[0] = 30 + x; pixel[1] = 60 + y; pixel[2] = 150; pixel[3] = 255;
    }
    gst_buffer_unmap(buffer, &mapped);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    if (!sample) return 4;
    const QImage image = imageFromGstSample(sample);
    gst_sample_unref(sample);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    if (image.size() != QSize(width, height) || image.pixelColor(4, 2) != QColor(34, 62, 150, 255)) return 5;
    if (!imageFromGstSample(nullptr).isNull()) return 6;
    std::fprintf(stderr, "frame conversion and independent lifetime passed\n");
    return 0;
}
