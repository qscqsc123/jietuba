#include "speech_dispatcher.h"
#include <QCoreApplication>
#include <QLocale>
#include <QTimer>
#include <cstdio>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const QList<LinuxSpeechVoice> voices{{"espeak-ng","Cantonese","yue"},{"espeak-ng","Mandarin","cmn"},
        {"espeak-ng","English Caribbean","en-029"},{"espeak-ng","English America","en-us"},
        {"espeak-ng","Japanese","ja"},{"dummy","Fake English","en-us"}};
    if(LinuxSpeechDispatcher::chooseVoice(voices,QStringLiteral("中文测试"))!=1
        || LinuxSpeechDispatcher::chooseVoice(voices,"hello")!=3
        || LinuxSpeechDispatcher::chooseVoice(voices,QStringLiteral("日本語かな"))!=4
        || LinuxSpeechDispatcher::chooseVoice({voices[5]},"hello")!=-1
        || LinuxSpeechDispatcher::chooseVoice({voices[3]},QStringLiteral("中文"))!=-1) return 1;
    if(!app.arguments().contains("--live-muted")) return 0;
    if(!LinuxSpeechDispatcher::supported()) return 77;
    // 显式手动启用的真实服务探测，音量固定为零；验证中英文配置及开始/结束通知，不产生可听声音。
    // Explicit opt-in live check at zero volume: configure both languages and verify lifecycle without audible speech.
    LinuxSpeechDispatcher speech;
    const auto original=QLocale().name(); int phase=0,completed=0; bool began=false;
    const auto run=[&] {
        const auto text=phase==0 ? QStringLiteral("test") : QStringLiteral("测试");
        if(!speech.prepare(text)) {std::fprintf(stderr,"prepare failed: %s\n",qPrintable(speech.error())); app.exit(2); return;}
        const auto voice=speech.voice(); std::printf("voice module=%s language=%s name=%s\n",qPrintable(voice.module),qPrintable(voice.language),qPrintable(voice.name));
        if(!speech.speak(text,0)) {std::fprintf(stderr,"speak failed: %s\n",qPrintable(speech.error())); app.exit(3);}
    };
    QObject::connect(&speech,&LinuxSpeechDispatcher::activeChanged,&app,[&](bool active) {
        if(active) {began=true; return;}
        if(!began) return;
        began=false; ++completed; ++phase;
        if(phase==2) {app.exit(completed==2 && QLocale().name()==original ? 0 : 4); return;}
        QTimer::singleShot(0,&app,run);
    });
    QTimer::singleShot(10000,&app,[&] {std::fprintf(stderr,"speech lifecycle timeout\n"); app.exit(5);});
    QTimer::singleShot(0,&app,run); return app.exec();
}
