#include "global_shortcut.h"
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QTimer>
#include <QKeySequence>
#include <cstdio>

struct FakeShortcut { QString id; QVariantMap options; };
using FakeShortcuts = QList<FakeShortcut>;
Q_DECLARE_METATYPE(FakeShortcut)
Q_DECLARE_METATYPE(FakeShortcuts)
QDBusArgument &operator<<(QDBusArgument &a, const FakeShortcut &s) { a.beginStructure(); a << s.id << s.options; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, FakeShortcut &s) { a.beginStructure(); a >> s.id >> s.options; a.endStructure(); return a; }

/**
 * 私有 KDE 快捷键元数据服务：只接受本应用四个动作的默认值更新，拒绝活动键/其他应用写入。
 * Fake KDE metadata service accepts only our four defaults, rejecting active bindings or foreign actions.
 */
class FakeKGlobalAccel final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.kde.KGlobalAccel")
public:
    QMap<QString,int> defaults;
    QMap<QString,QList<int>> active{{"capture",{int(Qt::ControlModifier)|int(Qt::ShiftModifier)|int(Qt::Key_S)}},{"ocr",{int(Qt::AltModifier)|int(Qt::Key_2)}}};
    int writes=0;

public slots:
    QList<QStringList> allActionsForComponent(const QStringList &component) {
        if(component!=QStringList{GlobalShortcut::applicationId()}) { sendErrorReply(QDBusError::InvalidArgs,"wrong component"); return {}; }
        QList<QStringList> result;
        for(const auto &entry:GlobalShortcut::actions()) result.append({component[0],entry.id,"截图吧",entry.options.value("description").toString()});
        result.append({"unrelated-app","capture","Other","Other action"});
        result.append({component[0],"untouched","截图吧","Unrelated action"});
        return result;
    }
    QList<int> shortcut(const QStringList &action) {return active.value(action.value(1));}
    bool isGlobalShortcutAvailable(int key,const QString &component) {return component==GlobalShortcut::applicationId() && key!=int(Qt::Key_F8);}
    QList<int> setShortcut(const QStringList &action,const QList<int> &keys,uint flags) {
        if(flags==4 && action.size()==4 && action[0]==GlobalShortcut::applicationId() && action[1]=="capture") {
            ++writes;
            auto accepted=keys; accepted.removeAll(int(Qt::Key_F9)); active[action[1]]=accepted; return accepted;
        }
        const QMap<QString,int> wanted{{"capture",int(Qt::AltModifier)|int(Qt::Key_1)},
            {"pin_clipboard",int(Qt::AltModifier)|int(Qt::Key_2)},{"ocr",0},
            {"translate",int(Qt::AltModifier)|int(Qt::Key_3)},{"screen_annotation",int(Qt::AltModifier)|int(Qt::Key_4)}};
        if(action.size()!=4 || action[0]!=GlobalShortcut::applicationId() || !wanted.contains(action[1])
            || keys!=(wanted.value(action[1]) ? QList<int>{wanted.value(action[1])} : QList<int>{}) || flags!=8) {
            sendErrorReply(QDBusError::InvalidArgs,"must only update our default metadata"); return {};
        }
        defaults.insert(action[1],keys.value(0));
        return keys;
    }
};

/**
 * 私有 D-Bus 上的快捷键 portal，验证会话、快捷键绑定及激活信号。
 * Fake portal on a private bus verifies session creation, binding and activation.
 */
class FakePortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options) {
        return respond(options, {{"session_handle", QString("/org/freedesktop/portal/desktop/session/test/session")}});
    }
    QDBusObjectPath BindShortcuts(const QDBusObjectPath &session, const FakeShortcuts &shortcuts, const QString &, const QVariantMap &options) {
        if(QCoreApplication::arguments().contains("--portable") || QCoreApplication::arguments().contains("--portable-denied") || QCoreApplication::arguments().contains("--portable-kde")) {
            FakeShortcuts bound;
            for(const auto &entry:shortcuts) {
                const auto key=entry.options.value("preferred_trigger").toString();
                if(QCoreApplication::arguments().contains("--portable-denied") && entry.id=="capture" && key=="F6") return respond(options,{},uint(1));
                if(!key.isEmpty()) bound.append({entry.id,{{"trigger_description",QKeySequence::fromString(key,QKeySequence::PortableText).toString(QKeySequence::PortableText)}}});
            }
            return respond(options,{{"shortcuts",QVariant::fromValue(bound)}});
        }
        QStringList ids;
        for (const auto &entry : shortcuts) ids << entry.id;
        if (session.path() != "/org/freedesktop/portal/desktop/session/test/session" || ids != QStringList{"capture", "ocr", "translate", "pin_clipboard", "screen_annotation"}) {
            sendErrorReply(QDBusError::Failed, "incorrect binding"); return {};
        }
        // 核对每个动作的默认建议；下面仍返回不同的用户绑定，验证桌面设置优先。
        // Verify each preferred trigger, then return different bindings to exercise desktop overrides.
        const QMap<QString, QString> defaults{{"capture", "ALT+1"}, {"ocr", ""},
                                              {"translate", "ALT+3"}, {"pin_clipboard", "ALT+2"}, {"screen_annotation", "ALT+4"}};
        for (const auto &entry : shortcuts) {
            if (entry.options.value("preferred_trigger").toString() != defaults.value(entry.id)) {
                sendErrorReply(QDBusError::Failed, "incorrect default trigger"); return {};
            }
        }
        const FakeShortcuts bound = QCoreApplication::arguments().contains("--empty") ? FakeShortcuts{} : FakeShortcuts{{"capture", {{"trigger_description", "Ctrl+Shift+S"}}}, {"ocr", {{"trigger_description", "Alt+2"}}}};
        return respond(options, {{"shortcuts", QVariant::fromValue(bound)}});
    }
    void ConfigureShortcuts(const QDBusObjectPath &session, const QString &, const QVariantMap &) {
        if (QCoreApplication::arguments().contains("--no-config")) {
            sendErrorReply(QDBusError::UnknownMethod, QStringLiteral("not supported")); return;
        }
        QTimer::singleShot(10, this, [session] {
            auto changed = QDBusMessage::createSignal("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "ShortcutsChanged");
            changed << QVariant::fromValue(session) << QVariant::fromValue(FakeShortcuts{{"capture", {{"trigger_description", "Alt+F8"}}}});
            QDBusConnection::sessionBus().send(changed);
        });
        QTimer::singleShot(30, this, [session] {
            for (const QString id : {"unknown", "capture", "ocr", "translate", "screen_annotation", "pin_clipboard"}) {
                auto activation = QDBusMessage::createSignal("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "Activated");
                activation << QVariant::fromValue(session) << id << QVariant::fromValue(qulonglong(42)) << QVariantMap{};
                QDBusConnection::sessionBus().send(activation);
            }
        });
    }
private:
    QDBusObjectPath respond(const QVariantMap &options, const QVariantMap &results,uint code=0) {
        QString sender = message().service().mid(1); sender.replace('.', '_');
        const QString path = "/org/freedesktop/portal/desktop/request/" + sender + '/' + options.value("handle_token").toString();
        QTimer::singleShot(10, this, [path, results,code] {
            auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            signal << code << results; QDBusConnection::sessionBus().send(signal);
        });
        return QDBusObjectPath(path);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const bool portable=app.arguments().contains("--portable") || app.arguments().contains("--portable-denied") || app.arguments().contains("--portable-kde");
    qputenv("XDG_CURRENT_DESKTOP",portable && !app.arguments().contains("--portable-kde") ? "GNOME" : "KDE");
    qDBusRegisterMetaType<QList<QStringList>>(); qDBusRegisterMetaType<QList<int>>();
    qDBusRegisterMetaType<FakeShortcut>(); qDBusRegisterMetaType<FakeShortcuts>();
    FakePortal portal;
    FakeKGlobalAccel kde;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop") || !bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots)) return 1;
    if(!portable && (!bus.registerService("org.kde.kglobalaccel") || !bus.registerObject("/kglobalaccel",&kde,QDBusConnection::ExportAllSlots))) return 8;
    GlobalShortcut shortcut;
    if(portable) {
        int step=0; bool started=false;
        QObject::connect(&shortcut,&GlobalShortcut::bindingChanged,&app,[&] {
            if(!started && shortcut.canConfigure()) {started=true; QTimer::singleShot(0,&shortcut,[&] {shortcut.changeSlot("capture",0,{});});}
        });
        QObject::connect(&shortcut,&GlobalShortcut::editFinished,&app,[&](const QString &id,bool ok,const QString &error) {
            const bool denied=app.arguments().contains("--portable-denied") && step==1;
            if(id!="capture" || ok==denied || shortcut.isSaving()) {std::fprintf(stderr,"portable edit: %s\n",qPrintable(error)); app.exit(30); return;}
            if(denied) {app.exit(shortcut.canRetry() && shortcut.slotBindings("capture").value(0).isEmpty() ? 0 : 31); return;}
            ++step;
            QTimer::singleShot(0,&shortcut,[&] {
                if(step==1) shortcut.changeSlot("capture",0,"F6");
                else if(step==2) shortcut.changeSlot("capture",3,"F7");
                else {const auto keys=shortcut.slotBindings("capture"); app.exit(keys[0]=="F6" && keys[1].isEmpty() && keys[2].isEmpty() && keys[3]=="F7" && kde.defaults.isEmpty() ? 0 : 32);}
            });
        });
        QTimer::singleShot(7000,&app,[&] {app.exit(33);}); shortcut.start(); return app.exec();
    }
    if(app.arguments().contains("--edit")) {
        // 所有系统写入均在私有 D-Bus 上：测试清空、多个槽、冲突和部分拒绝后的恢复。
        // Exercise clear, multiple slots, conflict checks and partial-write rollback on a private bus only.
        int step=0; bool started=false;
        const auto waitNative=[&] {
            if(started || !shortcut.canConfigure() || kde.defaults.size()!=5) return;
            started=true; QTimer::singleShot(40,&shortcut,[&] {shortcut.changeSlot("capture",0,{});});
        };
        QObject::connect(&shortcut,&GlobalShortcut::bindingChanged,&app,waitNative);
        QObject::connect(&shortcut,&GlobalShortcut::editFinished,&app,[&](const QString &id,bool ok,const QString &error) {
            if(id!="capture" || ok!=(step!=3 && step!=4 && step!=5)) {std::fprintf(stderr,"edit step %d unexpected: %s\n",step,qPrintable(error)); app.exit(21); return;}
            const auto keys=shortcut.slotBindings(id);
            if(step==0 && !keys[0].isEmpty()) {app.exit(22); return;}
            if(step>=2 && (keys[0]!="F6" || keys[1]!="F7")) {app.exit(23); return;}
            ++step;
            QTimer::singleShot(0,&shortcut,[&] {
                if(step==1) shortcut.changeSlot("capture",0,"F6");
                else if(step==2) shortcut.changeSlot("capture",1,"F7");
                else if(step==3) shortcut.changeSlot("capture",2,"F7");
                else if(step==4) shortcut.changeSlot("capture",2,"F8");
                else if(step==5) shortcut.changeSlot("capture",2,"F9");
                else if(step==6) shortcut.changeSlot("capture",2,"F10");
                else if(step==7) shortcut.changeSlot("capture",3,"F11");
                else if(step==8) shortcut.changeSlot("capture",2,{});
                else if(step==9) {if(kde.active.value("capture")!=QList<int>{int(Qt::Key_F6),int(Qt::Key_F7),int(Qt::Key_F11)} || kde.writes!=8 || shortcut.slotBindings("capture")[3]!="F11" || !shortcut.slotBindings("capture")[2].isEmpty()) app.exit(24); else app.exit(0);}
            });
        });
        QTimer::singleShot(7000,&app,[&] {app.exit(25);}); shortcut.start(); return app.exec();
    }
    bool initialChecked = false, changedChecked = false;
    QObject::connect(&shortcut, &GlobalShortcut::bindingChanged, &app, [&] {
        if (!shortcut.canConfigure()) return;
        if (!initialChecked) {
            const bool empty = app.arguments().contains("--empty");
            if (!(empty ? shortcut.bindingText().contains("未绑定") : shortcut.bindingText().contains("Ctrl+Shift+S"))) { std::fprintf(stderr, "incorrect initial state: %s\n", qPrintable(shortcut.bindingText())); app.exit(4); return; }
            if (shortcut.triggerDescription()!=(empty ? QString() : QStringLiteral("Ctrl+Shift+S"))) { app.exit(6); return; }
            if (shortcut.triggerDescription("ocr") != (empty ? QString() : QStringLiteral("Alt+2"))
                || !shortcut.bindingText("translate").contains("未绑定")) { app.exit(7); return; }
            initialChecked = true;
            shortcut.configure();
        } else if (shortcut.bindingText().contains("Alt+F8")) changedChecked = shortcut.triggerDescription()=="Alt+F8";
    });
    QStringList received;
    QObject::connect(&shortcut, &GlobalShortcut::activated, &app, [&](const QString &id) {
        received << id;
        if (id == "pin_clipboard") app.exit(initialChecked && changedChecked && kde.defaults.size()==5
            && received == QStringList{"capture", "ocr", "translate", "screen_annotation", "pin_clipboard"} ? 0 : 5);
    });
    QObject::connect(&shortcut, &GlobalShortcut::statusChanged, &app, [&](const QString &error) {
        if (app.arguments().contains("--no-config") && initialChecked && shortcut.canConfigure() && error.contains("not supported")) { app.exit(0); return; }
        std::fprintf(stderr, "%s\n", qPrintable(error)); app.exit(2);
    });
    QTimer::singleShot(5000, &app, [&] { std::fprintf(stderr, "shortcut test timed out\n"); app.exit(3); });
    QTimer::singleShot(0, &shortcut, &GlobalShortcut::start);
    return app.exec();
}
#include "global_shortcut_test.moc"
