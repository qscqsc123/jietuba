#include "recording_audio.h"
#include <gst/gst.h>
#include <QFile>
#include <QFileInfo>
LinuxRecordingAudio::LinuxRecordingAudio(QObject *parent,const QString &sourceFactory):QObject(parent),sourceFactory_(sourceFactory) {
    poll_.setInterval(20); connect(&poll_,&QTimer::timeout,this,&LinuxRecordingAudio::pollBus);
}
LinuxRecordingAudio::~LinuxRecordingAudio() {release();}
bool LinuxRecordingAudio::start() {
    release(); finishing_=false;
    GError *error=nullptr;
    if(!directory_.isValid() || !gst_init_check(nullptr,nullptr,&error)) {
        if(error) g_error_free(error);
        emit failed(QStringLiteral("无法初始化系统声音录制或创建音频临时目录。")); return false;
    }
    pipeline_=gst_pipeline_new(nullptr);
    auto *source=gst_element_factory_make(sourceFactory_.toUtf8().constData(),nullptr);
    auto *convert=gst_element_factory_make("audioconvert",nullptr),*resample=gst_element_factory_make("audioresample",nullptr);
    auto *filter=gst_element_factory_make("capsfilter",nullptr),*wav=gst_element_factory_make("wavenc",nullptr),*sink=gst_element_factory_make("filesink",nullptr);
    if(!pipeline_ || !source || !convert || !resample || !filter || !wav || !sink) {
        for(auto *element:{source,convert,resample,filter,wav,sink}) if(element) gst_object_unref(element);
        release(); emit failed(QStringLiteral("缺少系统声音组件，请安装 gst-plugins-good（PulseAudio/WAV），或关闭系统声音。")); return false;
    }
    // 明确指定播放 monitor：绝不能用默认 source，否则可能录到麦克风。
    // Select the playback monitor explicitly, never the default source (which can be a microphone).
    if(sourceFactory_=="pulsesrc") g_object_set(source,"device","@DEFAULT_MONITOR@",nullptr);
    else if(sourceFactory_=="audiotestsrc") g_object_set(source,"is-live",TRUE,nullptr);
    GstCaps *caps=gst_caps_from_string("audio/x-raw,format=S16LE,rate=48000,channels=2");
    g_object_set(filter,"caps",caps,nullptr); gst_caps_unref(caps);
    path_=directory_.filePath(QStringLiteral("system-audio.wav"));
    const auto encoded=QFile::encodeName(path_); g_object_set(sink,"location",encoded.constData(),nullptr);
    gst_bin_add_many(GST_BIN(pipeline_),source,convert,resample,filter,wav,sink,nullptr);
    if(!gst_element_link_many(source,convert,resample,filter,wav,sink,nullptr)
        || gst_element_set_state(pipeline_,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) {
        release(); emit failed(QStringLiteral("系统声音录制失败：请检查 PipeWire-Pulse/PulseAudio 播放设备，或关闭系统声音。")); return false;
    }
    bus_=gst_element_get_bus(pipeline_); poll_.start(); return true;
}
void LinuxRecordingAudio::setPaused(bool paused) {
    if(pipeline_ && !finishing_ && gst_element_set_state(pipeline_,paused ? GST_STATE_PAUSED : GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) {
        release(); emit failed(QStringLiteral("系统声音暂停或恢复失败。"));
    }
}
void LinuxRecordingAudio::finish() {
    if(!pipeline_) {emit finished({}); return;}
    finishing_=true; finishWait_.start();
    gst_element_set_state(pipeline_,GST_STATE_PLAYING);
    if(!gst_element_send_event(pipeline_,gst_event_new_eos())) {release(); emit failed(QStringLiteral("无法结束系统声音录制。"));}
}
void LinuxRecordingAudio::cancel() {release();}
void LinuxRecordingAudio::release() {
    poll_.stop();
    if(pipeline_) {gst_element_set_state(pipeline_,GST_STATE_NULL); gst_object_unref(pipeline_); pipeline_=nullptr;}
    if(bus_) {gst_object_unref(bus_); bus_=nullptr;}
}
void LinuxRecordingAudio::pollBus() {
    if(!bus_) return;
    GstMessage *message=gst_bus_pop_filtered(bus_,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
    if(message) {
        const bool failedMessage=GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR;
        QString reason;
        if(failedMessage) {GError *error=nullptr; gchar *debug=nullptr; gst_message_parse_error(message,&error,&debug);
            if(error) {reason=QString::fromUtf8(error->message); g_error_free(error);} g_free(debug);}
        gst_message_unref(message); release();
        if(failedMessage) emit failed(QStringLiteral("系统声音录制失败：%1。请检查播放设备或关闭系统声音。").arg(reason));
        else if(QFileInfo(path_).size()>44) emit finished(path_);
        else emit failed(QStringLiteral("系统声音未产生有效采样，请检查播放设备。"));
    } else if(finishing_ && finishWait_.elapsed()>5000) {release(); emit failed(QStringLiteral("结束系统声音录制超时。"));}
}
