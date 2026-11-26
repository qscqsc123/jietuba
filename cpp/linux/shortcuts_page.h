#pragma once
#include <QWidget>
#include <QMap>
#include <QPointer>
class LinuxSettings;
class GlobalShortcut;
class QLabel;
class QPushButton;
class LinuxShortcutEdit;
/**
 * Linux 快捷键页：复用原版圆角分组、键位输入、状态图标和四个应用内页签。
 * 本地绑定直接校验和保存；全局绑定仅在桌面确认后保存；KDE 直接编辑本应用动作，其他桌面由门户授权。
 * 所有绿色状态只来自桌面实际返回值，未移植的动作只显示禁用占位，不注册无效快捷键。
 * KDE、GNOME、niri 使用相同页面，不根据发行版名称选择实现，也不强行改写桌面用户绑定。
 * Linux shortcut page sharing original section/chip/status/tab widgets with Windows.
 * Local edits validate and persist; global edits require desktop confirmation (native KDE or portal).
 * Confirmed desktop bindings alone get a green status. Unported actions remain disabled placeholders.
 * The same UI works across compositors, without distro-specific assumptions or overwriting user bindings.
 */
class LinuxShortcutsPage final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxShortcutsPage(LinuxSettings &settings,QWidget *parent=nullptr);
    void setShortcut(GlobalShortcut *shortcut);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void refreshGlobals();
    void configureGlobal();
    LinuxSettings &settings_;
    QPointer<GlobalShortcut> shortcut_;
    QMap<QString,QWidget *> globalRows_;
    QMap<QString,LinuxShortcutEdit *> editors_;
    QLabel *message_;
    QPushButton *configure_,*retry_;
};
