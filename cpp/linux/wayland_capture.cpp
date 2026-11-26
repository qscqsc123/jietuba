#include "wayland_capture.h"
#include "frame_conversion.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QUuid>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace {
QString service() { return QStringLiteral("org.freedesktop.portal.Desktop"); }
QString desktop() { return QStringLiteral("/org/freedesktop/portal/desktop"); }
QString castInterface() { return QStringLiteral("org.freedesktop.portal.ScreenCast"); }
QString requestInterface() { return QStringLiteral("org.freedesktop.portal.Request"); }
QString sessionInterface() { return QStringLiteral("org.freedesktop.portal.Session"); }
QString token() { return QStringLiteral("jietuba_") + QUuid::createUuid().toString(QUuid::Id128); }

// D-Bus 变体可能由 Qt 包在 QDBusVariant 内；统一解包后再读取。
// Portal dictionaries can contain a QDBusVariant wrapper depending on the Qt backend.
QVariant unwrap(QVariant value) {
    while (value.metaType() == QMetaType::fromType<QDBusVariant>()) value = qvariant_cast<QDBusVariant>(value).variant();
    return value;
}

// 从 portal 的 a(ua{sv}) 取唯一流；版本 6 优先使用不会重用的 serial。
// Read the single a(ua{sv}) stream; v6's serial is safer than a reusable node ID.
bool readStream(const QVariant &value, uint *node, qulonglong *serial,QVariantMap *metadata) {
    if (value.metaType() != QMetaType::fromType<QDBusArgument>()) return false;
    const QDBusArgument array = qvariant_cast<QDBusArgument>(value);
    array.beginArray();
    if (array.atEnd()) { array.endArray(); return false; }
    QVariantMap properties;
    array.beginStructure();
    array >> *node >> properties;
    array.endStructure();
    const bool extra = !array.atEnd();
    array.endArray();
    *serial = unwrap(properties.value(QStringLiteral("pipewire-serial"))).toULongLong();
    *metadata=properties;
    return !extra && *node != 0;
}

}

WaylandCapture::WaylandCapture(QObject *parent) : QObject(parent) {
    // GStreamer 初始化只需一次；无需传递或修改 Qt 应用的命令行。
    // Initialize GStreamer once without changing the Qt application's arguments.
    static const bool initialized = [] { gst_init(nullptr, nullptr); return true; }();
    Q_UNUSED(initialized);
    pollTimer_.setInterval(30);
    connect(&pollTimer_, &QTimer::timeout, this, &WaylandCapture::pollStream);
    requestTimeout_.setSingleShot(true);
    requestTimeout_.setInterval(120000);
    connect(&requestTimeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("屏幕共享请求超时，请重试。")); });
    frameTimeout_.setSingleShot(true);
    frameTimeout_.setInterval(8000);
    connect(&frameTimeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("屏幕共享已授权，但没有收到画面。")); });
    // portal 服务重启时不一定发出 Session::Closed；主动丢弃旧会话以便重试。
    // A restarted portal may not emit Session::Closed; discard the stale session.
    auto *watcher = new QDBusServiceWatcher(service(), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &oldOwner, const QString &newOwner) {
        if (!oldOwner.isEmpty() && oldOwner != newOwner && phase_ != Phase::Idle) {
            stop();
            emit sessionEnded();
        }
    });
}

WaylandCapture::~WaylandCapture() { stop(); }

void WaylandCapture::capture() {
    if (phase_ == Phase::Ready) { requestFrame(); return; }
    if (phase_ != Phase::Idle) return;
    if (!QDBusConnection::sessionBus().isConnected()) { emit failed(QStringLiteral("无法连接桌面 D-Bus 会话。")); return; }
    phase_ = Phase::Creating;
    request(QStringLiteral("CreateSession"), {}, {{QStringLiteral("session_handle_token"), token()}});
}

