#pragma once
#include <QWidget>
#include <QMap>
#include <QPointer>
class LinuxSettings;
class LinuxShortcutsPage;
class QStackedWidget;
class GlobalShortcut;
class QPushButton;
class QVBoxLayout;
namespace jietuba { class SwitchSettingCard; }
namespace jietuba { class SettingCard; class NavigationInterface; }

/**
 * Linux 主设置窗口：复用原版导航与设置卡片，展示已经接通的选项。
 * 设置修改后立即写入配置并作用于后续截图；尚未移植的功能明确展示状态。
 *
 * Linux settings window built with the original navigation and card components.
 * Implemented options are persisted immediately; unsupported sections are labelled.
 */
class LinuxSettingsWindow final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxSettingsWindow(LinuxSettings &settings, QWidget *parent = nullptr);
    void setShortcut(GlobalShortcut *shortcut);
    void showCapturePage();
    void showShortcutsPage();
    void showTranslationPage();
signals:
    void captureRequested();
protected:
    void changeEvent(QEvent *event) override;
private:
    void buildPages();
    void importSettings();
    void persist();
    LinuxSettings &settings_;
    QVBoxLayout *rootLayout_ = nullptr;
    QWidget *content_ = nullptr;
    QPointer<GlobalShortcut> shortcut_;
    jietuba::SwitchSettingCard *autostart_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    jietuba::NavigationInterface *navigation_ = nullptr;
    int translationPageIndex_ = -1;
    int capturePageIndex_ = -1;
    int shortcutsPageIndex_ = -1;
    int otherPageIndex_ = -1;
    LinuxShortcutsPage *shortcutsPage_ = nullptr;
};
