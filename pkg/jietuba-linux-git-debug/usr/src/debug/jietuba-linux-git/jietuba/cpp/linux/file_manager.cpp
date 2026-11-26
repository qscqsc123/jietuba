#include "file_manager.h"
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

LinuxFileManager::LinuxFileManager(QObject *parent,const QDBusConnection &bus):QObject(parent),bus_(bus) {
    timeout_.setSingleShot(true); timeout_.setInterval(8000);
    connect(&timeout_,&QTimer::timeout,this,[this] {timedOut_=true; process_.kill();});
    connect(&process_,&QProcess::readyReadStandardError,this,[this] {
        diagnostic_=(diagnostic_+QString::fromLocal8Bit(process_.readAllStandardError())).right(4096);
    });
    connect(&process_,&QProcess::readyReadStandardOutput,this,[this] {process_.readAllStandardOutput();});
    connect(&process_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status) {
        if(!opening_) return;
        timeout_.stop();
        if(!timedOut_ && status==QProcess::NormalExit && code==0) {succeed(); return;}
        errors_<<QStringLiteral("%1：%2（退出码 %3）%4").arg(command_,timedOut_ ? QStringLiteral("打开超时") : QStringLiteral("打开失败"))
            .arg(code).arg(diagnostic_.isEmpty() ? QString() : QStringLiteral("：")+diagnostic_.trimmed());
        fallback();
    });
    connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if(!opening_ || error!=QProcess::FailedToStart) return;
        timeout_.stop(); errors_<<QStringLiteral("%1：%2").arg(command_,process_.errorString()); fallback();
    });
}
LinuxFileManager::~LinuxFileManager() {
    process_.disconnect(this); process_.kill();
    if(process_.state()!=QProcess::NotRunning) process_.waitForFinished(500);
}
void LinuxFileManager::reveal(const QString &path) {
    if(opening_) return;
    const QFileInfo target(path);
    folder_=target.isDir() ? target.absoluteFilePath() : target.absolutePath();
    if(path.isEmpty() || !QFileInfo(folder_).isDir()) {emit failed(QStringLiteral("保存目录不存在：%1").arg(folder_)); return;}
    opening_=true; attempt_=0; errors_.clear();
    // 标准签名为 as,s。URI 必须完整编码，不能把 #、% 或中文路径误解释为 URL 片段。
    // The standard signature is as,s; fully encode URIs so special path characters remain literal.
    const bool item=target.exists() && !target.isDir();
    auto message=QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),item ? QStringLiteral("ShowItems") : QStringLiteral("ShowFolders"));
    const auto uri=QUrl::fromLocalFile(item ? target.absoluteFilePath() : folder_).toString(QUrl::FullyEncoded);
    message.setArguments({QStringList{uri},QString()});
    auto *watcher=new QDBusPendingCallWatcher(bus_.asyncCall(message,5000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher] {
        const auto reply=watcher->reply(); watcher->deleteLater();
        if(!opening_) return;
        if(reply.type()==QDBusMessage::ReplyMessage) {succeed(); return;}
        errors_<<QStringLiteral("FileManager1：%1：%2").arg(reply.errorName(),reply.errorMessage()); fallback();
    });
}
void LinuxFileManager::fallback() {
    // 保留进程对象直到退出，才能报告 xdg-open 的真实失败；不能使用 startDetached 后立即关闭预览。
    // Keep process ownership until exit; startDetached cannot report a later xdg-open failure.
    const QStringList commands{QStringLiteral("xdg-open"),QStringLiteral("gio")};
    while(attempt_<commands.size()) {
        command_=commands[attempt_++]; const auto executable=QStandardPaths::findExecutable(command_);
        if(executable.isEmpty()) {errors_<<QStringLiteral("未找到 %1").arg(command_); continue;}
        diagnostic_.clear(); timedOut_=false;
        QStringList args;
        if(command_=="gio") args<<QStringLiteral("open");
        args<<QUrl::fromLocalFile(folder_).toString(QUrl::FullyEncoded);
        process_.start(executable,args); timeout_.start(); return;
    }
    opening_=false; emit failed(QStringLiteral("无法打开保存目录：%1\n%2").arg(folder_,errors_.join('\n')));
}
void LinuxFileManager::succeed() {opening_=false; timeout_.stop(); emit opened();}
