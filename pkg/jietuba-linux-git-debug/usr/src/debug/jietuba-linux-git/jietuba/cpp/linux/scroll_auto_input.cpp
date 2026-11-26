#include "scroll_auto_input.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QUuid>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString desktop = QStringLiteral("/org/freedesktop/portal/desktop");
const QString remote = QStringLiteral("org.freedesktop.portal.RemoteDesktop");
const QString cast = QStringLiteral("org.freedesktop.portal.ScreenCast");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString sessionInterface = QStringLiteral("org.freedesktop.portal.Session");
QString token() { return QStringLiteral("jietuba_scroll_") + QUuid::createUuid().toString(QUuid::Id128); }
QVariant unwrap(QVariant v) {
    while (v.metaType() == QMetaType::fromType<QDBusVariant>()) v = qvariant_cast<QDBusVariant>(v).variant();
    return v;
}
bool pairValue(const QVariant &wrapped, int &a, int &b) {
    const QVariant v = unwrap(wrapped);
    if (v.metaType() != QMetaType::fromType<QDBusArgument>()) return false;
    const auto arg = qvariant_cast<QDBusArgument>(v);
    if (arg.currentSignature() != QStringLiteral("(ii)")) return false;
    arg.beginStructure(); arg >> a >> b; arg.endStructure(); return true;
}
}
LinuxScrollAutoInput::LinuxScrollAutoInput(QObject *parent) : QObject(parent) {
    timeout_.setSingleShot(true); timeout_.setInterval(120000);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("自动滚动授权超时，仍可手动滚动。")); });
    auto *watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &old, const QString &now) {
        if (phase_ != Phase::Idle && !old.isEmpty() && old != now) fail(QStringLiteral("桌面输入服务已重启，自动滚动已停止。"));
    });
}
LinuxScrollAutoInput::~LinuxScrollAutoInput() { stop(); }
void LinuxScrollAutoInput::start(const QRect &geometry, const QPointF &point) {
    stop(); screen_ = geometry; point_ = point;
    if (screen_.isEmpty() || !QRectF(screen_).contains(point_)) { fail(QStringLiteral("自动滚动目标不在显示器内。")); return; }
    phase_ = Phase::Creating;
    request(remote, QStringLiteral("CreateSession"), {}, {{QStringLiteral("session_handle_token"), token()}});
}
void LinuxScrollAutoInput::request(const QString &interface, const QString &method, QList<QVariant> arguments, QVariantMap options) {
    auto bus = QDBusConnection::sessionBus();
    QString sender = bus.baseService().mid(1); sender.replace('.', '_');
    const QString handle = token(); options.insert(QStringLiteral("handle_token"), handle);
    requestPath_ = desktop + QStringLiteral("/request/") + sender + '/' + handle;
    if (!bus.connect(service, requestPath_, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)))) {
        fail(QStringLiteral("无法监听桌面输入授权结果。")); return;
    }
    auto message = QDBusMessage::createMethodCall(service, desktop, interface, method);
    arguments.append(options); message.setArguments(arguments);
    const QString expected = requestPath_;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusObjectPath> reply = *call; call->deleteLater();
        if (requestPath_ != expected) return;
        if (reply.isError()) fail(QStringLiteral("桌面不支持自动滚动或调用失败：%1").arg(reply.error().message()));
        else if (reply.value().path() != expected) fail(QStringLiteral("桌面返回了无效输入授权请求。"));
    });
    timeout_.start();
}
void LinuxScrollAutoInput::response(uint code, const QVariantMap &results) {
    if (requestPath_.isEmpty()) return;
    clearRequest(false);
    if (code) { fail(QStringLiteral("未授权自动滚动，仍可手动滚动。")); return; }
    const auto session = QVariant::fromValue(QDBusObjectPath(sessionPath_.isEmpty() ? QStringLiteral("/") : sessionPath_));
    if (phase_ == Phase::Creating) {
        const QVariant value = unwrap(results.value(QStringLiteral("session_handle")));
        sessionPath_ = value.metaType() == QMetaType::fromType<QDBusObjectPath>() ? value.value<QDBusObjectPath>().path() : value.toString();
        if (!sessionPath_.startsWith('/')) { fail(QStringLiteral("桌面未返回有效输入会话。")); return; }
        QDBusConnection::sessionBus().connect(service, sessionPath_, sessionInterface, QStringLiteral("Closed"), this, SLOT(sessionClosed(QVariantMap)));
        phase_ = Phase::Devices;
        request(remote, QStringLiteral("SelectDevices"), {QVariant::fromValue(QDBusObjectPath(sessionPath_))}, {{QStringLiteral("types"), uint(2)}});
    } else if (phase_ == Phase::Devices) {
        // 绝对指针定位必须绑定 portal 返回的流；不可猜测多显示器的全局坐标。
        // Absolute input needs a selected stream, never guessed global multi-monitor coordinates.
        phase_ = Phase::Sources;
        request(cast, QStringLiteral("SelectSources"), {session}, {{QStringLiteral("types"), uint(1)}, {QStringLiteral("multiple"), false}});
    } else if (phase_ == Phase::Sources) {
        phase_ = Phase::Starting; request(remote, QStringLiteral("Start"), {session, QString()}, {});
    } else if (phase_ == Phase::Starting) {
        if (!(unwrap(results.value(QStringLiteral("devices"))).toUInt() & 2)) { fail(QStringLiteral("桌面未授予鼠标控制权限。")); return; }
        const QVariant streams = unwrap(results.value(QStringLiteral("streams")));
        if (streams.metaType() != QMetaType::fromType<QDBusArgument>()) { fail(QStringLiteral("桌面未返回自动滚动目标显示器。")); return; }
        const auto array = streams.value<QDBusArgument>();
        uint node = 0; QVariantMap properties;
        array.beginArray();
        if (array.atEnd()) { array.endArray(); fail(QStringLiteral("未选择自动滚动目标显示器。")); return; }
        array.beginStructure(); array >> node >> properties; array.endStructure();
        const bool extra = !array.atEnd(); array.endArray();
        int x = 0, y = 0, w = 0, h = 0;
        const bool position = pairValue(properties.value(QStringLiteral("position")), x, y);
        const bool size = pairValue(properties.value(QStringLiteral("size")), w, h);
        // 多屏必须能核实选中屏幕；单屏可接受旧 portal 缺少位置元数据。
        // Verify monitor identity on multi-screen desktops; tolerate old single-screen metadata.
        if (!node || extra || (QGuiApplication::screens().size() > 1 && (!position || !size || QRect(x,y,w,h) != screen_)) ||
            (position && QPoint(x,y) != screen_.topLeft())) {
            fail(QStringLiteral("请选择当前长截图所在的显示器；无法核实目标时不发送滚轮。")); return;
        }
        const qreal sx = size && w > 0 ? qreal(w)/screen_.width() : 1;
        const qreal sy = size && h > 0 ? qreal(h)/screen_.height() : 1;
        phase_ = Phase::Moving;
        notify(QStringLiteral("NotifyPointerMotionAbsolute"), {node, (point_.x()-screen_.x())*sx, (point_.y()-screen_.y())*sy}, true);
    }
}
void LinuxScrollAutoInput::notify(const QString &method, QList<QVariant> arguments, bool moving) {
    auto message = QDBusMessage::createMethodCall(service, desktop, remote, method);
    QList<QVariant> prefix{QVariant::fromValue(QDBusObjectPath(sessionPath_)), QVariantMap{}}; prefix.append(arguments); message.setArguments(prefix);
    const auto generation = generation_;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation, moving](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<> reply = *call; call->deleteLater();
        if (generation != generation_) return;
        stepping_ = false;
        if (reply.isError()) { fail(QStringLiteral("自动滚动输入失败：%1").arg(reply.error().message())); return; }
        if (moving) { phase_ = Phase::Ready; emit authorized(); } else emit stepped();
    });
}
void LinuxScrollAutoInput::step(bool horizontal) {
    if (!ready() || stepping_) return;
    stepping_ = true;
    notify(QStringLiteral("NotifyPointerAxisDiscrete"), {uint(horizontal ? 1 : 0), int(1)}, false);
}
void LinuxScrollAutoInput::clearRequest(bool cancel) {
    timeout_.stop();
    if (requestPath_.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(service, requestPath_, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)));
    if (cancel) bus.asyncCall(QDBusMessage::createMethodCall(service, requestPath_, requestInterface, QStringLiteral("Close")));
    requestPath_.clear();
}
void LinuxScrollAutoInput::stop() {
    ++generation_; clearRequest(true); phase_ = Phase::Idle; stepping_ = false;
    if (!sessionPath_.isEmpty()) {
        auto bus = QDBusConnection::sessionBus();
        bus.disconnect(service, sessionPath_, sessionInterface, QStringLiteral("Closed"), this, SLOT(sessionClosed(QVariantMap)));
        bus.asyncCall(QDBusMessage::createMethodCall(service, sessionPath_, sessionInterface, QStringLiteral("Close")));
        sessionPath_.clear();
    }
}
void LinuxScrollAutoInput::fail(const QString &message) { stop(); emit failed(message); }
void LinuxScrollAutoInput::sessionClosed(const QVariantMap &) { fail(QStringLiteral("桌面已结束自动滚动，仍可手动滚动。")); }
