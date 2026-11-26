#include "recording_result_preview.h"
#include "gif_frames.h"
#include "gif/range_slider.h"
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QMediaPlayer>
#include <QResizeEvent>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cmath>

LinuxGifPreviewLabel::LinuxGifPreviewLabel(QWidget *parent) : QLabel(parent) {
    setAlignment(Qt::AlignCenter); setMinimumSize(1,1); setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
}
void LinuxGifPreviewLabel::setFrame(const QImage &frame) { frame_ = frame; fit(); }
QPixmap LinuxGifPreviewLabel::fittedPixmap(const QImage &frame, const QSize &logicalSize, qreal dpr) {
    if (frame.isNull() || logicalSize.isEmpty() || !std::isfinite(dpr) || dpr <= 0) return {};
    // 先计算物理像素预算再设置 DPR，避免分数缩放下二次放大。
    // Fit physical pixels before setting DPR, avoiding double scaling on fractional-DPI screens.
    const QSize bounds(qRound(logicalSize.width()*dpr),qRound(logicalSize.height()*dpr));
    const QSize fitted = frame.size().scaled(bounds,Qt::KeepAspectRatio).boundedTo(frame.size());
    QPixmap pixmap = QPixmap::fromImage(fitted == frame.size() ? frame : frame.scaled(fitted,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(dpr); return pixmap;
}
void LinuxGifPreviewLabel::fit() { if (!frame_.isNull()) setPixmap(fittedPixmap(frame_,size(),devicePixelRatioF())); }
void LinuxGifPreviewLabel::resizeEvent(QResizeEvent *event) { QLabel::resizeEvent(event); fit(); }
bool LinuxGifPreviewLabel::event(QEvent *event) {
    const bool result = QLabel::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange) fit();
    return result;
}

LinuxRecordingResultPreview::LinuxRecordingResultPreview(const QString &path, QWidget *parent, const LinuxGifFrames *gifFrames)
    : QWidget(parent), gifFrames_(gifFrames) {
    setObjectName(QStringLiteral("linuxRecordingResultPreview"));
    setAutoFillBackground(true);
    auto palette=this->palette(); palette.setColor(QPalette::Window,QColor("#20252c")); palette.setColor(QPalette::WindowText,Qt::white); setPalette(palette);
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(8,8,8,8); layout->setSpacing(4); layout->setSizeConstraint(QLayout::SetNoConstraint);
    image_=new LinuxGifPreviewLabel(this); image_->setText(QStringLiteral("正在加载预览…")); layout->addWidget(image_,1);
    status_=new QLabel(this); status_->setObjectName(QStringLiteral("recordingResultStatus")); status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); layout->addWidget(status_);
    status_->setStyleSheet(QStringLiteral("color: #c9d5e4; background: transparent;"));
    if (gifFrames_) {
        trimSlider_ = new jietuba::RangeSlider(this);
        trimSlider_->setObjectName(QStringLiteral("recordingResultTrim"));
        trimSlider_->setToolTip(QStringLiteral("拖动两端手柄，选择要保留的起止帧"));
        layout->insertWidget(1, trimSlider_);
        auto *bounds = new QHBoxLayout;
        trimStart_ = new QLabel(this);
        trimStart_->setObjectName("recordingTrimStart");
        trimEnd_ = new QLabel(this);
        trimEnd_->setObjectName("recordingTrimEnd");
        for (auto *label : {trimStart_, trimEnd_})
            label->setStyleSheet(QStringLiteral("color: #d2eaff; background: transparent;"));
        bounds->addWidget(trimStart_);
        bounds->addStretch();
        bounds->addWidget(trimEnd_);
        layout->insertLayout(2, bounds);
        trimSlider_->set_range(int(gifFrames_->frames().size()));
        connect(trimSlider_, &jietuba::RangeSlider::seek_requested, this, &LinuxRecordingResultPreview::seekGif);
        connect(trimSlider_, &jietuba::RangeSlider::trim_changed, this, [this](int first, int last) {
            if (gifFrame_ < first || gifFrame_ > last) seekGif(gifFrame_);
            else updateGifStatus();
        });
        gifTimer_.setSingleShot(true);
        gifTimer_.setTimerType(Qt::PreciseTimer);
        connect(&gifTimer_, &QTimer::timeout, this, &LinuxRecordingResultPreview::advanceGif);
        // 只读取首帧，不启动动画计时器；不会在完成窗口出现时自动播放。
        // Decode frame zero without starting the animation timer.
        QTimer::singleShot(0, this, [this] { seekGif(0); });
    } else {
        auto *timeline = new QSlider(Qt::Horizontal, this);
        timeline->setObjectName(QStringLiteral("recordingResultTimeline"));
        timeline->setRange(0, 0);
        layout->insertWidget(1, timeline);
        player_=new QMediaPlayer(this); audio_=new QAudioOutput(this); player_->setAudioOutput(audio_);
        player_->setLoops(QMediaPlayer::Infinite);
        auto *sink=new QVideoSink(this); player_->setVideoSink(sink);
        connect(sink,&QVideoSink::videoFrameChanged,this,[this](const QVideoFrame &frame) {if(frame.isValid()) present(frame.toImage());});
        connect(player_,&QMediaPlayer::durationChanged,this,[timeline](qint64 ms) {timeline->setRange(0,int(ms));});
        const auto updatePosition = [this,timeline](qint64 ms) {
            if(!timeline->isSliderDown()) timeline->setValue(int(ms));
            status_->setText(QStringLiteral("MP4 播放进度 · %1 / %2 秒").arg(ms/1000.,0,'f',1).arg(player_->duration()/1000.,0,'f',1));
        };
        connect(player_, &QMediaPlayer::positionChanged, this, updatePosition);
        connect(player_, &QMediaPlayer::durationChanged, this, [this, updatePosition] { updatePosition(player_->position()); });
        updatePosition(0);
        connect(player_,&QMediaPlayer::playbackStateChanged,this,[this] {emit playingChanged(isPlaying());});
        connect(player_,&QMediaPlayer::errorOccurred,this,[this](QMediaPlayer::Error,const QString &reason) {reportError(reason);});
        connect(timeline,&QSlider::sliderMoved,player_,&QMediaPlayer::setPosition);
        // 暂停加载会解码静止首帧，不启动声音播放；与 GIF 首帧状态一致。
        // Load paused to decode the still first frame without starting audio, matching GIF.
        player_->setSource(QUrl::fromLocalFile(path)); QTimer::singleShot(0,this,[this] {player_->pause();});
    }
}
LinuxRecordingResultPreview::~LinuxRecordingResultPreview() {
    // 停止声音和异步视频回调，再让 QWidget 销毁图像控件。
    // Stop audio and asynchronous frame callbacks before child widgets are destroyed.
    if(player_) {player_->disconnect(this); player_->setVideoSink(nullptr); player_->stop();}
}
bool LinuxRecordingResultPreview::isPlaying() const {
    return !failed_ && (gifFrames_ ? gifTimer_.isActive() : player_ && player_->playbackState()==QMediaPlayer::PlayingState);
}
void LinuxRecordingResultPreview::togglePlayback() {
    if(failed_) return;
    if (isPlaying()) { pausePlayback(); return; }
    if (gifFrame_ >= 0) {
        const auto [first, last] = trimRange();
        if (gifFrame_ >= last) seekGif(first);
        if (failed_) return;
        gifTimer_.start(gifFrameDelay());
        emit playingChanged(true);
    } else if (player_) {
        if (player_->position() >= player_->duration()) player_->setPosition(0);
        player_->play();
    }
}
void LinuxRecordingResultPreview::pausePlayback() {
    if (gifTimer_.isActive()) {
        gifTimer_.stop();
        emit playingChanged(false);
    }
    if (player_) player_->pause();
}
std::pair<int, int> LinuxRecordingResultPreview::trimRange() const {
    return gifFrame_ >= 0 && !failed_ ? trimSlider_->get_trim_range() : std::pair{0, -1};
}
void LinuxRecordingResultPreview::seekGif(int frame) {
    if (failed_) return;
    const auto [first, last] = trimSlider_->get_trim_range();
    frame = std::clamp(frame, first, last);
    if (gifFrame_ != frame) {
        // 无损源帧支持随机访问，回跳无需重新解码整个 GIF，也无需缓存整段画面。
        // Seek independent source frames without decoding from frame zero or caching the whole clip.
        const auto image = gifFrames_->image(frame);
        if (image.isNull()) { reportError(QStringLiteral("无法读取第 %1 帧").arg(frame + 1)); return; }
        gifFrame_ = frame;
        trimSlider_->set_playhead(frame);
        present(image);
    }
    updateGifStatus();
    if (gifTimer_.isActive()) gifTimer_.start(gifFrameDelay());
}
int LinuxRecordingResultPreview::gifFrameDelay() const {
    const auto &frames = gifFrames_->frames();
    const auto end = gifFrame_ + 1 < frames.size() ? frames[gifFrame_ + 1].milliseconds : gifFrames_->duration();
    return int(std::max<qint64>(10, end - frames[gifFrame_].milliseconds));
}
void LinuxRecordingResultPreview::advanceGif() {
    // 与原版一致，播完保留区间后停在起始帧。手动调度以保留原 GIF 各帧不同的延时。
    // Stop at the selected start after playback, using each frame's own GIF delay.
    const auto [first, last] = trimSlider_->get_trim_range();
    if (gifFrame_ >= last) {
        seekGif(first);
        if (!failed_) emit playingChanged(false);
    } else {
        seekGif(gifFrame_ + 1);
        if (!failed_) gifTimer_.start(gifFrameDelay());
    }
}
void LinuxRecordingResultPreview::updateGifStatus() {
    const auto [first, last] = trimSlider_->get_trim_range();
    trimStart_->setText(QStringLiteral("起始帧 %1").arg(first + 1));
    trimEnd_->setText(QStringLiteral("结束帧 %1").arg(last + 1));
    status_->setText(QStringLiteral("GIF · 当前 %1 / %2 帧 · 保留 %3 帧")
                         .arg(gifFrame_+1).arg(gifFrames_->frames().size()).arg(last-first+1));
}
void LinuxRecordingResultPreview::present(const QImage &frame) {
    if(frame.isNull()) return;
    currentFrame_=frame; image_->setFrame(frame); emit framePresented(frame.size());
}
void LinuxRecordingResultPreview::reportError(const QString &reason) {
    gifTimer_.stop();
    failed_=true; status_->setText(QStringLiteral("预览失败：%1；文件已保存，可从文件夹打开。").arg(reason)); status_->setWordWrap(true); emit playingChanged(false);
}
LinuxRecordingResultPreview::ResultLayout LinuxRecordingResultPreview::resultLayout(const QRect &available,const QSize &content,const QSize &toolbar) {
    if(available.isEmpty() || toolbar.isEmpty()) return {};
    // 预览和工具栏作为一组居中，向下偏移屏幕高度的 6%；为任务栏/屏幕边缘保留余量。
    // Center the preview/toolbar group with a 6% downward offset, respecting panels and screen edges.
    constexpr int margin=12,gap=8;
    const QRect safe=available.adjusted(margin,margin,-margin,-margin);
    const int maxWidth=std::min(720,safe.width()),maxHeight=std::min(480,safe.height()-toolbar.height()-gap);
    if(maxWidth<1 || maxHeight<1) return {};
    const QSize preview(std::clamp(content.width(),std::min(320,maxWidth),maxWidth),
                        std::clamp(content.height()+80,std::min(200,maxHeight),maxHeight));
    const int height=preview.height()+gap+toolbar.height();
    const int top=std::clamp(available.center().y()+qRound(available.height()*.06)-height/2,safe.top(),safe.bottom()-height+1);
    const int center=available.center().x();
    return {QRect(QPoint(center-preview.width()/2,top),preview),
            QRect(QPoint(center-toolbar.width()/2,top+preview.height()+gap),toolbar)};
}
