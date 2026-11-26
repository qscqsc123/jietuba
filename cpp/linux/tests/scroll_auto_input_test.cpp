#include "scroll_auto_input.h"
#include <QGuiApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QTimer>
#include <cstdio>

struct Pair { int a, b; };
struct Stream { uint node; QVariantMap properties; };
using Streams = QList<Stream>;
Q_DECLARE_METATYPE(Pair)
Q_DECLARE_METATYPE(Stream)
Q_DECLARE_METATYPE(Streams)
QDBusArgument &operator<<(QDBusArgument &a, const Pair &v) { a.beginStructure(); a << v.a << v.b; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, Pair &v) { a.beginStructure(); a >> v.a >> v.b; a.endStructure(); return a; }
QDBusArgument &operator<<(QDBusArgument &a, const Stream &v) { a.beginStructure(); a << v.node << v.properties; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, Stream &v) { a.beginStructure(); a >> v.node >> v.properties; a.endStructure(); return a; }

/**
 * 私有 D-Bus 模拟输入 portal，不移动实际指针。
 * 验证授权前零输入、只申请鼠标、显示器流坐标、双方向滚轮，以及取消/拒绝后释放会话。
 * A private-bus fake validates permission gating, coordinate binding, both wheel axes
 * and cleanup without sending input to the real desktop.
 */
class FakeInputPortal final : public QDBusVirtualObject {
public:
    bool reject = false, hold = false, bad = false;
    int moves = 0, wheels = 0, closed = 0;
    QStringList order;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        const auto args = m.arguments(); const QString method = m.member();
        if (method == "Close") { ++closed; bus.send(m.createReply()); return true; }
        order << method;
        if (method.startsWith("Notify")) {
            bad |= args.size() != (method == "NotifyPointerMotionAbsolute" ? 5 : 4) || args[0].value<QDBusObjectPath>().path() != session();
            if (method == "NotifyPointerMotionAbsolute") {
                ++moves; bad |= args[2].toUInt() != 77 || args[3].toDouble() != 160 || args[4].toDouble() != 180;
            } else {
                ++wheels; bad |= args[2].toUInt() != uint(wheels-1) || args[3].toInt() != 1;
                // Wheel notifications contain four arguments, unlike absolute motion.
            }
            bus.send(m.createReply()); return true;
        }
        const auto options = qdbus_cast<QVariantMap>(args.last());
        if (method == "SelectDevices") bad |= options.value("types").toUInt() != 2 || options.contains("persist_mode");
        QString sender = m.service().mid(1); sender.replace('.', '_');
        const QString path = "/org/freedesktop/portal/desktop/request/"+sender+'/'+options.value("handle_token").toString();
        bus.send(m.createReply(QVariant::fromValue(QDBusObjectPath(path))));
        if (hold && method == "Start") return true;
        QVariantMap result;
        if (method == "CreateSession") result.insert("session_handle", session());
        if (method == "Start") {
            result.insert("devices", uint(reject ? 0 : 2));
            result.insert("streams", QVariant::fromValue(Streams{{77, {{"position", QVariant::fromValue(Pair{0,0})}, {"size", QVariant::fromValue(Pair{800,600})}}}}));
        }
        QTimer::singleShot(5, this, [path, result, bus] {
            auto response = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            response << uint(0) << result; bus.send(response);
        });
        return true;
    }
    static QString session() { return "/org/freedesktop/portal/desktop/session/fake/scroll"; }
};
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    qDBusRegisterMetaType<Pair>(); qDBusRegisterMetaType<Stream>(); qDBusRegisterMetaType<Streams>();
    FakeInputPortal portal; auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop") || !bus.registerVirtualObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::SubPath)) return 1;
    LinuxScrollAutoInput input;
    const QRect screen(0,0,800,600); const QPointF target(160,180);
    int stage = 0;
    input.step(false); // Not authorized: must have no effect.
    QObject::connect(&input, &LinuxScrollAutoInput::authorized, &app, [&] {
        if (stage || portal.moves != 1 || portal.wheels || portal.order != QStringList{"CreateSession","SelectDevices","SelectSources","Start","NotifyPointerMotionAbsolute"}) { app.exit(2); return; }
        input.step(false);
    });
    QObject::connect(&input, &LinuxScrollAutoInput::stepped, &app, [&] {
        if (portal.wheels == 1) input.step(true);
        else {
            input.stop(); input.step(false);
            portal.reject = true; stage = 1;
            QTimer::singleShot(20, &app, [&] { input.start(screen, target); });
        }
    });
    QObject::connect(&input, &LinuxScrollAutoInput::failed, &app, [&](const QString &error) {
        if (stage != 1 || !error.contains(QStringLiteral("权限")) || input.ready() || portal.moves != 1 || portal.wheels != 2) { std::fprintf(stderr,"unexpected failure: %s\n", qPrintable(error)); app.exit(3); return; }
        stage = 2; portal.reject = false; portal.hold = true;
        input.start(screen, target);
        QTimer::singleShot(80, &app, [&] {
            input.stop(); input.step(true);
            QTimer::singleShot(20, &app, [&] {
                if (portal.bad || portal.closed < 4 || portal.moves != 1 || portal.wheels != 2) { std::fprintf(stderr,"cleanup/input mismatch: bad=%d close=%d\n", portal.bad, portal.closed); app.exit(4); return; }
                std::fprintf(stderr,"input authorization, axes, denial and cancellation passed\n"); app.exit(0);
            });
        });
    });
    QTimer::singleShot(5000, &app, [&] { std::fprintf(stderr,"input test timeout, stage %d\n",stage); app.exit(5); });
    QTimer::singleShot(0, &app, [&] { input.start(screen, target); });
    return app.exec();
}
