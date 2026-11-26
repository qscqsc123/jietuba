#include "global_shortcut.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QUuid>
#include <QDebug>
#include <QKeyCombination>
#include <QKeySequence>
#include <QSet>
#include <algorithm>
#include <memory>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString path = QStringLiteral("/org/freedesktop/portal/desktop");
const QString iface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const QString requestIface = QStringLiteral("org.freedesktop.portal.Request");
QString token() { return QStringLiteral("jietuba_") + QUuid::createUuid().toString(QUuid::Id128); }
}
QDBusArgument &operator<<(QDBusArgument &a, const GlobalShortcutEntry &s) { a.beginStructure(); a << s.id << s.options; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, GlobalShortcutEntry &s) { a.beginStructure(); a >> s.id >> s.options; a.endStructure(); return a; }

GlobalShortcut::GlobalShortcut(QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<GlobalShortcutEntry>(); qDBusRegisterMetaType<GlobalShortcutList>();
    qDBusRegisterMetaType<QList<QStringList>>(); qDBusRegisterMetaType<QList<int>>();
    requestTimeout_.setSingleShot(true);
    requestTimeout_.setInterval(120000);
    connect(&requestTimeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("全局快捷键配置超时，可使用托盘菜单截图")); });
    QDBusConnection::sessionBus().connect(service, path, iface, QStringLiteral("Activated"), this,
        SLOT(shortcutActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    QDBusConnection::sessionBus().connect(service, path, iface, QStringLiteral("ShortcutsChanged"), this,
        SLOT(shortcutsChanged(QDBusObjectPath,GlobalShortcutList)));
    QDBusConnection::sessionBus().connect(QStringLiteral("org.kde.kglobalaccel"),QStringLiteral("/kglobalaccel"),
        QStringLiteral("org.kde.KGlobalAccel"),QStringLiteral("yourShortcutGotChanged"),this,SLOT(kdeShortcutChanged(QStringList,QList<int>)));
    auto *watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &oldOwner, const QString &newOwner) {
        if (phase_ != Phase::Idle && !oldOwner.isEmpty() && oldOwner != newOwner)
            fail(QStringLiteral("桌面快捷键服务已重启，请重试或使用托盘截图。"));
    });
}
GlobalShortcut::~GlobalShortcut() {
    clearRequest();
    closeSession();
}
void GlobalShortcut::closeSession() {
    triggers_.clear(); confirmedSlots_.clear(); kdeActions_.clear();
    bindingsKnown_ = false;
    if (!sessionPath_.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(service, sessionPath_, QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this, SLOT(sessionClosed(QVariantMap)));
        auto msg = QDBusMessage::createMethodCall(service, sessionPath_, QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(msg);
        sessionPath_.clear();
    }
}
void GlobalShortcut::start() {
    if (phase_ != Phase::Idle) return;
    closeSession();
    phase_ = Phase::Creating;
    bindingText_ = QStringLiteral("正在向桌面注册截图快捷键…"); emit bindingChanged();
    request(QStringLiteral("CreateSession"), {}, {{QStringLiteral("session_handle_token"), token()}});
}
void GlobalShortcut::request(const QString &method, QList<QVariant> arguments, QVariantMap options) {
    auto bus = QDBusConnection::sessionBus();
    const QString handle = token();
    options.insert(QStringLiteral("handle_token"), handle);
    QString sender = bus.baseService().mid(1); sender.replace('.', '_');
    requestPath_ = QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + '/' + handle;
    // 在调用前订阅响应，避免桌面服务很快回复导致丢失信号。
    // Subscribe before the call so a fast portal response cannot be missed.
    if (!bus.connect(service, requestPath_, requestIface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)))) {
        fail(QStringLiteral("无法监听全局快捷键配置结果")); return;
    }
    auto message = QDBusMessage::createMethodCall(service, path, iface, method);
    arguments.append(options); message.setArguments(arguments);
    const QString expected = requestPath_;
    requestTimeout_.start();
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusObjectPath> reply = *call; call->deleteLater();
        if (requestPath_ != expected) return;
        if (reply.isError()) fail(QStringLiteral("全局快捷键服务不可用：%1").arg(reply.error().message()));
        else if (reply.value().path() != expected) fail(QStringLiteral("全局快捷键服务返回了无效请求路径"));
    });
}
void GlobalShortcut::clearRequest() {
    requestTimeout_.stop();
    if (!requestPath_.isEmpty()) QDBusConnection::sessionBus().disconnect(service, requestPath_, requestIface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)));
    requestPath_.clear();
}
void GlobalShortcut::fail(const QString &message) {
    const auto pending=pendingId_; const auto keys=pendingKeys_;
    clearRequest(); closeSession(); phase_ = Phase::Idle;
    if(saving_) completeEdit(pending,keys,false,message);
    bindingText_ = message; emit bindingChanged(); qWarning().noquote() << message; emit statusChanged(message);
}
void GlobalShortcut::response(uint code, const QVariantMap &results) {
    if (requestPath_.isEmpty()) return;
    clearRequest();
    if (code != 0) { fail(QStringLiteral("全局快捷键未启用，可使用托盘菜单截图")); return; }
    if (phase_ == Phase::Creating) {
        QVariant value = results.value(QStringLiteral("session_handle"));
        if (value.metaType() == QMetaType::fromType<QDBusVariant>()) value = qvariant_cast<QDBusVariant>(value).variant();
        sessionPath_ = value.metaType() == QMetaType::fromType<QDBusObjectPath>() ? qvariant_cast<QDBusObjectPath>(value).path() : value.toString();
        if (!sessionPath_.startsWith('/')) { fail(QStringLiteral("无法创建全局快捷键会话")); return; }
        QDBusConnection::sessionBus().connect(service, sessionPath_, QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this, SLOT(sessionClosed(QVariantMap)));
        phase_ = Phase::Binding;

        request(QStringLiteral("BindShortcuts"), {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QVariant::fromValue(requestedActions()), QString()}, {});
    } else if (phase_ == Phase::Binding) {
        phase_ = Phase::Ready;
        synchronizeKdeDefaults();
        refreshKdeBindings();
        // 响应中的列表才是实际绑定，不把建议键或空响应当成绑定成功。
        // The returned list is authoritative; the preferred trigger is only a suggestion.
        QVariant value = results.value(QStringLiteral("shortcuts"));
        if (value.metaType() == QMetaType::fromType<QDBusVariant>()) value = qvariant_cast<QDBusVariant>(value).variant();
        if (value.metaType() == QMetaType::fromType<QDBusArgument>()) applyBindings(qdbus_cast<GlobalShortcutList>(value), true);
        else if (value.canConvert<GlobalShortcutList>()) applyBindings(value.value<GlobalShortcutList>(), true);
        else { bindingText_ = QStringLiteral("桌面未返回快捷键绑定信息，请在桌面设置中确认。"); emit bindingChanged(); }
    }
}
// 默认建议匹配用户给出的原版界面：截图 Alt+1、贴图 Alt+2、翻译 Alt+3；OCR 不抢占按键。
// ID 保持稳定，门户返回的桌面自定义绑定仍然优先于这些默认建议。
// Match the reference UI: capture Alt+1, pin Alt+2, translation Alt+3; OCR has no default.
// Stable IDs preserve desktop overrides; portal responses remain authoritative.
const GlobalShortcutList &GlobalShortcut::actions() {
    static const GlobalShortcutList entries{
        {"capture", {{"description", QStringLiteral("截图")}, {"preferred_trigger", "ALT+1"}}},
        {"ocr", {{"description", QStringLiteral("框选文字识别")}}},
        {"translate", {{"description", QStringLiteral("翻译")}, {"preferred_trigger", "ALT+3"}}},
        {"pin_clipboard", {{"description", QStringLiteral("贴图剪贴板内容")}, {"preferred_trigger", "ALT+2"}}},
        {"screen_annotation", {{"description", QStringLiteral("屏幕标注")}, {"preferred_trigger", "ALT+4"}}},
    };
    return entries;
}
void GlobalShortcut::synchronizeKdeDefaults() {
    if (!QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains("KDE",Qt::CaseInsensitive)) return;
    // Portal preferred_trigger 用于新安装的初始建议；KDE 会保留旧动作的默认键元数据，
    // 因而旧版本留下的“无默认键”不会自动升级。只更新本应用已注册动作的默认值（IsDefault=8），
    // 不设置活动键、不重新注册或删除动作，保留用户已经选择的自定义组合键。
    // Portal preferences seed new installations, but KDE retains defaults for existing actions.
    // Update only our registered actions' default metadata (IsDefault=8), never active/user bindings.
    auto message=QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),QStringLiteral("/kglobalaccel"),
        QStringLiteral("org.kde.KGlobalAccel"),QStringLiteral("allActionsForComponent"));
    message.setAutoStartService(false);
    message.setArguments({QStringList{applicationId()}});
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,2000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QList<QStringList>> reply=*call; call->deleteLater();
        if(reply.isError() || phase_!=Phase::Ready) return;
        QMap<QString,QString> defaults;
        for(const auto &entry:actions()) defaults.insert(entry.id,entry.options.value("preferred_trigger").toString());
        for(const auto &action:reply.value()) {
            if(action.size()<4 || action[0]!=applicationId() || !defaults.contains(action[1])) continue;
            const auto trigger=defaults.value(action[1]);
            // 已接通动作只写内置的 Alt+数字默认建议；OCR 清空旧默认值，当前用户绑定仍保留。
            // Only built-in Alt+digit defaults are written; clear OCR's old default without changing active bindings.
            if(!trigger.isEmpty() && (trigger.size()!=5 || !trigger.startsWith("ALT+") || trigger.back()<'1' || trigger.back()>'4')) continue;
            QList<int> keys;
            if(!trigger.isEmpty()) keys.append(QKeyCombination(Qt::AltModifier,static_cast<Qt::Key>(Qt::Key_0+trigger.back().digitValue())).toCombined());
            auto update=QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),QStringLiteral("/kglobalaccel"),
                QStringLiteral("org.kde.KGlobalAccel"),QStringLiteral("setShortcut"));
            // 单键的 ai 接口兼容多个 KDE 版本；只传 IsDefault，绝不传 SetPresent/NoAutoloading。
            // The single-chord ai interface spans KDE versions. IsDefault never grabs or overwrites active keys.
            update.setAutoStartService(false);
            update.setArguments({action,QVariant::fromValue(keys),uint(8)});
            auto *setting=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(update,2000),this);
            connect(setting,&QDBusPendingCallWatcher::finished,this,[](QDBusPendingCallWatcher *done) {
                QDBusPendingReply<QList<int>> result=*done;
                if(result.isError()) qWarning().noquote()<<"KDE 默认快捷键同步失败:"<<result.error().message();
                done->deleteLater();
            });
        }
    });
}
QString GlobalShortcut::bindingText(const QString &id) const {
    if (!bindingsKnown_) return bindingText_;
    if (!triggers_.contains(id)) return QStringLiteral("未绑定");
    const QString binding = triggers_.value(id);
    return binding.isEmpty() ? QStringLiteral("桌面未提供按键描述") : binding;
}
void GlobalShortcut::applyBindings(const GlobalShortcutList &shortcuts, bool complete) {
    if (complete) {triggers_.clear(); confirmedSlots_.clear();}
    bindingsKnown_ = true;
    for (const auto &entry : shortcuts) {
        QVariant value = entry.options.value(QStringLiteral("trigger_description"));
        if (value.metaType() == QMetaType::fromType<QDBusVariant>()) value = qvariant_cast<QDBusVariant>(value).variant();
        const int marker=entry.id.indexOf("_slot_");
        const auto id=marker<0 ? entry.id : entry.id.left(marker);
        const int slot=marker<0 ? 0 : entry.id.mid(marker+6).toInt()-1;
        auto keys=confirmedSlots_.value(id); while(keys.size()<4) keys.append(QString());
        if(slot>=0 && slot<4) keys[slot]=value.toString().trimmed();
        confirmedSlots_[id]=keys;
        if(slot==0) triggers_.insert(id,value.toString().trimmed());
    }
    emit bindingChanged();
    if(saving_ && !pendingId_.isEmpty() && editingThroughPortal_) {
        const auto id=pendingId_; const auto keys=pendingKeys_;
        auto actual=confirmedSlots_.value(id); while(actual.size()<4) actual.append(QString());
        bool accepted=true;
        for(int i=0;i<4;++i) accepted &= QKeySequence::fromString(actual.value(i),QKeySequence::PortableText)==QKeySequence::fromString(keys.value(i),QKeySequence::PortableText);
        completeEdit(id,keys,accepted,accepted ? QString() : QStringLiteral("桌面未接受所录入的组合键，请在授权界面确认或更换按键。"));
    }
}
void GlobalShortcut::shortcutsChanged(const QDBusObjectPath &session, const GlobalShortcutList &shortcuts) {
    if (phase_ == Phase::Ready && session.path() == sessionPath_) applyBindings(shortcuts, false);
}
void GlobalShortcut::sessionClosed(const QVariantMap &) {
    fail(QStringLiteral("桌面已关闭快捷键会话，请重试或使用托盘截图。"));
}
void GlobalShortcut::configure() {
    if (phase_ != Phase::Ready || sessionPath_.isEmpty()) return;
    // ConfigureShortcuts 在 Portal v2 才支持，不能对同一个会话再次 BindShortcuts。
    // ConfigureShortcuts is a v2 method; BindShortcuts may only be called once per session.
    auto message = QDBusMessage::createMethodCall(service, path, iface, QStringLiteral("ConfigureShortcuts"));
    message << QVariant::fromValue(QDBusObjectPath(sessionPath_)) << QString() << QVariantMap{};
    const QString session = sessionPath_;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, session](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<> reply = *call; call->deleteLater();
        if (sessionPath_ != session || !reply.isError()) return;
        const QString message = QStringLiteral("无法打开快捷键配置，请在桌面系统设置中调整：%1").arg(reply.error().message());
        emit statusChanged(message);
    });
}
void GlobalShortcut::shortcutActivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &) {
    if (phase_ != Phase::Ready || session.path() != sessionPath_) return;
    const auto base=id.section("_slot_",0,0);
    for (const auto &entry : actions()) if (entry.id == base) {
        const auto keys = slotBindings(base);
        // 非 KDE 门户分别报告每个槽位；KDE 只报告动作。多绑定时不能猜是第一键并误触快速保存。
        // Other portals report the slot ID; KDE reports just the action. Don't guess which local command to run.
        const int slot = id == base ? 0 : id.section("_slot_",1).toInt() - 1;
        auto binding = keys.value(slot);
        if (kdeActions_.contains(base)) {
            auto active = keys;
            active.removeAll(QString());
            binding = active.size() == 1 ? active.first() : QString();
        }
        if(recording_) {emit shortcutCaptured(binding); return;}
        emit activated(base, binding); return;
    }
}
QStringList GlobalShortcut::slotBindings(const QString &id) const {
    auto keys=confirmedSlots_.contains(id) ? confirmedSlots_.value(id) : configured_.value(id);
    if(keys.isEmpty() && !bindingsKnown_) for(const auto &entry:actions()) if(entry.id==id) keys<<entry.options.value("preferred_trigger").toString();
    while(keys.size()<4) keys.append(QString()); return keys.mid(0,4);
}
bool GlobalShortcut::slotConfirmed(const QString &id,int slot) const {
    return bindingsKnown_ && !confirmedSlots_.value(id).value(slot).isEmpty();
}
GlobalShortcutList GlobalShortcut::requestedActions() const {
    const bool kde=!editingThroughPortal_ && QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains("KDE",Qt::CaseInsensitive);
    GlobalShortcutList result;
    for(auto entry:actions()) {
        const bool custom=configured_.contains(entry.id) || (saving_ && pendingId_==entry.id);
        auto keys=saving_ && pendingId_==entry.id ? pendingKeys_ : configured_.value(entry.id);
        if(kde || !custom) {result.append(entry); continue;}
        // 每槽一个稳定 portal ID，让无 KDE 接口的桌面也能授权多个组合键。
        // Stable per-slot portal IDs provide multi-binding authorization on non-KDE desktops.
        for(int slot=0;slot<keys.size() && slot<4;++slot) if(!keys[slot].isEmpty()) {
            auto action=entry; action.id=slot ? entry.id+QStringLiteral("_slot_%1").arg(slot+1) : entry.id;
            action.options["preferred_trigger"]=keys[slot].toUpper(); result.append(action);
        }
    }
    return result;
}
void GlobalShortcut::refreshKdeBindings() {
    if(!QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).contains("KDE",Qt::CaseInsensitive)) return;
    auto message=QDBusMessage::createMethodCall("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel","allActionsForComponent");
    message.setAutoStartService(false); message.setArguments({QStringList{applicationId()}});
    const auto session=sessionPath_;
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,2000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,session,watcher] {
        QDBusPendingReply<QList<QStringList>> reply=*watcher; watcher->deleteLater();
        if(reply.isError() || phase_!=Phase::Ready || sessionPath_!=session) return;
        for(const auto &action:reply.value()) {
            if(action.size()<4 || action[0]!=applicationId()) continue;
            bool known=false; for(const auto &entry:actions()) known|=entry.id==action[1]; if(!known) continue;
            kdeActions_[action[1]]=action;
            auto call=QDBusMessage::createMethodCall("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel","shortcut");
            call.setAutoStartService(false); call.setArguments({action});
            auto *keys=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call,2000),this);
            connect(keys,&QDBusPendingCallWatcher::finished,this,[this,keys,action,session] {
                QDBusPendingReply<QList<int>> result=*keys; keys->deleteLater();
                if(result.isError() || saving_ || sessionPath_!=session || phase_!=Phase::Ready) return;
                QStringList actual; for(int code:result.value()) if(code) actual<<QKeySequence(QKeyCombination::fromCombined(code)).toString(QKeySequence::PortableText);
                auto positions=configured_.value(action[1]); while(positions.size()<4) positions.append(QString());
                QStringList ordered{QString(),QString(),QString(),QString()};
                for(int i=0;i<4;++i) if(actual.removeOne(positions[i])) ordered[i]=positions[i];
                for(const auto &key:actual) for(auto &slot:ordered) if(slot.isEmpty()) {slot=key; break;}
                confirmedSlots_[action[1]]=ordered; triggers_[action[1]]=ordered.value(0); emit bindingChanged();
            });
        }
    });
}
void GlobalShortcut::changeSlot(const QString &id,int slot,const QString &value) {
    if(saving_) return;
    bool known=false; for(const auto &action:actions()) known|=action.id==id;
    const auto key=QKeySequence::fromString(value,QKeySequence::PortableText);
    if(!known || slot<0 || slot>3 || (!value.isEmpty() && (key.count()!=1 || key[0].key()==Qt::Key_unknown || key[0].key()==Qt::Key_Escape))) {
        emit editFinished(id,false,QStringLiteral("请输入有效的单个组合键，Esc 用于撤销录入。")); return;
    }
    if(!canConfigure()) {emit editFinished(id,false,QStringLiteral("桌面快捷键服务尚未就绪，请先重试注册。")); return;}
    const auto normalized=key.toString(QKeySequence::PortableText);
    for(const auto &action:actions()) {
        const auto existing=slotBindings(action.id);
        for(int i=0;i<existing.size();++i) if((action.id!=id || i!=slot) && !normalized.isEmpty() && QKeySequence::fromString(existing[i],QKeySequence::PortableText)==key) {
            emit editFinished(id,false,QStringLiteral("该按键已用于“%1”，请先清除原绑定。").arg(action.options.value("description").toString())); return;
        }
    }
    auto keys=slotBindings(id); keys[slot]=normalized; saving_=true; pendingId_=id; pendingKeys_=keys; emit bindingChanged();
    if(kdeActions_.contains(id)) {editingThroughPortal_=false; editKde(id,keys);}
    else {
        // 门户不提供无授权写入接口：新会话提交用户录入值，桌面确认后才标记成功。
        // Portals require desktop authorization; only a confirmed new session becomes effective.
        // 混合桌面即使带有 KDE 环境变量，也可能没有 KGlobalAccel 的本应用动作。
        // 按实际能力选择门户回退，不能仅按桌面名称等待一个永远不会到来的原生确认。
        // Mixed desktops may advertise KDE without a native action. Track the actual editing path,
        // so portal fallback still confirms or cancels instead of leaving the UI permanently busy.
        editingThroughPortal_=true;
        clearRequest(); closeSession(); phase_=Phase::Idle; start();
    }
}
void GlobalShortcut::editKde(const QString &id,const QStringList &keys) {
    QList<int> wanted,previous;
    for(const auto &key:keys) if(!key.isEmpty()) wanted<<QKeySequence::fromString(key,QKeySequence::PortableText)[0].toCombined();
    for(const auto &key:confirmedSlots_.value(id)) if(!key.isEmpty()) previous<<QKeySequence::fromString(key,QKeySequence::PortableText)[0].toCombined();
    const auto write=[this,id,keys,wanted,previous] {
        auto message=QDBusMessage::createMethodCall("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel","setShortcut");
        message.setAutoStartService(false);
        // NoAutoloading=4：配置当前应用已有动作，不重新注册、不抢占其他应用、不修改默认元数据。
        // Configure this application's existing action with NoAutoloading, without stealing or re-registering.
        message.setArguments({kdeActions_.value(id),QVariant::fromValue(wanted),uint(4)});
        auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,3000),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,id,keys,wanted,previous,watcher] {
            QDBusPendingReply<QList<int>> reply=*watcher; watcher->deleteLater();
            if(reply.isError()) {completeEdit(id,keys,false,reply.error().message()); return;}
            auto accepted=reply.value(),expected=wanted; accepted.removeAll(0); expected.removeAll(0);
            std::sort(accepted.begin(),accepted.end()); std::sort(expected.begin(),expected.end());
            if(accepted==expected) {completeEdit(id,keys,true); return;}
            auto rollback=QDBusMessage::createMethodCall("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel","setShortcut");
            rollback.setAutoStartService(false); rollback.setArguments({kdeActions_.value(id),QVariant::fromValue(previous),uint(4)});
            auto *restore=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(rollback,3000),this);
            connect(restore,&QDBusPendingCallWatcher::finished,this,[this,id,keys,restore] {restore->deleteLater(); completeEdit(id,keys,false,QStringLiteral("桌面拒绝了部分组合键，已请求恢复原绑定。"));});
        });
    };
    if(wanted.isEmpty()) {write(); return;}
    auto remaining=std::make_shared<int>(wanted.size()); auto rejected=std::make_shared<bool>(false);
    for(int code:wanted) {
        auto message=QDBusMessage::createMethodCall("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel","isGlobalShortcutAvailable");
        message.setAutoStartService(false); message.setArguments({code,applicationId()});
        auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,2000),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,id,keys,watcher,remaining,rejected,write] {
            QDBusPendingReply<bool> reply=*watcher; watcher->deleteLater(); *rejected|=reply.isError() || !reply.value();
            if(--*remaining==0) {if(*rejected) completeEdit(id,keys,false,QStringLiteral("该组合键已被其他应用占用，或桌面无法检查冲突。")); else write();}
        });
    }
}
void GlobalShortcut::completeEdit(const QString &id,const QStringList &keys,bool success,const QString &error) {
    const QString completedId=id; const QStringList completedKeys=keys;
    saving_=false; pendingId_.clear(); pendingKeys_.clear();
    if(success) {bindingsKnown_=true; configured_[completedId]=completedKeys; confirmedSlots_[completedId]=completedKeys; triggers_[completedId]=completedKeys.value(0);}
    emit bindingChanged(); emit editFinished(completedId,success,error);
}

void GlobalShortcut::kdeShortcutChanged(const QStringList &action,const QList<int> &) {
    // 只重新读取本应用的已注册动作；系统设置修改后同步显示，不监听或改写其他应用。
    // Refresh only our registered actions when desktop settings change them.
    if(!saving_ && phase_==Phase::Ready && action.value(0)==applicationId() && kdeActions_.contains(action.value(1))) refreshKdeBindings();
}
