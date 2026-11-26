#pragma once
#include <QObject>
#include <QDBusObjectPath>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QTimer>
#include <QDBusArgument>

// Portal 使用 a(sa{sv}) 传输快捷键列表；每一项包含 ID 和描述属性。
// Portal shortcut entries are encoded as a(sa{sv}).
struct GlobalShortcutEntry { QString id; QVariantMap options; };
using GlobalShortcutList = QList<GlobalShortcutEntry>;
Q_DECLARE_METATYPE(GlobalShortcutEntry)
Q_DECLARE_METATYPE(GlobalShortcutList)
QDBusArgument &operator<<(QDBusArgument &argument, const GlobalShortcutEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &argument, GlobalShortcutEntry &entry);

/**
 * 通过桌面 Global Shortcuts Portal 注册全局截图动作。
 * 注册和授权异步完成，失败时保留托盘入口；不同桌面可要求用户配置快捷键。
 * Registers a global capture action asynchronously via the desktop portal.
 * The tray remains usable when the portal is unavailable or binding is denied.
 */
class GlobalShortcut final : public QObject {
    Q_OBJECT
public:
    explicit GlobalShortcut(QObject *parent = nullptr);
    ~GlobalShortcut() override;
    void start();
    void configure();
    static const GlobalShortcutList &actions();
    static QString applicationId() { return QStringLiteral("io.github.jietuba.LinuxPreview"); }
    QString bindingText(const QString &id = QStringLiteral("capture")) const;
    // 保留桌面原始按键描述供本地动作匹配；显示用中文前缀不能参与组合键解析。
    // Keep the desktop's raw trigger separate from the localized status text used for display.
    QString triggerDescription(const QString &id = QStringLiteral("capture")) const { return triggers_.value(id); }
    bool canConfigure() const { return phase_ == Phase::Ready; }
    bool canRetry() const { return phase_ == Phase::Idle; }
    void setConfiguredSlots(const QMap<QString,QStringList> &values) {configured_=values;}
    QStringList slotBindings(const QString &id) const;
    bool slotConfirmed(const QString &id,int slot) const;
    void changeSlot(const QString &id,int slot,const QString &value);
    bool isSaving() const {return saving_;}
    void setRecordingKey(bool recording) {recording_=recording;}
signals:
    void activated(const QString &id, const QString &binding);
    void statusChanged(const QString &message);
    void bindingChanged();
    void shortcutCaptured(const QString &key);
    void editFinished(const QString &id,bool success,const QString &error);
private slots:
    void response(uint response, const QVariantMap &results);
    void shortcutActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp, const QVariantMap &options);
    void shortcutsChanged(const QDBusObjectPath &session, const GlobalShortcutList &shortcuts);
    void sessionClosed(const QVariantMap &details);
    void kdeShortcutChanged(const QStringList &action,const QList<int> &keys);
private:
    void request(const QString &method, QList<QVariant> arguments, QVariantMap options);
    void clearRequest();
    void fail(const QString &message);
    void closeSession();
    void applyBindings(const GlobalShortcutList &shortcuts, bool complete);
    void synchronizeKdeDefaults();
    void refreshKdeBindings();
    GlobalShortcutList requestedActions() const;
    void editKde(const QString &id,const QStringList &keys);
    void completeEdit(const QString &id,const QStringList &keys,bool success,const QString &error={});
    QString requestPath_, sessionPath_;
    QString bindingText_ = QStringLiteral("等待注册全局快捷键…");
    QMap<QString, QString> triggers_;
    bool bindingsKnown_ = false;
    QMap<QString,QStringList> configured_,confirmedSlots_,kdeActions_;
    bool saving_=false,recording_=false,editingThroughPortal_=false;
    QString pendingId_;
    QStringList pendingKeys_;
    QTimer requestTimeout_;
    enum class Phase { Idle, Creating, Binding, Ready } phase_ = Phase::Idle;
};
