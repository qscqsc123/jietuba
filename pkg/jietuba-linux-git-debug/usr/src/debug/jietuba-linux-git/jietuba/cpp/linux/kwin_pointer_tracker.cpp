#include "kwin_pointer_tracker.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>
#include <QGuiApplication>
#include <QPointer>

KWinPointerTracker::KWinPointerTracker(QObject *parent) : QObject(parent) {
    name_ = "jietuba_pointer_" + QUuid::createUuid().toString(QUuid::Id128);
    path_ = "/" + name_;
}
KWinPointerTracker::~KWinPointerTracker() {
    if (!started_) return;
    auto bus = QDBusConnection::sessionBus();
    bus.unregisterObject(path_);
    auto call = QDBusMessage::createMethodCall("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
    call.setArguments({name_});
    bus.asyncCall(call);
}
void KWinPointerTracker::start(bool hitWindows) {
    if (started_) return;
    if (!QGuiApplication::platformName().startsWith("wayland")) return;
    if (!QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains("KDE", Qt::CaseInsensitive)) return;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.interface()) return;
    const auto owner = bus.interface()->serviceOwner("org.kde.KWin");
    if (!owner.isValid()) return;
    owner_ = owner.value();
    if (!bus.registerObject(path_, this, QDBusConnection::ExportAllSlots) || !script_.open()) return;
    started_ = true;
    // workspace.cursorPosChanged 在穿透模式也会触发，不需要轮询或读取输入设备。
    // KWin emits cursorPosChanged even during pass-through, without polling or device access.
    const QString report=hitWindows ? QStringLiteral(R"JS(
function report(){var p=workspace.cursorPos, g={x:0,y:0,width:0,height:0},pid=0;
 var windows=workspace.windowAt(p,-1); for(var w of windows){if(!w.normalWindow && !w.dialog)continue;
 if(w.caption==='截图吧 - 框选'||w.caption==='截图吧 — Linux 画布预览')continue;
 g=w.frameGeometry;pid=w.pid;break;}
 callDBus('%1','%2','io.github.jietuba.Pointer','hit',p.x,p.y,g.x,g.y,g.width,g.height,pid);}
)JS") : QStringLiteral("function report(){var p=workspace.cursorPos;callDBus('%1','%2','io.github.jietuba.Pointer','position',p.x,p.y);}");
    const auto source=(report+"\nworkspace.cursorPosChanged.connect(report);report();").arg(bus.baseService(),path_).toUtf8();
    if (script_.write(source) != source.size() || !script_.flush()) return;
    auto call = QDBusMessage::createMethodCall("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript");
    call.setArguments({script_.fileName(), name_});
    // 关闭窗口可能早于加载完成；回调独立存活并清理迟到的脚本。
    // Keep the completion alive to unload a script whose window closed during loading.
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), QCoreApplication::instance());
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [self = QPointer<KWinPointerTracker>(this), name = name_, watcher] {
        QDBusPendingReply<int> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError() || reply.value() < 0) return;
        if (!self) {
            auto unload = QDBusMessage::createMethodCall("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
            unload.setArguments({name});
            QDBusConnection::sessionBus().asyncCall(unload);
            return;
        }
        auto run = QDBusMessage::createMethodCall("org.kde.KWin", "/Scripting/Script" + QString::number(reply.value()), "org.kde.kwin.Script", "run");
        QDBusConnection::sessionBus().asyncCall(run);
    });
}
void KWinPointerTracker::position(int x, int y) {
    // D-Bus supplies a unique sender name; only accept the current KWin owner.
    // 只接收当前 KWin 连接的光标通知，避免其它进程伪造坐标。
    // 缓存唯一连接名，避免每次移动都同步查询 D-Bus；KWin 重启后重新打开标注即可。
    // Cache the unique owner to avoid a synchronous bus lookup on every pointer movement.
    if (!calledFromDBus() || owner_.isEmpty() || message().service() != owner_) return;
    emit moved(QPointF(x, y));
}

void KWinPointerTracker::hit(int x,int y,int left,int top,int width,int height,int pid) {
    if (calledFromDBus() && !owner_.isEmpty() && message().service()==owner_) emit windowHit({x,y},{left,top,width,height},pid);
}
