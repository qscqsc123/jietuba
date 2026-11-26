#include "recording_audio.h"
#include <QTest>
#include <QSignalSpy>
#include <QFile>
#include <QtEndian>
#include <gst/gst.h>
/**
 * 合成音频验证暂停/恢复、EOS 与 WAV 时长，不连接真实播放设备或麦克风。
 * Synthetic audio verifies pause/resume, EOS and WAV duration without desktop audio access.
 */
class RecordingAudioTest final : public QObject {
    Q_OBJECT
private slots:
    void pauseAndFinalize() {
        gst_init(nullptr,nullptr);
        for(const char *name:{"audiotestsrc","wavenc","audioconvert","audioresample"}) {
            auto *factory=gst_element_factory_find(name); if(!factory) QSKIP("GStreamer audio test plugins are unavailable"); gst_object_unref(factory);
        }
        LinuxRecordingAudio audio(nullptr,"audiotestsrc");
        QSignalSpy ready(&audio,&LinuxRecordingAudio::finished),errors(&audio,&LinuxRecordingAudio::failed);
        QVERIFY(audio.start()); QTest::qWait(150); audio.setPaused(true); QTest::qWait(400); audio.setPaused(false); QTest::qWait(150); audio.finish();
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000); QCOMPARE(errors.size(),0);
        QFile wav(ready.first().first().toString()); QVERIFY(wav.open(QIODevice::ReadOnly)); const auto bytes=wav.readAll();
        QVERIFY(bytes.startsWith("RIFF")); QVERIFY(bytes.mid(8,4)=="WAVE");
        int position=12,pcmBytes=0;
        while(position+8<=bytes.size()) {
            const auto size=qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()+position+4));
            if(bytes.mid(position,4)=="data") {pcmBytes=int(size); break;} position+=8+int(size)+(size%2);
        }
        QVERIFY(pcmBytes>48000*4*.1); QVERIFY(pcmBytes<48000*4*.55);
    }
};
QTEST_GUILESS_MAIN(RecordingAudioTest)
#include "recording_audio_test.moc"
