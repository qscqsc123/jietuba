#include "gif_frames.h"
#include <QFile>
#include <QImageReader>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>

/**
 * 使用真实 FFmpeg 编码和 Qt GIF 解码验证帧顺序、真实时长、裁剪、缩放与失败重试。
 * 所有画面均为合成色块；不访问真实桌面，不修改用户设置或保存目录。
 * Real FFmpeg encoding and Qt decoding check order, timing, trim, scale and retry.
 * Synthetic frames only: no desktop access or user configuration/output changes.
 */
class GifTest final : public QObject {
    Q_OBJECT
private slots:
    void mp4AudioTimingAndPadding() {
        if(LinuxGifFrames::ffmpegPath().isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()) QSKIP("FFmpeg/ffprobe not installed");
        QTemporaryDir output; LinuxGifFrames store;
        QSignalSpy exported(&store,&LinuxGifFrames::exported),errors(&store,&LinuxGifFrames::failed);
        const QList<QColor> colors{Qt::red,Qt::green,Qt::blue}; const QList<int> times{0,120,400};
        for(int i=0;i<3;++i) {QImage image(35,27,QImage::Format_RGB32); image.fill(colors[i]); QVERIFY(store.append(image,times[i])); QTRY_COMPARE(store.frames().size(),i+1);}
        store.finish(700);
        // 使用合成音调，不访问真实声音设备；验证真正的 H.264/AAC、奇数尺寸补边和末帧时长。
        // Synthetic tone only: verify real H.264/AAC, odd-dimension padding and terminal-frame duration.
        const auto audio=output.filePath("audio.wav"); QProcess tone;
        tone.start(LinuxGifFrames::ffmpegPath(),{"-v","error","-nostdin","-y","-f","lavfi","-i","sine=frequency=600:sample_rate=48000","-t","0.7","-c:a","pcm_s16le",audio});
        QVERIFY(tone.waitForFinished(5000)); QCOMPARE(tone.exitCode(),0);
        jietuba::RecordingOptions options; options.fps=24; options.hardware=true; options.system_audio=true;
        const auto path=output.filePath(QStringLiteral("视频 空格.mp4"));
        QVERIFY(store.exportMp4(path,options,audio)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000);
        if(!errors.isEmpty()) qWarning()<<errors.last().first().toString();
        QCOMPARE(errors.size(),0); QCOMPARE(exported.size(),1);
        QProcess probe; probe.start(QStandardPaths::findExecutable("ffprobe"),{"-v","error","-show_streams","-show_format","-of","json",path});
        QVERIFY(probe.waitForFinished(5000)); QCOMPARE(probe.exitCode(),0);
        const auto info=QJsonDocument::fromJson(probe.readAllStandardOutput()).object(); bool video=false,sound=false;
        for(const auto &value:info["streams"].toArray()) {const auto stream=value.toObject();
            if(stream["codec_type"]=="video") {video=true; QCOMPARE(stream["codec_name"].toString(),QString("h264")); QCOMPARE(stream["width"].toInt(),36); QCOMPARE(stream["height"].toInt(),28); QCOMPARE(stream["avg_frame_rate"].toString(),QString("24/1"));}
            if(stream["codec_type"]=="audio") {sound=true; QCOMPARE(stream["codec_name"].toString(),QString("aac"));}
        }
        QVERIFY(video && sound); QVERIFY(std::abs(info["format"].toObject()["duration"].toString().toDouble()-.7)<.08);
        QProcess decode; decode.start(LinuxGifFrames::ffmpegPath(),{"-v","error","-ss","0.55","-i",path,"-frames:v","1","-vcodec","png","-f","image2pipe","pipe:1"});
        QVERIFY(decode.waitForFinished(5000)); QCOMPARE(decode.exitCode(),0);
        const auto last=QImage::fromData(decode.readAllStandardOutput());
        QVERIFY2(!last.isNull(),decode.readAllStandardError().constData()); QVERIFY(last.pixelColor(12,12).blue()>230);
        options.fps=0; QVERIFY(!store.exportMp4(path,options));
        const auto gif=output.filePath("after-video.gif"); QVERIFY(store.exportGif(gif,0,2)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000); QCOMPARE(QImageReader(gif).imageCount(),3);
        QVERIFY(store.reset()); QCOMPARE(store.frames().size(),0);
    }
    void encodeTimingTrimAndRetry() {
        if (LinuxGifFrames::ffmpegPath().isEmpty()) QSKIP("FFmpeg not installed");
        QTemporaryDir output; QVERIFY(output.isValid());
        LinuxGifFrames store; QVERIFY(store.valid());
        QSignalSpy ready(&store, &LinuxGifFrames::ready), errors(&store, &LinuxGifFrames::failed), exported(&store, &LinuxGifFrames::exported);
        QSignalSpy progress(&store, &LinuxGifFrames::exportProgress);
        const QList<QColor> colors{Qt::red,Qt::green,Qt::blue};
        const QList<int> timestamps{0,120,400};
        for (int i=0;i<3;++i) {
            QImage frame(34,26,QImage::Format_RGB32); frame.fill(colors[i]);
            QVERIFY(store.append(frame,timestamps[i]));
            // 背压不会保存并发帧；测试等待真正的写盘结束再推进时间。
            // Backpressure rejects concurrent writes; advance after real persistence.
            QTRY_COMPARE(store.frames().size(), i+1);
        }
        QImage wrong(10,10,QImage::Format_RGB32); wrong.fill(Qt::black);
        QVERIFY(!store.append(wrong,500)); QVERIFY(!store.append(store.image(0),400));
        store.finish(700); QCOMPARE(ready.size(),1); QVERIFY(!store.append(store.image(0),800));
        const QString path = output.filePath(QStringLiteral("真实动画 空格.gif"));
        QVERIFY(store.exportGif(path,0,2));
        QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000);
        if (!errors.isEmpty()) qWarning().noquote() << errors.last().first().toString();
        QCOMPARE(errors.size(),0); QCOMPARE(exported.size(),1);
        QVERIFY(progress.size() >= 3);
        QCOMPARE(progress.first().at(1).toInt(), -1);
        QVERIFY(progress.at(1).at(1).toInt() >= 0);
        QCOMPARE(progress.last().at(1).toInt(), 99);
        QImageReader gif(path); QCOMPARE(gif.size(),QSize(34,26)); QVERIFY(gif.supportsAnimation());
        QCOMPARE(gif.imageCount(),3); int duration=0;
        for (int i=0;i<3;++i) {
            const QImage image=gif.read(); QVERIFY(!image.isNull());
            QCOMPARE(image.pixelColor(15,12).rgb(),colors[i].rgb());
            duration+=gif.nextImageDelay();
        }
        QVERIFY2(std::abs(duration-700)<=20,qPrintable(QStringLiteral("actual GIF duration %1").arg(duration)));
        QVERIFY(!store.exportGif(path,-1,2)); QVERIFY(!store.exportGif(path,2,1));
        const QString trimmed = output.filePath(QStringLiteral("trim.gif"));
        QVERIFY(store.exportGif(trimmed,1,2,2.,.5)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000);
        QCOMPARE(errors.size(),0); QCOMPARE(exported.size(),2);
        QImageReader trim(trimmed); QCOMPARE(trim.size(),QSize(17,13)); QCOMPARE(trim.imageCount(),2);
        duration=0;
        for (int i=1;i<3;++i) { const QImage image=trim.read(); QCOMPARE(image.pixelColor(8,6).rgb(),colors[i].rgb()); duration+=trim.nextImageDelay(); }
        QVERIFY(std::abs(duration-290)<=20);
        // 非法目标目录保存失败，但原帧保留；之后可重新另存。
        // A failed destination keeps source frames available for another export.
        QVERIFY(store.exportGif(output.filePath(QStringLiteral("missing/out.gif")),0,2));
        QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000); QCOMPARE(errors.size(),1); QCOMPARE(store.frames().size(),3);
        QVERIFY(store.exportGif(path,0,2)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000); QCOMPARE(exported.size(),3);
    }
    void pendingFrameDrainAndCancel() {
        if (LinuxGifFrames::ffmpegPath().isEmpty()) QSKIP("FFmpeg not installed");
        QTemporaryDir output; LinuxGifFrames store;
        QSignalSpy ready(&store,&LinuxGifFrames::ready), exported(&store,&LinuxGifFrames::exported), errors(&store,&LinuxGifFrames::failed);
        QImage frame(33,25,QImage::Format_RGB32); frame.fill(Qt::yellow);
        QVERIFY(store.append(frame,0)); store.finish(500);
        QTRY_COMPARE(ready.size(),1); QCOMPARE(store.frames().size(),1);
        const QString path=output.filePath(QStringLiteral("cancel.gif"));
        QFile existing(path); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("original"); existing.close();
        QVERIFY(store.exportGif(path,0,0)); store.cancelExport();
        QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),5000); QCOMPARE(exported.size(),0); QCOMPARE(errors.size(),0);
        QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(),QByteArray("original")); existing.close();
        QVERIFY(store.exportGif(path,0,0)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000);
        QCOMPARE(exported.size(),1); QImageReader gif(path); QCOMPARE(gif.size(),QSize(33,25));
        QVERIFY(!gif.read().isNull()); QCOMPARE(gif.nextImageDelay(),500);
    }
    void perFramePaletteFidelity() {
        if (LinuxGifFrames::ffmpegPath().isEmpty()) QSKIP("FFmpeg not installed");
        QTemporaryDir output; LinuxGifFrames store;
        QSignalSpy errors(&store,&LinuxGifFrames::failed);
        QList<QImage> originals;
        // 每帧恰好 256 色，但整段超过 256 色；逐像素检查颜色误差，不能丢末帧或残影。
        // Each frame has 256 colors, but the clip exceeds that; check per-pixel errors and stale pixels.
        for (int i=0;i<3;++i) {
            QImage frame(256,64,QImage::Format_RGB32);
            for (int y=0;y<64;++y) for (int x=0;x<256;++x) {
                const int tone=(y<32 ? x : 255-x);
                frame.setPixel(x,y,i==0 ? qRgb(tone,0,0) : i==1 ? qRgb(0,tone,0) : qRgb(0,0,tone));
            }
            originals.append(frame); QVERIFY(store.append(frame,i*100)); QTRY_COMPARE(store.frames().size(),i+1);
        }
        store.finish(300);
        const QString high=output.filePath("high.gif"),compact=output.filePath("compact.gif");
        QVERIFY(store.exportGif(high,0,2)); QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000);
        if (!errors.isEmpty()) qWarning().noquote()<<errors.last().first().toString();
        QCOMPARE(errors.size(),0);
        QImageReader precise(high); QCOMPARE(precise.imageCount(),3); QCOMPARE(precise.size(),originals.first().size());
        qint64 highError=0;
        for (int i=0;i<3;++i) {
            const QImage decoded=precise.read().convertToFormat(QImage::Format_RGB32);
            QVERIFY(!decoded.isNull()); QCOMPARE(store.image(i).convertToFormat(QImage::Format_RGB32),originals[i]);
            for (int y=0;y<64;++y) for (int x=0;x<256;++x) {
                const QColor actual=decoded.pixelColor(x,y),expected=originals[i].pixelColor(x,y);
                const int dr=actual.red()-expected.red(),dg=actual.green()-expected.green(),db=actual.blue()-expected.blue();
                // FFmpeg 的感知色彩空间往返存在少量取整误差，不能误称 GIF 无损。
                // Perceptual color-space rounding is small but GIF must not be called lossless.
                QVERIFY(std::abs(dr)<=4 && std::abs(dg)<=4 && std::abs(db)<=4); QCOMPARE(actual.alpha(),255);
                highError+=qint64(dr)*dr+qint64(dg)*dg+qint64(db)*db;
            }
        }
        QVERIFY(store.exportGif(compact,0,2,1,1,LinuxGifFrames::Quality::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(!store.exporting(),15000); QCOMPARE(errors.size(),0);
        QImageReader shared(compact); QCOMPARE(shared.imageCount(),3); qint64 squaredError=0;
        for (int i=0;i<3;++i) {
            const QImage decoded=shared.read(); QVERIFY(!decoded.isNull());
            for (int y=0;y<64;++y) for (int x=0;x<256;++x) {
                const QColor actual=decoded.pixelColor(x,y),expected=originals[i].pixelColor(x,y);
                squaredError+=qint64(actual.red()-expected.red())*(actual.red()-expected.red())
                    +qint64(actual.green()-expected.green())*(actual.green()-expected.green())
                    +qint64(actual.blue()-expected.blue())*(actual.blue()-expected.blue());
            }
        }
        QVERIFY2(highError*4<squaredError,"Local palettes must materially improve on shared-palette quantization");
        // 新默认路径的误差必须显著低于全局调色板，而非仅改参数但画质无变化。
        // Verify measurable improvement, not just a change of encoder arguments.
    }
};
QTEST_MAIN(GifTest)
#include "gif_test.moc"
