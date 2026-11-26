#pragma once
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QTimer>
#include <QVariantMap>

/**
 * 长截图的 Wayland 自动滚轮适配器。
 * 仅在用户点击自动滚动后创建 RemoteDesktop portal 会话，请求 POINTER，
 * 通过桌面授权选择目标显示器，再用该流的逻辑坐标定位指针。手动截图不走本接口。
 * 每次发送一个滚轮刻度并等待调用结果；失败、取消、超时及桌面重启均关闭会话。
 * 不保存授权令牌，不请求键盘权限，也不使用 root/uinput。
 *
 * User-initiated, pointer-only RemoteDesktop portal adapter. The selected monitor
 * supplies absolute coordinates; a wheel step is acknowledged before capture.
 * Every cancellation/error closes outstanding requests and the input session.
 */
class LinuxScrollAutoInput final : public QObject {
    Q_OBJECT
public:
    explicit LinuxScrollAutoInput(QObject *parent = nullptr);
    ~LinuxScrollAutoInput() override;
    void start(const QRect &screenGeometry, const QPointF &screenPoint);
    void step(bool horizontal);
    void stop();
    bool ready() const { return phase_ == Phase::Ready; }
signals:
    void authorized();
    void stepped();
    void failed(const QString &message);
private slots:
    void response(uint code, const QVariantMap &results);
    void sessionClosed(const QVariantMap &details);
private:
    enum class Phase { Idle, Creating, Devices, Sources, Starting, Moving, Ready };
    void request(const QString &interface, const QString &method, QList<QVariant> arguments, QVariantMap options);
    void clearRequest(bool cancel);
    void fail(const QString &message);
    void notify(const QString &method, QList<QVariant> arguments, bool moving);
    Phase phase_ = Phase::Idle;
    QString requestPath_, sessionPath_;
    QRect screen_;
    QPointF point_;
    QTimer timeout_;
    quint64 generation_ = 0;
    bool stepping_ = false;
};
