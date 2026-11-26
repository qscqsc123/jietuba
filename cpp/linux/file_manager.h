#pragma once
#include <QObject>
#include <QDBusConnection>
#include <QProcess>
#include <QTimer>

/**
 * Linux 文件管理器打开器，异步调用标准 FileManager1 接口，打开目录并选中结果文件。
 * 不依赖 Qt 的窗口 URL/portal 转发，避免 Wayland layer-shell 窗口缺少普通父窗口标识的问题。
 * 服务不可用时依次尝试 xdg-open、gio open，并检查退出状态，而不是启动命令后立即假定成功。
 * 路径作为独立参数传递，支持中文、空格和特殊字符；不使用 shell，不硬编码 Dolphin。
 * opened 表示桌面服务/系统打开器接受请求；failed 包含各条路径的诊断，调用方决定是否关闭 UI。
 * Tests can supply a private D-Bus connection; commands resolve through PATH for isolated fixtures.
 * Asynchronous Linux folder opener: FileManager1 reveals a result, then xdg-open/gio provide fallbacks.
 * Bypass Qt's window/portal URL forwarding for layer-shell windows lacking an ordinary parent handle.
 * Explicit process completion checks avoid treating process creation as a successful desktop handoff.
 * Pass paths as arguments, never shell code, with no hardcoded desktop/file manager dependency.
 * opened reports acceptance; failed aggregates diagnostics so callers can preserve their UI on failure.
 */
class LinuxFileManager final : public QObject {
    Q_OBJECT
public:
    explicit LinuxFileManager(QObject *parent=nullptr,const QDBusConnection &bus=QDBusConnection::sessionBus());
    ~LinuxFileManager() override;
    void reveal(const QString &path);
    bool isOpening() const {return opening_;}
signals:
    void opened();
    void failed(const QString &reason);
private:
    void fallback();
    void succeed();
    QDBusConnection bus_;
    QProcess process_;
    QTimer timeout_;
    QString folder_,diagnostic_,command_;
    QStringList errors_;
    int attempt_=0;
    bool opening_=false,timedOut_=false;
};
