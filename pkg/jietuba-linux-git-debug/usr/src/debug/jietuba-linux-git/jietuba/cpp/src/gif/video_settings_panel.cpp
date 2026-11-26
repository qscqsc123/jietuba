#include "gif/video_settings_panel.h"
#include <QFormLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QCoreApplication>
#include <cmath>
namespace jietuba {
VideoSettingsPanel::VideoSettingsPanel(const RecordingOptions &values,QWidget *parent):QWidget(parent) {
    // 保留原版翻译上下文：抽取成共享控件后，已有语言包仍能匹配这些文字。
    // Preserve the original translation context when extracting this shared form.
    const auto text=[](const char *source) {return QCoreApplication::translate("GifRecordToolbar",source);};
    auto *form=new QFormLayout(this);
    fps_=new QSpinBox(this); fps_->setObjectName("videoFps"); fps_->setRange(1,60); fps_->setValue(values.fps); fps_->setSuffix(" fps");
    form->addRow(text("帧率"),fps_);
    bitrate_=new QDoubleSpinBox(this); bitrate_->setObjectName("videoBitrate"); bitrate_->setRange(.5,50); bitrate_->setSingleStep(.5);
    bitrate_->setDecimals(1); bitrate_->setValue(values.bitrate/1000000.); bitrate_->setSuffix(" Mbps");
    auto *presets=new QComboBox(this);
    for(int value:{1,2,4,8}) presets->addItem(QStringLiteral("%1 Mbps").arg(value),double(value));
    presets->addItem(text("自定义"));
    const int index=presets->findData(values.bitrate/1000000.); presets->setCurrentIndex(index>=0 ? index : 4);
    connect(presets,&QComboBox::currentIndexChanged,this,[presets,this](int i) {if(presets->itemData(i).isValid()) bitrate_->setValue(presets->itemData(i).toDouble());});
    connect(bitrate_,&QDoubleSpinBox::valueChanged,this,[presets](double value) {const int i=presets->findData(value); presets->setCurrentIndex(i>=0 ? i : 4);});
    auto *row=new QHBoxLayout; row->addWidget(presets); row->addWidget(bitrate_); form->addRow(text("视频码率"),row);
    audio_=new QCheckBox(text("录制系统声音"),this); audio_->setObjectName("videoAudio"); audio_->setChecked(values.system_audio);
    hardware_=new QCheckBox(text("优先使用硬件编码（不可用时使用系统软件编码）"),this); hardware_->setObjectName("videoHardware"); hardware_->setChecked(values.hardware);
    cursor_=new QCheckBox(text("录制鼠标指针"),this); cursor_->setObjectName("videoCursor"); cursor_->setChecked(values.cursor);
    for(auto *widget:{audio_,hardware_,cursor_}) form->addRow(widget);
    auto *hint=new QLabel(text("码率是目标值，实际大小随画面变化。较低码率减少文件大小和细节。系统声音不包含麦克风。"),this);
    hint->setWordWrap(true); form->addRow(hint);
}
RecordingOptions VideoSettingsPanel::options() const {
    RecordingOptions result; result.fps=fps_->value(); result.bitrate=int(std::lround(bitrate_->value()*1000000));
    result.system_audio=audio_->isChecked(); result.hardware=hardware_->isChecked(); result.cursor=cursor_->isChecked(); result.validate(); return result;
}
}
