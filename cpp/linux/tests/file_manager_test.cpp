#include "file_manager.h"
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

/**
 * 私有总线上的文件管理器，不打开真实窗口；记录标准方法的实参，并可模拟服务拒绝。
 * File manager on a private test bus: records wire arguments without opening desktop windows.
 */
class FakeFileManager final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.freedesktop.FileManager1")
public:
    bool reject=false;
    QString method,startup,signature;
    QStringList urls;
public slots:
    void ShowItems(const QStringList &items,const QString &id) {record("ShowItems",items,id);}
    void ShowFolders(const QStringList &items,const QString &id) {record("ShowFolders",items,id);}
private:
    void record(const QString &name,const QStringList &items,const QString &id) {
        method=name; urls=items; startup=id; signature=message().signature();
        if(reject) sendErrorReply(QDBusError::NotSupported,QStringLiteral("test file manager unavailable"));
    }
};
/**
 * 验证真实 D-Bus 序列化和异步确认，以及系统命令回退的参数/退出码。
 * PATH 只包含临时脚本，不会启动 Dolphin、xdg-open 或其他真实桌面程序。
 * Tests real D-Bus marshalling/acknowledgement and fallback arguments/exit codes.
 * Temporary PATH fixtures prevent any real desktop opener from being launched.
 */
class FileManagerTest final : public QObject {
    Q_OBJECT
    FakeFileManager server_;
    QByteArray originalPath_;
    static bool write(const QString &path,const QByteArray &bytes,bool executable=false) {
        QFile file(path); if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size()) return false;
        file.close(); return !executable || file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    }
private slots:
    void init() {
        originalPath_=qgetenv("PATH"); server_.reject=false; server_.urls.clear(); server_.method.clear();
        auto bus=QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("org.freedesktop.FileManager1"));
        QVERIFY(bus.registerObject("/org/freedesktop/FileManager1",&server_,QDBusConnection::ExportAllSlots));
    }
    void cleanup() {
        QDBusConnection::sessionBus().unregisterObject("/org/freedesktop/FileManager1");
        QDBusConnection::sessionBus().unregisterService("org.freedesktop.FileManager1");
        qputenv("PATH",originalPath_); qunsetenv("JIETUBA_TEST_OPEN_LOG");
    }
    void revealFileAndFolder() {
        QTemporaryDir directory; const auto path=directory.filePath(QStringLiteral("录像 中文 #% 空格.mp4"));
        QVERIFY(write(path,"keep-video"));
        LinuxFileManager opener; QSignalSpy opened(&opener,&LinuxFileManager::opened),failed(&opener,&LinuxFileManager::failed);
        opener.reveal(path); QCOMPARE(opened.size(),0); QVERIFY(opener.isOpening());
        QTRY_COMPARE(opened.size(),1); QCOMPARE(failed.size(),0); QVERIFY(!opener.isOpening());
        QCOMPARE(server_.method,QString("ShowItems")); QCOMPARE(server_.signature,QString("ass"));
        QCOMPARE(server_.urls,QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)}); QVERIFY(server_.startup.isEmpty());
        opener.reveal(directory.path()); QTRY_COMPARE(opened.size(),2);
        QCOMPARE(server_.method,QString("ShowFolders"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("keep-video"));
    }
    void commandFallback() {
        server_.reject=true; QTemporaryDir directory; const auto path=directory.filePath("video.gif"),log=directory.filePath("args.txt");
        QVERIFY(write(path,"keep"));
        QVERIFY(write(directory.filePath("xdg-open"),"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$JIETUBA_TEST_OPEN_LOG\"\nexit 0\n",true));
        qputenv("PATH",directory.path().toUtf8()); qputenv("JIETUBA_TEST_OPEN_LOG",log.toUtf8());
        LinuxFileManager opener; QSignalSpy opened(&opener,&LinuxFileManager::opened),failed(&opener,&LinuxFileManager::failed);
        opener.reveal(path); QTRY_COMPARE(opened.size(),1); QCOMPARE(failed.size(),0);
        QFile args(log); QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll(),QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded).toUtf8()+'\n');
    }
    void gioFallback() {
        server_.reject=true; QTemporaryDir directory; const auto path=directory.filePath("video.mp4"),log=directory.filePath("args.txt");
        QVERIFY(write(path,"keep"));
        QVERIFY(write(directory.filePath("xdg-open"),"#!/bin/sh\nexit 7\n",true));
        QVERIFY(write(directory.filePath("gio"),"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$JIETUBA_TEST_OPEN_LOG\"\nexit 0\n",true));
        qputenv("PATH",directory.path().toUtf8()); qputenv("JIETUBA_TEST_OPEN_LOG",log.toUtf8());
        LinuxFileManager opener; QSignalSpy opened(&opener,&LinuxFileManager::opened),failed(&opener,&LinuxFileManager::failed);
        opener.reveal(path); QTRY_COMPARE(opened.size(),1); QCOMPARE(failed.size(),0);
        QFile args(log); QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll(),"open\n"+QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded).toUtf8()+'\n');
    }
    void allPathsFail() {
        server_.reject=true; QTemporaryDir directory; const auto path=directory.filePath("video.mp4");
        QVERIFY(write(path,"keep"));
        QVERIFY(write(directory.filePath("xdg-open"),"#!/bin/sh\nprintf 'test opener rejected' >&2\nexit 7\n",true));
        qputenv("PATH",directory.path().toUtf8());
        LinuxFileManager opener; QSignalSpy opened(&opener,&LinuxFileManager::opened),failed(&opener,&LinuxFileManager::failed);
        opener.reveal(path); QTRY_COMPARE(failed.size(),1); QCOMPARE(opened.size(),0); QVERIFY(!opener.isOpening());
        const auto error=failed.first().first().toString();
        QVERIFY(error.contains("FileManager1") && error.contains("test opener rejected") && error.contains("gio"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("keep"));
        opener.reveal(directory.filePath("missing/video.mp4")); QCOMPARE(failed.size(),2); QCOMPARE(opened.size(),0);
    }
};
QTEST_GUILESS_MAIN(FileManagerTest)
#include "file_manager_test.moc"
