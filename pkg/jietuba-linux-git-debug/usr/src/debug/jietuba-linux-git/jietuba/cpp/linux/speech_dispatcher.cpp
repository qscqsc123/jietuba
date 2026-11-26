#include "speech_dispatcher.h"
#include <QLibrary>
#include <QLocale>
#include <QMutex>
#include <QHash>
#include <algorithm>
#include <cstdlib>
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
#include <libspeechd.h>
namespace {
QMutex &clientsMutex() {static QMutex mutex; return mutex;}
QHash<size_t,LinuxSpeechDispatcher *> &clients() {static QHash<size_t,LinuxSpeechDispatcher *> map; return map;}
void notify(size_t message,size_t client,SPDNotificationType state) {
    const QMutexLocker lock(&clientsMutex());
    if(auto *owner=clients().value(client)) QMetaObject::invokeMethod(owner,[owner,message,state] {owner->notification(message,int(state));},Qt::QueuedConnection);
}
}
#endif
namespace {
QString language(const QString &code) {
    auto tag=code.toLower().replace('_','-');
    // eSpeak NG 使用 cmn / yue，不能让 QLocale 把未知 ISO 标签错误映射到 C。
    // Preserve service aliases explicitly instead of mapping unknown ISO tags through QLocale::C.
    if(tag=="cmn" || tag.startsWith("cmn-") || tag=="yue" || tag.startsWith("yue-")) return "zh";
    return tag.section('-',0,0);
}
QString textLanguage(const QString &text) {
    bool han=false,latin=false,cyrillic=false,arabic=false;
    for(const auto c:text) {
        if(c.script()==QChar::Script_Hiragana || c.script()==QChar::Script_Katakana) return "ja";
        if(c.script()==QChar::Script_Hangul) return "ko";
        han|=c.script()==QChar::Script_Han; latin|=c.script()==QChar::Script_Latin;
        cyrillic|=c.script()==QChar::Script_Cyrillic; arabic|=c.script()==QChar::Script_Arabic;
    }
    if(han) return "zh";
    if(cyrillic) return "ru";
    if(arabic) return "ar";
    if(latin) return "en";
    auto system=language(QLocale::system().name()); return system=="c" ? QStringLiteral("en") : system;
}
}
struct LinuxSpeechDispatcher::Impl {
    QList<LinuxSpeechVoice> voices;
    int message=-1,client=-1;
    bool autoSpawn=true;
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    QLibrary library{QStringLiteral("speechd"),2};
    SPDConnection *connection=nullptr;
#define SPEECH_API(member,symbol) decltype(&symbol) member=nullptr;
    SPEECH_API(open,spd_open2) SPEECH_API(close,spd_close) SPEECH_API(clientId,spd_get_client_id)
    SPEECH_API(modules,spd_list_modules) SPEECH_API(freeModules,free_spd_modules)
    SPEECH_API(setModule,spd_set_output_module) SPEECH_API(listVoices,spd_list_synthesis_voices)
    SPEECH_API(freeVoices,free_spd_voices) SPEECH_API(setLanguage,spd_set_language)
    SPEECH_API(setVoice,spd_set_synthesis_voice) SPEECH_API(setVolume,spd_set_volume)
    SPEECH_API(say,spd_say) SPEECH_API(cancel,spd_cancel) SPEECH_API(notifyOn,spd_set_notification_on)
#undef SPEECH_API
    bool load() {
        if(!library.load()) return false;
#define LOAD_API(member,symbol) member=reinterpret_cast<decltype(member)>(library.resolve(#symbol)); if(!member) return false;
        LOAD_API(open,spd_open2) LOAD_API(close,spd_close) LOAD_API(clientId,spd_get_client_id)
        LOAD_API(modules,spd_list_modules) LOAD_API(freeModules,free_spd_modules)
        LOAD_API(setModule,spd_set_output_module) LOAD_API(listVoices,spd_list_synthesis_voices)
        LOAD_API(freeVoices,free_spd_voices) LOAD_API(setLanguage,spd_set_language)
        LOAD_API(setVoice,spd_set_synthesis_voice) LOAD_API(setVolume,spd_set_volume)
        LOAD_API(say,spd_say) LOAD_API(cancel,spd_cancel) LOAD_API(notifyOn,spd_set_notification_on)
#undef LOAD_API
        return true;
    }
#endif
};
LinuxSpeechDispatcher::LinuxSpeechDispatcher(QObject *parent):QObject(parent),d_(std::make_unique<Impl>()) {}
LinuxSpeechDispatcher::~LinuxSpeechDispatcher() {closeConnection();}
bool LinuxSpeechDispatcher::supported() {
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    return true;
#else
    return false;
#endif
}
int LinuxSpeechDispatcher::chooseVoice(const QList<LinuxSpeechVoice> &voices,const QString &text) {
    const auto desired=textLanguage(text); int best=-1,score=-1;
    for(int i=0;i<voices.size();++i) {
        const auto &v=voices[i]; if(v.name.isEmpty() || v.module.startsWith("dummy") || language(v.language)!=desired) continue;
        int rank=(v.module=="espeak-ng" || v.module=="espeak") ? 20 : 0;
        if(!v.name.contains('+')) rank+=10;
        const auto tag=v.language.toLower().replace('_','-');
        if(desired=="zh" && (tag=="cmn" || tag=="zh" || tag=="zh-cn")) rank+=8;
        if(desired=="en" && tag=="en-us") rank+=8;
        if(rank>score) {score=rank; best=i;}
    }
    return best;
}
bool LinuxSpeechDispatcher::initialize() {
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    if(d_->connection) return true;
    if(!d_->load()) {error_=QStringLiteral("无法加载 libspeechd：")+d_->library.errorString(); return false;}
    char *detail=nullptr;
    d_->connection=d_->open("jietuba","translation",nullptr,SPD_MODE_THREADED,nullptr,int(d_->autoSpawn),&detail);
    if(!d_->connection) {error_=QStringLiteral("无法连接用户语音服务：")+QString::fromUtf8(detail ? detail : "Speech Dispatcher unavailable"); std::free(detail); return false;}
    std::free(detail);
    d_->client=d_->clientId(d_->connection);
    if(d_->client<0) {error_=QStringLiteral("无法取得语音客户端标识。"); closeConnection(); return false;}
    {const QMutexLocker lock(&clientsMutex()); clients().insert(size_t(d_->client),this);}
    d_->connection->callback_begin=notify; d_->connection->callback_end=notify; d_->connection->callback_cancel=notify;
    if(d_->notifyOn(d_->connection,SPD_BEGIN)<0 || d_->notifyOn(d_->connection,SPD_END)<0 || d_->notifyOn(d_->connection,SPD_CANCEL)<0) {
        error_=QStringLiteral("语音服务不支持播放状态通知。"); closeConnection(); return false;
    }
    char **modules=d_->modules(d_->connection);
    for(int m=0;modules && modules[m];++m) {
        const auto module=QString::fromUtf8(modules[m]); if(module.startsWith("dummy") || d_->setModule(d_->connection,modules[m])<0) continue;
        auto **voices=d_->listVoices(d_->connection);
        for(int i=0;voices && voices[i];++i) {
            const auto *v=voices[i];
            // 仅保留基础声音；eSpeak 会枚举数万种变声组合，没必要全部复制到 UI。
            // Keep base voices instead of copying tens of thousands of synthetic variants.
            if(v->variant && *v->variant && QByteArray(v->variant)!="none") continue;
            d_->voices.append({module,QString::fromUtf8(v->name),QString::fromUtf8(v->language)});
        }
        if(voices) d_->freeVoices(voices);
    }
    if(modules) d_->freeModules(modules);
    if(d_->voices.isEmpty()) {error_=QStringLiteral("服务未提供可用声音，请检查语音包与输出模块。"); closeConnection(); return false;}
    return true;
#else
    error_=QStringLiteral("此构建未启用 Speech Dispatcher 适配器。"); return false;
#endif
}
bool LinuxSpeechDispatcher::prepare(const QString &text) {
    error_.clear(); if(!initialize()) return false;
    const int index=chooseVoice(d_->voices,text);
    if(index<0) {error_=QStringLiteral("未安装适合当前文字语言的声音。"); return false;}
    voice_=d_->voices[index];
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    // 原样使用服务给出的语言和声音名称，包括 cmn；不依赖用户的 LANG/LC_ALL 或修改全局 locale。
    // Use exact service identifiers, including cmn, without changing LANG/LC_ALL or global Qt locale.
    if(d_->setModule(d_->connection,voice_.module.toUtf8().constData())<0
        || d_->setLanguage(d_->connection,voice_.language.toUtf8().constData())<0
        || d_->setVoice(d_->connection,voice_.name.toUtf8().constData())<0) {
        error_=QStringLiteral("无法选择声音：%1（%2）。").arg(voice_.name,voice_.language); closeConnection(); return false;
    }
#endif
    return true;
}
bool LinuxSpeechDispatcher::speak(const QString &text,int volume) {
    if(text.trimmed().isEmpty()) return false;
    if(active_) stop();
    if(!prepare(text)) return false;
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    if((volume>=0 && d_->setVolume(d_->connection,std::clamp(volume,0,100)*2-100)<0) || (d_->message=d_->say(d_->connection,SPD_MESSAGE,text.toUtf8().constData()))<0) {
        error_=QStringLiteral("语音服务未能接受朗读请求。"); closeConnection(); return false;
    }
    setActive(true); return true;
#else
    return false;
#endif
}
void LinuxSpeechDispatcher::setAutoSpawnEnabled(bool enabled) {d_->autoSpawn=enabled;}
void LinuxSpeechDispatcher::setActive(bool active) {if(active_!=active) {active_=active; emit activeChanged(active);}}
void LinuxSpeechDispatcher::stop() {
    const bool wasActive=active_;
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    // 只取消本客户端，不影响读屏器、浏览器或其他翻译窗口正在播放的语音。
    // Cancel only this connection, leaving screen readers and other applications untouched.
    if(d_->connection && active_ && d_->cancel(d_->connection)<0) closeConnection();
#endif
    d_->message=-1; active_=false; if(wasActive) emit activeChanged(false);
}
void LinuxSpeechDispatcher::notification(size_t message,int state) {
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    if(d_->message<0 || size_t(d_->message)!=message) return;
    if(state==SPD_EVENT_END || state==SPD_EVENT_CANCEL) {d_->message=-1; setActive(false);}
    else if(state==SPD_EVENT_BEGIN) setActive(true);
#else
    Q_UNUSED(message); Q_UNUSED(state);
#endif
}
void LinuxSpeechDispatcher::closeConnection() {
#ifdef JIETUBA_HAS_SPEECH_DISPATCHER
    {const QMutexLocker lock(&clientsMutex()); if(d_->client>=0) clients().remove(size_t(d_->client));}
    if(d_->connection) {if(active_) d_->cancel(d_->connection); d_->close(d_->connection); d_->connection=nullptr;}
#endif
    d_->client=-1; d_->message=-1; d_->voices.clear(); active_=false;
}
