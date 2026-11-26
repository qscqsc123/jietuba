#include "wayland_capture.h"
#include <QCoreApplication>
#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QTimer>
#include <cstdio>

/**
 * 模拟桌面 portal 的建会话和取消授权过程。
 * 测试在独立 D-Bus 上运行，不会访问用户的屏幕或实际 portal。
 *
 * Simulates session creation and authorization cancellation on a private D-Bus.
 * No real desktop portal or screen is accessed by this fake service.
 */
class FakePortal : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public slots:
 QDBusObjectPath CreateSession(const QVariantMap &options) { return respond(options, 0, {{"session_handle", QString("/org/freedesktop/portal/desktop/session/fake/session")}}); }
 QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options) { return respond(options, 1, {}); }
private:
 QDBusObjectPath respond(const QVariantMap &options, uint code, const QVariantMap &results) {
  QString sender = message().service().mid(1); sender.replace('.', '_');
  QString path = "/org/freedesktop/portal/desktop/request/"+sender+"/"+options.value("handle_token").toString();
  QTimer::singleShot(10, this, [path,code,results] {
   auto sig=QDBusMessage::createSignal(path,"org.freedesktop.portal.Request","Response"); sig << code << results; QDBusConnection::sessionBus().send(sig);
  });
  return QDBusObjectPath(path);
 }
};
int main(int argc,char **argv){
 QCoreApplication app(argc,argv); FakePortal portal; auto bus=QDBusConnection::sessionBus();
 if(!bus.registerService("org.freedesktop.portal.Desktop") || !bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots))return 1;
 WaylandCapture capture; int count=0;
 QObject::connect(&capture,&WaylandCapture::cancelled,&app,[&]{if(++count==2){fprintf(stderr,"portal cancellation and restart passed\n");app.exit(0);}else QTimer::singleShot(0,&capture,&WaylandCapture::capture);});
 QObject::connect(&capture,&WaylandCapture::failed,&app,[&](const QString &error){fprintf(stderr,"unexpected error: %s\n",qPrintable(error));app.exit(2);});
 QTimer::singleShot(5000,&app,[&]{fprintf(stderr,"portal test timed out\n");app.exit(3);});
 QTimer::singleShot(0,&capture,&WaylandCapture::capture); return app.exec();
}
#include "portal_cancel_test.moc"
