#include "frame_conversion.h"
#include <gst/gst.h>
#include <gst/video/video.h>

QImage imageFromGstSample(GstSample *sample) {
    if (!sample || !gst_sample_get_caps(sample) || !gst_sample_get_buffer(sample)) return {};
    GstVideoInfo info;
    GstVideoFrame frame;
    QImage image;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) && GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGBA &&
        gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
        const int width = GST_VIDEO_FRAME_WIDTH(&frame), height = GST_VIDEO_FRAME_HEIGHT(&frame);
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        const auto *data = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        // 必须复制：sample 释放后 PipeWire 可以立即复用底层缓冲区。
        // Copy before unmapping: PipeWire may immediately reuse the underlying buffer.
        if (data && width > 0 && height > 0 && qint64(stride) >= qint64(width) * 4)
            image = QImage(data, width, height, stride, QImage::Format_RGBA8888).copy();
        gst_video_frame_unmap(&frame);
    }
    return image;
}
