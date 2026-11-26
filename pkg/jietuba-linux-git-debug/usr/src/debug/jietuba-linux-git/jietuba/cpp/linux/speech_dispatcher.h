#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <memory>
#include <cstddef>
struct LinuxSpeechVoice {QString module,name,language;};
/**
 * Linux 原生朗读适配器：直接使用 Speech Dispatcher 公共客户端 API，绕开 Qt 后端
 * 对默认 locale 及 cmn/yue 语言标签的初始化限制。声音从服务实际枚举，原样传回服务。
 * 动态加载 libspeechd，缺少它不影响程序启动；只在点击朗读时连接/自动启动用户语音服务。
 * 通知从库线程投递到 Qt 主线程；只取消本客户端的消息，关闭时先断开回调再释放连接。
 * Native Speech Dispatcher adapter bypassing Qt's default-locale/ISO-tag assumptions.
 * Voices are enumerated and selected by their actual service identifiers. Optional dynamic loading
 * keeps the application runnable without speech libraries. Threaded notifications are queued to Qt;
 * stopping affects only this client, and teardown unregisters callbacks before closing the connection.
 */
class LinuxSpeechDispatcher final : public QObject {
    Q_OBJECT
public:
    explicit LinuxSpeechDispatcher(QObject *parent=nullptr);
    ~LinuxSpeechDispatcher() override;
    static bool supported();
    static int chooseVoice(const QList<LinuxSpeechVoice> &voices,const QString &text);
    bool prepare(const QString &text);
    bool speak(const QString &text,int volume=-1);
    void setAutoSpawnEnabled(bool enabled);
    void stop();
    bool isSpeaking() const {return active_;}
    QString error() const {return error_;}
    LinuxSpeechVoice voice() const {return voice_;}
    void notification(size_t message,int state);
signals:
    void activeChanged(bool active);
private:
    bool initialize();
    void closeConnection();
    void setActive(bool active);
    struct Impl;
    std::unique_ptr<Impl> d_;
    QString error_;
    LinuxSpeechVoice voice_;
    bool active_=false;
};
