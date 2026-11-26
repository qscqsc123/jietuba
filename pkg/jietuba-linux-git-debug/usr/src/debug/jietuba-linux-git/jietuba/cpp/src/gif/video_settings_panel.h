#pragma once
#include "gif/video_settings.h"
#include <QWidget>
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
namespace jietuba {
/**
 * Windows/Linux 共用的视频设置表单：帧率、码率预设、系统声音、硬件优先和鼠标指针。
 * 只处理控件与经过校验的 RecordingOptions，不依赖平台采集器或设置存储。
 * Shared Windows/Linux video settings form. It edits validated RecordingOptions;
 * capture backends, persistence and dialog presentation remain owned by the caller.
 */
class VideoSettingsPanel final : public QWidget {
public:
    explicit VideoSettingsPanel(const RecordingOptions &options,QWidget *parent=nullptr);
    RecordingOptions options() const;
private:
    QSpinBox *fps_;
    QDoubleSpinBox *bitrate_;
    QCheckBox *audio_,*hardware_,*cursor_;
};
}
