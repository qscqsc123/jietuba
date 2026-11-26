#include "wayland_capture.h"
#ifdef JIETUBA_TEST_ROUTER
#include "capture/capture_router.h"
#include <QGuiApplication>
#endif
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QTimer>
#include <cstdio>
struct MockStream { uint node; QVariantMap props; };
using MockStreams = QList<MockStream>;
Q_DECLARE_METATYPE(MockStream)
Q_DECLARE_METATYPE(MockStreams)
QDBusArgument &operator<<(QDBusArgument &a,const MockStream &s){a.beginStructure();a<<s.node<<s.props;a.endStructure();return a;}
const QDBusArgument &operator>>(const QDBusArgument &a,MockStream &s){a.beginStructure();a>>s.node>>s.props;a.endStructure();return a;}
/**
 * 模拟 portal 返回单屏视频流，并故意拒绝 PipeWire FD 请求。
 * 用于验证 D-Bus 结构解析、错误传播和下一次请求是否能够重新开始。
 *
 * Returns a fake single-monitor stream and rejects the PipeWire FD request.
 * Exercises D-Bus stream parsing, error propagation and retry after failure.
 */
class FakePortal : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public slots:
 QDBusObjectPath CreateSession(const QVariantMap &o) { return respond(o,0,{{"session_handle",QString("/org/freedesktop/portal/desktop/session/fake/session")}}); }
 QDBusObjectPath SelectSources(const QDBusObjectPath &,const QVariantMap &o) { return respond(o,0,{}); }
 QDBusObjectPath Start(const QDBusObjectPath &,const QString &,const QVariantMap &o) {
  MockStreams streams{{42,{{"pipewire-serial",QVariant::fromValue(qulonglong(12345))}}}};
  return respond(o,0,{{"streams",QVariant::fromValue(streams)}});
 }
 QDBusUnixFileDescriptor OpenPipeWireRemote(const QDBusObjectPath &,const QVariantMap &) {
  sendErrorReply(QDBusError::Failed,QStringLiteral("intentional fake FD failure")); return {};
 }
private:
 QDBusObjectPath respond(const QVariantMap &o,uint code,const QVariantMap &results) {
  QString sender=message().service().mid(1); sender.replace('.','_');
  QString path="/org/freedesktop/portal/desktop/request/"+sender+"/"+o.value("handle_token").toString();
  QTimer::singleShot(10,this,[path,code,results]{auto sig=QDBusMessage::createSignal(path,"org.freedesktop.portal.Request","Response");sig<<code<<results;QDBusConnection::sessionBus().send(sig);});
  return QDBusObjectPath(path);
 }
};
int main(int argc,char**argv){
#ifdef JIETUBA_TEST_ROUTER
 QGuiApplication app(argc,argv);
#else
 QCoreApplication app(argc,argv);
#endif
 qDBusRegisterMetaType<MockStream>();qDBusRegisterMetaType<MockStreams>();
 FakePortal portal; auto bus=QDBusConnection::sessionBus(); if(!bus.registerService("org.freedesktop.portal.Desktop")||!bus.registerObject("/org/freedesktop/portal/desktop",&portal,QDBusConnection::ExportAllSlots))return 1;
#ifdef JIETUBA_TEST_ROUTER
 // 原生协议不可用的 GNOME 环境，由已注册的私有 portal 接住，绝不访问真实合成器。
 // Simulate GNOME without native capture protocols; the private portal handles the fallback.
 qputenv("XDG_CURRENT_DESKTOP","GNOME"); qputenv("WAYLAND_DISPLAY","jietuba-test-no-compositor");
 CaptureRouter router;
 QObject::connect(&router,&CaptureRouter::failed,&app,[&](const QString &error) {
  if(error.contains("intentional fake FD failure") && error.contains("XDG ScreenCast portal")) app.exit(0);
  else {fprintf(stderr,"unexpected router error: %s\n",qPrintable(error)); app.exit(2);}
 });
 QTimer::singleShot(5000,&app,[&] {app.exit(3);});
 QTimer::singleShot(0,&app,[&] {router.captureRegion("test-output",QRect(10,10,100,80),QRect(0,0,800,600));});
 return app.exec();
#endif
 WaylandCapture capture; int count=0;
 QObject::connect(&capture,&WaylandCapture::failed,&app,[&](const QString &message){ if(!message.contains("intentional fake FD failure")){fprintf(stderr,"unexpected error: %s\n",qPrintable(message));app.exit(2);return;} if(++count==2){fprintf(stderr,"portal stream parsing and error recovery passed\n");app.exit(0);}else QTimer::singleShot(0,&capture,&WaylandCapture::capture);});
 QTimer::singleShot(5000,&app,[&]{fprintf(stderr,"protocol test timed out\n");app.exit(3);}); QTimer::singleShot(0,&capture,&WaylandCapture::capture); return app.exec();
}
#include "portal_protocol_test.moc"
