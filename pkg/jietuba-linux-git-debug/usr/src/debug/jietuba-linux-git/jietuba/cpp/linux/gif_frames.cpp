#include "gif_frames.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

LinuxGifFrames::LinuxGifFrames(QObject *parent) : QObject(parent), directory_(std::make_shared<QTemporaryDir>()) {
    connect(&writer_, &QFutureWatcher<WriteResult>::finished, this, &LinuxGifFrames::writeFinished);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        // 错误输出设上限，异常编码器不能无限占用内存。
        // Bound diagnostics even if a broken encoder continuously writes errors.
        diagnostic_ = (diagnostic_ + QString::fromLocal8Bit(process_.readAllStandardError())).right(4096);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &LinuxGifFrames::readProgress);
    connect(&process_, &QProcess::finished, this, &LinuxGifFrames::processFinished);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && exporting()) exportFailed(QStringLiteral("无法启动 FFmpeg：%1").arg(process_.errorString()));
    });
    connect(&copier_, &QFutureWatcher<QString>::finished, this, [this] {
        const QString error = copier_.result();
        stage_ = Stage::Idle;
        if (error.isEmpty()) emit exported(target_); else emit failed(error);
    });
}
LinuxGifFrames::~LinuxGifFrames() {
    // 工作线程持有目录共享引用；等待的是本地文件任务，绝不在这里等待桌面回调。
    // Workers retain the directory; only local file jobs are joined at destruction.
    process_.disconnect(this); process_.kill(); process_.waitForFinished(1000);
    writer_.waitForFinished(); copier_.waitForFinished();
}
bool LinuxGifFrames::valid() const { return directory_->isValid(); }
bool LinuxGifFrames::reset() {
    if(exporting() || writing_) return false;
    const QDir dir(directory_->path());
    for(const auto &name:dir.entryList(QDir::Files)) if(!QFile::remove(dir.filePath(name))) return false;
    frames_.clear(); size_={}; bytes_=0; duration_=0; finishing_=ready_=false; return true;
}
QString LinuxGifFrames::ffmpegPath() { return QStandardPaths::findExecutable(QStringLiteral("ffmpeg")); }
bool LinuxGifFrames::append(const QImage &image, qint64 milliseconds) {
    if (!valid() || finishing_ || writing_ || image.isNull() || milliseconds < 0
        || (!frames_.isEmpty() && (image.size() != size_ || milliseconds <= frames_.last().milliseconds))) return false;
    if (bytes_ >= 2LL * 1024 * 1024 * 1024 || frames_.size() >= 3600) {
        emit failed(QStringLiteral("录制缓存已达到上限（2 GiB / 3600 帧），请停止并保存。")); return false;
    }
    size_ = image.size(); writing_ = true;
    const Frame frame{QStringLiteral("frame_%1.png").arg(frames_.size(), 6, 10, QLatin1Char('0')), milliseconds};
    const auto directory = directory_;
    // 一次只接收一个写盘任务，采集器可以丢掉来不及处理的帧，但不能积压原始图像。
    // A single write in flight provides backpressure instead of queuing raw frames.
    writer_.setFuture(QtConcurrent::run([directory, image, frame] {
        const QString path = QDir(directory->path()).filePath(frame.name);
        const bool ok = image.save(path, "PNG");
        return WriteResult{frame, QFileInfo(path).size(), ok};
    }));
    return true;
}
void LinuxGifFrames::writeFinished() {
    const auto result = writer_.result(); writing_ = false;
    if (result.ok) { frames_.append(result.frame); bytes_ += result.bytes; }
    else emit failed(QStringLiteral("无法写入 GIF 临时帧，请检查临时目录空间。"));
    if (finishing_ && !ready_) { ready_ = true; emit ready(); }
}
void LinuxGifFrames::finish(qint64 duration) {
    if (finishing_) return;
    finishing_ = true; duration_ = std::max<qint64>(duration, 10);
    if (!writing_) { ready_ = true; emit ready(); }
}
QImage LinuxGifFrames::image(int index) const {
    return index >= 0 && index < frames_.size() ? QImage(QDir(directory_->path()).filePath(frames_[index].name)) : QImage();
}
bool LinuxGifFrames::prepareExport(const QString &path,int first,int last,double speed,double scale) {
    if (!ready_ || exporting() || path.isEmpty() || first < 0 || last < first || last >= frames_.size()
        || !std::isfinite(speed) || speed < 0.25 || speed > 4 || !std::isfinite(scale) || scale < 0.1 || scale > 1) return false;
    if (ffmpegPath().isEmpty()) { emit failed(QStringLiteral("缺少 FFmpeg。请安装发行版提供的 ffmpeg 软件包。")); return false; }
    QFile manifest(QDir(directory_->path()).filePath(QStringLiteral("frames.ffconcat")));
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) { emit failed(QStringLiteral("无法创建 GIF 编码帧清单。")); return false; }
    QByteArray list("ffconcat version 1.0\n");
    exportDurationUs_ = 0;
    for (int i = first; i <= last; ++i) {
        // 文件名由本类生成，不接受用户输入；小数点固定为 C 格式，避免区域设置破坏时长。
        // Only generated filenames enter the manifest; decimal timing is locale independent.
        const qint64 end = i + 1 < frames_.size() ? frames_[i + 1].milliseconds : duration_;
        const double delay = std::max(0.01, double(end - frames_[i].milliseconds) / 1000 / speed);
        exportDurationUs_ += std::llround(delay * 1000000);
        list += "file '" + frames_[i].name.toUtf8() + "'\noption framerate 100\nduration " + QByteArray::number(delay, 'f', 6) + '\n';
        finalDelay_ = std::clamp(int(std::lround(delay * 100)), 1, 65535);
    }
    if (manifest.write(list) != list.size()) { emit failed(QStringLiteral("写入 GIF 帧清单失败。")); return false; }
    manifest.close(); target_ = path; exportScale_ = scale; diagnostic_.clear(); return true;
}
bool LinuxGifFrames::exportGif(const QString &path,int first,int last,double speed,double scale,Quality quality) {
    if(!prepareExport(path,first,last,speed,scale)) return false;
    video_=false; resultFile_=QStringLiteral("result.gif"); quality_=quality;
    stage_ = quality == Quality::High ? Stage::Gif : Stage::Palette;
    encode(); return true;
}
bool LinuxGifFrames::exportMp4(const QString &path,const jietuba::RecordingOptions &options,const QString &audioPath) {
    try {options.validate();} catch(const std::exception &) {return false;}
    if(!audioPath.isEmpty() && !QFileInfo::exists(audioPath)) return false;
    if(!prepareExport(path,0,int(frames_.size())-1,1.,1.)) return false;
    // concat 的末帧需额外结束时间点，MP4 再按实际录制时长截断，避免末帧只有一格时长。
    // Add the terminal timestamp, then trim MP4 to real active duration so the last frame holds correctly.
    QFile manifest(QDir(directory_->path()).filePath(QStringLiteral("frames.ffconcat")));
    if(!manifest.open(QIODevice::Append)) {emit failed(QStringLiteral("无法补全视频帧清单。")); return false;}
    const auto terminal="file '"+frames_.last().name.toUtf8()+"'\noption framerate 100\n";
    if(manifest.write(terminal)!=terminal.size()) {emit failed(QStringLiteral("写入视频末帧失败。")); return false;}
    manifest.close(); video_=true; videoOptions_=options; audioPath_=audioPath; resultFile_="result.mp4";
    codec_="libx264"; device_.clear();
    if(options.hardware) {
        const QDir dri(QStringLiteral("/dev/dri"));
        const auto devices=dri.entryInfoList({QStringLiteral("renderD*")},QDir::System|QDir::Files|QDir::Readable|QDir::Writable);
        if(!devices.isEmpty()) {codec_="h264_vaapi"; device_=devices.first().absoluteFilePath();}
        else if(QFileInfo::exists(QStringLiteral("/dev/nvidia0"))) codec_="h264_nvenc";
    }
    stage_=Stage::Mp4;
    exportDurationUs_ = std::max<qint64>(10, duration_ - frames_.first().milliseconds) * 1000;
    encode(); return true;
}
void LinuxGifFrames::encode() {
    progressBuffer_.clear();
    exportPercent_ = -1;
    progressMessage_ = video_ ? QStringLiteral("正在编码 MP4…")
        : stage_ == Stage::Palette ? QStringLiteral("正在生成 GIF 调色板…") : QStringLiteral("正在编码 GIF…");
    emit exportProgress(progressMessage_, -1);
    process_.setWorkingDirectory(directory_->path());
    // 全局调色板分两遍处理；局部调色板 stats_mode=single 每帧即产即用，不积压整段视频。
    // Global palettes use two passes; single-frame palettes stream immediately without retaining the clip.
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostdin"),
                     QStringLiteral("-nostats"), QStringLiteral("-progress"), QStringLiteral("pipe:1"),
                     QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
                     QStringLiteral("-i"), QStringLiteral("frames.ffconcat")};
    if(video_) {
        if(!audioPath_.isEmpty()) args<<"-i"<<audioPath_<<"-map"<<"0:v:0"<<"-map"<<"1:a:0"<<"-af"<<"aresample=async=1:first_pts=0,apad"<<"-c:a"<<"aac"<<"-b:a"<<"192k";
        else args<<"-an";
        // 显式补足末帧并按目标帧率生成 CFR 帧，不能仅依靠输出 -r：稀疏采集的末帧
        // 在部分 FFmpeg 版本会提前结束。最终 -t 仍严格截断到有效录制时长。
        // Hold the terminal frame and generate CFR timestamps in the filter, rather than relying
        // on output -r alone. Sparse inputs can otherwise end early; -t trims the added hold.
        QString filter=QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2,tpad=stop_mode=clone:stop_duration=1,fps=%1").arg(videoOptions_.fps);
        if(codec_=="h264_vaapi") {args<<"-vaapi_device"<<device_; filter+=",format=nv12,hwupload";}
        args<<"-vf"<<filter<<"-c:v"<<codec_<<"-b:v"<<QString::number(videoOptions_.bitrate);
        if(codec_=="libx264") args<<"-preset"<<"veryfast"<<"-pix_fmt"<<"yuv420p";
        else if(codec_=="h264_nvenc") args<<"-pix_fmt"<<"yuv420p";
        args<<"-r"<<QString::number(videoOptions_.fps)<<"-fps_mode"<<"cfr"<<"-t"<<QByteArray::number(std::max<qint64>(10,duration_-frames_.first().milliseconds)/1000.,'f',3)
            <<"-movflags"<<"+faststart"<<resultFile_;
        process_.start(ffmpegPath(),args); return;
    }
    // 100% 尺寸完全跳过重采样，始终在 RGB 空间量化，避免文字彩边和不必要的格式转换。
    // At 100% skip resampling entirely; keep RGB throughout quantization for sharp text edges.
    const QString scale = exportScale_ == 1 ? QStringLiteral("format=rgb24")
        : QStringLiteral("scale=max(1\\,trunc(iw*%1)):max(1\\,trunc(ih*%1)):flags=lanczos,format=rgb24").arg(exportScale_, 0, 'f', 3);
    if (quality_ == Quality::High) {
        // 每帧可用完整 256 色。关闭基于透明索引的帧差分，防止局部调色板换色后出现残影。
        // Use all 256 colors per frame. Disable transparent-index differencing across palette changes.
        args << QStringLiteral("-lavfi")
             << QStringLiteral("[0:v]%1,split[image][colors];[colors]palettegen=stats_mode=single:reserve_transparent=0[palette];"
                               "[image][palette]paletteuse=new=1:dither=sierra2_4a").arg(scale)
             << QStringLiteral("-gifflags") << QStringLiteral("-transdiff");
    } else if (stage_ == Stage::Palette) {
        // 全帧统计包含末帧的颜色；diff 模式在部分版本/纯色切换中会漏掉最后一帧。
        // Full statistics retain final-frame colors, including abrupt solid-color changes.
        args << QStringLiteral("-vf") << scale + QStringLiteral(",palettegen=stats_mode=full")
             << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-threads") << QStringLiteral("1")
             << QStringLiteral("-update") << QStringLiteral("1") << QStringLiteral("palette.png");
    } else {
        args << QStringLiteral("-i") << QStringLiteral("palette.png") << QStringLiteral("-lavfi")
             << QStringLiteral("[0:v]%1[v];[v][1:v]paletteuse=dither=sierra2_4a").arg(scale);
    }
    if (stage_ == Stage::Gif)
        args << QStringLiteral("-fps_mode") << QStringLiteral("vfr") << QStringLiteral("-loop") << QStringLiteral("0")
             << QStringLiteral("-final_delay") << QString::number(finalDelay_) << QStringLiteral("result.gif");
    process_.start(ffmpegPath(), args);
}
void LinuxGifFrames::readProgress() {
    progressBuffer_.append(process_.readAllStandardOutput());
    // FFmpeg 的输出时间戳对应已编码的片段；原子写盘结束前不显示 100%。
    // FFmpeg timestamps measure actual encoding; reserve completion until the atomic save commits.
    qsizetype end;
    while ((end = progressBuffer_.indexOf('\n')) >= 0) {
        const auto line = progressBuffer_.left(end).trimmed();
        progressBuffer_.remove(0, end + 1);
        if ((stage_ != Stage::Gif && stage_ != Stage::Mp4) || !line.startsWith("out_time_us=") || exportDurationUs_ <= 0)
            continue;
        bool ok = false;
        const auto time = line.mid(12).toLongLong(&ok);
        if (!ok) continue;
        const auto bounded = std::clamp<qint64>(time, 0, exportDurationUs_);
        const int percent = int(bounded * 99 / exportDurationUs_);
        if (percent <= exportPercent_) continue;
        exportPercent_ = percent;
        emit exportProgress(progressMessage_, percent);
    }
}
void LinuxGifFrames::processFinished(int code, QProcess::ExitStatus status) {
    readProgress();
    if (!exporting()) return;
    if (stage_ == Stage::Cancelling) {
        stage_ = Stage::Idle; emit exportProgress(QStringLiteral("已取消导出，录制内容已保留。"), -1); return;
    }
    if(status!=QProcess::NormalExit || code!=0) {
        // 无损源帧仍在，硬件不可用时可安全重新编码，不丢画面、不修改已保存的目标文件。
        // Lossless source frames permit safe software retry if hardware fails, preserving the destination.
        if(video_ && codec_!="libx264") {codec_="libx264"; device_.clear(); diagnostic_.clear(); encode(); return;}
        exportFailed(QStringLiteral("%1 编码失败：\n%2").arg(video_ ? "MP4" : "GIF",diagnostic_)); return;
    }
    if (stage_ == Stage::Palette) { stage_ = Stage::Gif; encode(); return; }
    stage_ = Stage::Copy;
    const auto directory = directory_; const QString target = target_,resultFile=resultFile_;
    emit exportProgress(video_ ? QStringLiteral("正在保存 MP4…") : QStringLiteral("正在保存 GIF…"), 99);
    copier_.setFuture(QtConcurrent::run([directory, target,resultFile] {
        QFile source(QDir(directory->path()).filePath(resultFile));
        QSaveFile output(target);
        if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return QStringLiteral("无法写入：%1").arg(target);
        while (!source.atEnd()) {
            const QByteArray block = source.read(1024 * 1024);
            if (block.isEmpty() || output.write(block) != block.size()) return QStringLiteral("保存 GIF 时发生读写错误，原文件未被替换。");
        }
        return output.commit() ? QString() : QStringLiteral("保存 GIF 失败：%1").arg(output.errorString());
    }));
}
void LinuxGifFrames::exportFailed(const QString &reason) { stage_ = Stage::Idle; emit failed(reason); }
void LinuxGifFrames::cancelExport() {
    // 提交文件的短暂阶段不支持中断，避免向调用方误报“已取消但文件仍被替换”。
    // The atomic copy/commit stage is not cancellable, so completion stays unambiguous.
    if (stage_ == Stage::Idle || stage_ == Stage::Copy) return;
    stage_ = Stage::Cancelling; process_.kill();
}