void WaylandCapture::request(const QString &method, QList<QVariant> arguments, QVariantMap options) {
    auto bus = QDBusConnection::sessionBus();
    const QString handleToken = token();
    options.insert(QStringLiteral("handle_token"), handleToken);
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    requestPath_ = QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + '/' + handleToken;
    // 先订阅 Response 再发方法调用，防止桌面响应过快造成信号丢失。
    // Subscribe before invoking the method so an early Response cannot be lost.
    if (!bus.connect(service(), requestPath_, requestInterface(), QStringLiteral("Response"), this, SLOT(portalResponse(uint,QVariantMap)))) {
        fail(QStringLiteral("无法监听屏幕共享授权结果。"));
        return;
    }
    auto message = QDBusMessage::createMethodCall(service(), desktop(), castInterface(), method);
    arguments.append(options);
    message.setArguments(arguments);
    const QString expected = requestPath_;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusObjectPath> reply = *call;
        call->deleteLater();
        if (requestPath_ != expected) return; // 响应已处理，或会话已取消。
        if (reply.isError()) fail(QStringLiteral("屏幕共享服务调用失败：%1").arg(reply.error().message()));
        else if (reply.value().path() != expected) fail(QStringLiteral("屏幕共享服务返回了不匹配的请求路径。"));
    });
    requestTimeout_.start();
}

void WaylandCapture::portalResponse(uint response, const QVariantMap &results) {
    if (requestPath_.isEmpty()) return;
    clearRequest();
    if (response == 1) { stop(); emit cancelled(); return; }
    if (response != 0) { fail(QStringLiteral("桌面未能完成屏幕共享请求。")); return; }
    if (phase_ == Phase::Creating) {
        // 规范使用字符串，兼容部分实现返回的 D-Bus 对象路径。
        // The specification uses a string; tolerate an object-path variant as well.
        const auto handle = unwrap(results.value(QStringLiteral("session_handle")));
        sessionPath_ = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                           ? qvariant_cast<QDBusObjectPath>(handle).path() : handle.toString();
        if (!sessionPath_.startsWith('/')) { fail(QStringLiteral("屏幕共享服务未返回有效会话。")); return; }
        if (!QDBusConnection::sessionBus().connect(service(), sessionPath_, sessionInterface(), QStringLiteral("Closed"),
                                                    this, SLOT(portalSessionClosed(QVariantMap)))) {
            fail(QStringLiteral("无法监听屏幕共享会话状态。")); return;
        }
        phase_ = Phase::Selecting;
        // types=1：显示器；multiple=false：首版只选择一个显示器。
        // Request one monitor. Do not assume several streams share a pixel coordinate system.
        request(QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
                {{QStringLiteral("types"), uint(1)}, {QStringLiteral("multiple"), false},{QStringLiteral("cursor_mode"),uint(includeCursor_ ? 2 : 1)}});
    } else if (phase_ == Phase::Selecting) {
        phase_ = Phase::Starting;
        // 不把 Wayland 本地窗口 ID 当作 portal 句柄；当前预览窗口已隐藏。
        // No parent handle: a native Wayland window ID is not a portal window identifier.
        request(QStringLiteral("Start"), {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString()}, {});
    } else if (phase_ == Phase::Starting) {
        if (!readStream(unwrap(results.value(QStringLiteral("streams"))), &nodeId_, &serial_,&streamProperties_)) {
            fail(QStringLiteral("未获得有效的单屏视频流。")); return;
        }
        openRemote();
    }
}

void WaylandCapture::openRemote() {
    phase_ = Phase::Opening;
    auto message = QDBusMessage::createMethodCall(service(), desktop(), castInterface(), QStringLiteral("OpenPipeWireRemote"));
    message << QVariant::fromValue(QDBusObjectPath(sessionPath_)) << QVariantMap{};
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    const auto generation = generation_;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *call;
        call->deleteLater();
        if (generation != generation_ || phase_ != Phase::Opening) return;
        requestTimeout_.stop();
        if (reply.isError() || !reply.value().isValid()) {
            fail(QStringLiteral("无法连接屏幕共享视频流：%1").arg(reply.error().message())); return;
        }
        // 描述符必须活到管线停止；GStreamer 使用 portal 提供的受限连接。
        // Retain the portal FD until the pipeline stops; it is the restricted PipeWire connection.
        remoteFd_ = reply.value();
        QString error;
        if (!startPipeline(&error)) { fail(error); return; }
        phase_ = Phase::Ready;
        emit sessionReady();
        // 等待桌面的选屏对话框退出合成画面，再清空旧帧并抓取。
        // Let the portal chooser disappear from the compositor before requesting a frame.
        QTimer::singleShot(300, this, [this, generation] { if (generation == generation_ && phase_ == Phase::Ready) requestFrame(); });
    });
    requestTimeout_.start();
}

bool WaylandCapture::startPipeline(QString *error) {
    GstElement *pipe = gst_pipeline_new("jietuba-capture");
    GstElement *source = gst_element_factory_make("pipewiresrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *sink = gst_element_factory_make("appsink", nullptr);
    if (!pipe || !source || !convert || !filter || !sink) {
        for (auto *element : {source, convert, filter, sink, pipe}) if (element) gst_object_unref(element);
        *error = QStringLiteral("缺少 GStreamer 的 PipeWire、videoconvert 或 appsink 插件。");
        return false;
    }
    // serial 有效时使用 target-object；旧 portal 使用 node ID 的 path 属性。
    // Use a stable serial when supplied; older portals expose only the node ID.
    g_object_set(source, "fd", remoteFd_.fileDescriptor(), "keepalive-time", 100, nullptr);
    if (serial_ && g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object")) {
        const auto id = QByteArray::number(serial_);
        g_object_set(source, "target-object", id.constData(), nullptr);
    } else {
        const auto id = QByteArray::number(nodeId_);
        g_object_set(source, "path", id.constData(), nullptr);
    }
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "max-buffers", 1u, "drop", TRUE, "sync", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipe), source, convert, filter, sink, nullptr);
    if (!gst_element_link_many(source, convert, filter, sink, nullptr) || gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
        *error = QStringLiteral("无法启动 PipeWire 视频采集管线。");
        return false;
    }
    pipeline_ = pipe;
    sink_ = GST_APP_SINK(sink);
    pollTimer_.start();
    return true;
}

void WaylandCapture::requestFrame() {
    if (awaitingFrame_ || !sink_) return;
    // 清除隐藏窗口之前的旧帧；等待之后的新帧或 PipeWire keepalive 帧。
    // Discard frames queued before the preview was hidden, then wait for a fresh frame.
    while (auto *sample = gst_app_sink_try_pull_sample(sink_, 0)) gst_sample_unref(sample);
    awaitingFrame_ = true;
    frameTimeout_.start();
}

void WaylandCapture::pollStream() {
    if (!pipeline_) return;
    GstBus *bus = gst_element_get_bus(pipeline_);
    GstMessage *message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (message) {
        QString detail = QStringLiteral("屏幕共享视频流已断开。");
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr; gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            if (error) { detail += QStringLiteral(" %1").arg(QString::fromUtf8(error->message)); g_error_free(error); }
            g_free(debug);
        }
        gst_message_unref(message);
        fail(detail);
        return;
    }
    if (!awaitingFrame_) return;
    GstSample *sample = gst_app_sink_try_pull_sample(sink_, 0);
    if (!sample) return;
    const QImage image = imageFromGstSample(sample);
    gst_sample_unref(sample);
    if (image.isNull()) { fail(QStringLiteral("屏幕视频帧格式无效或无法读取。")); return; }
    awaitingFrame_ = false;
    frameTimeout_.stop();
    emit frameReady(image);
}

void WaylandCapture::portalSessionClosed(const QVariantMap &) {
    const bool active = phase_ != Phase::Idle;
    stop();
    if (active) emit sessionEnded();
}

void WaylandCapture::clearRequest() {
    requestTimeout_.stop();
    if (requestPath_.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(service(), requestPath_, requestInterface(), QStringLiteral("Response"), this, SLOT(portalResponse(uint,QVariantMap)));
    requestPath_.clear();
}

void WaylandCapture::closeSession() {
    if (sessionPath_.isEmpty()) return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(service(), sessionPath_, sessionInterface(), QStringLiteral("Closed"), this, SLOT(portalSessionClosed(QVariantMap)));
    bus.asyncCall(QDBusMessage::createMethodCall(service(), sessionPath_, sessionInterface(), QStringLiteral("Close")));
    sessionPath_.clear();
}

void WaylandCapture::stopPipeline() {
    pollTimer_.stop();
    frameTimeout_.stop();
    awaitingFrame_ = false;
    if (pipeline_) { gst_element_set_state(pipeline_, GST_STATE_NULL); gst_object_unref(pipeline_); }
    pipeline_ = nullptr;
    sink_ = nullptr;
    remoteFd_ = QDBusUnixFileDescriptor();
}

void WaylandCapture::stop() {
    ++generation_;
    if (!requestPath_.isEmpty())
        QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(service(), requestPath_, requestInterface(), QStringLiteral("Close")));
    clearRequest();
    stopPipeline();
    closeSession();
    phase_ = Phase::Idle;
    nodeId_ = 0;
    serial_ = 0;
}

void WaylandCapture::fail(const QString &error) { stop(); emit failed(error); }
