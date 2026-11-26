#include "shortcuts_page.h"
#include "global_shortcut.h"
#include "linux_settings.h"
#include "shortcut_bindings.h"
#include "shortcut_edit.h"
#include <QApplication>
#include "core/resource_manager.h"
#include "ui/shortcut_section.h"
#include "ui/key_chip.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/segmented.h"
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSet>

namespace {
/**
 * 四个全局键位按原版两行两列排列，窄窗口自动折成单列，避免挤掉动作名称。
 * Four global slots use the original two-by-two grid and reflow on narrow windows.
 */
class GlobalRow final : public QWidget {
public:
    GlobalRow(QWidget *parent,const QString &title,const QString &tone,jietuba::FluentIcon icon):QWidget(parent) {
        auto *row=new QHBoxLayout(this); row->setContentsMargins(0,8,0,8); row->setSpacing(14);
        row->addWidget(new jietuba::IconBadge(icon,tone,this,28,16));
        auto *name=new QLabel(title,this); name->setMinimumWidth(68); name->setWordWrap(true); row->addWidget(name,1);
        auto *host=new QWidget(this); grid_=new QGridLayout(host); grid_->setContentsMargins(0,0,0,0); grid_->setHorizontalSpacing(10); grid_->setVerticalSpacing(5);
        host->setMinimumWidth(0); host->setMaximumWidth(462); row->addWidget(host,3);
        for(int i=0;i<4;++i) {
            auto *slot=new QWidget(host); auto *layout=new QHBoxLayout(slot); layout->setContentsMargins(0,0,0,0); layout->setSpacing(10);
            auto *edit=new LinuxShortcutEdit(slot,true); edit->setObjectName(QStringLiteral("globalChip_%1").arg(i)); edit->setMinimumWidth(80);
            auto *status=new jietuba::StatusIcon(slot); status->setObjectName(QStringLiteral("globalStatus_%1").arg(i));
            layout->addWidget(edit,1); layout->addWidget(status); slots_.append(slot);
        }
        arrange(2);
    }
protected:
    void resizeEvent(QResizeEvent *event) override {QWidget::resizeEvent(event); arrange(width()<590 ? 1 : 2);}
private:
    void arrange(int columns) {
        if(columns_==columns) return; columns_=columns;
        for(int i=0;i<slots_.size();++i) grid_->addWidget(slots_[i],columns==2 ? i%2 : i,columns==2 ? i/2 : 0);
    }
    QGridLayout *grid_;
    QList<QWidget *> slots_;
    int columns_=-1;
};
/**
 * 高度跟随当前页，避免工具页较长时把“截图”或占位页撑出大片空白。
 * Follow the current page height rather than reserving space for the longest tab.
 */
class ShortcutStack final : public QStackedWidget {
public:
    using QStackedWidget::QStackedWidget;
    QSize sizeHint() const override {return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint();}
    QSize minimumSizeHint() const override {return currentWidget() ? currentWidget()->minimumSizeHint() : QStackedWidget::minimumSizeHint();}
};
QString tabFor(const LinuxShortcutAction &action) {
    if(action.context=="pin") return "pin";
    return linuxIsToolShortcut(action.id) ? "tools" : "capture";
}
jietuba::IconSource iconFor(const QString &id) {
    static const QMap<QString,QString> names{{"copy","确定.svg"},{"pin","钉图.svg"},{"save","下载.svg"},{"quick_save","快速保存.svg"},
        {"cursor","鼠标.svg"},{"pen","画笔.svg"},{"highlighter","荧光笔.svg"},{"mosaic","马赛克.svg"},{"arrow","箭头.svg"},
        {"number","序号线框.svg"},{"rect","方框.svg"},{"ellipse","圆框.svg"},{"text","文字.svg"},{"eraser","橡皮.svg"},
        {"undo","撤回.svg"},{"redo","复原.svg"},{"pin_copy","复制.svg"},{"pin_save","下载.svg"},{"pin_toggle_toolbar","切换工具栏.svg"},
        {"ocr","文字识别.svg"},{"barcode","扫码.svg"},{"long_capture","长截图.svg"},{"gif","gif.svg"},{"copy_color","颜色设置.svg"},
        {"zoom_in","zoom_in.svg"},{"zoom_out","zoom_out.svg"},{"spotlight","聚光灯.svg"},{"watermark","水印.svg"},
        {"pin_reset","重置大小.svg"},{"pin_zoom_in","zoom_in.svg"},{"pin_zoom_out","zoom_out.svg"},{"pin_undo","撤回.svg"},{"pin_redo","复原.svg"}};
    if(names.contains(id)) return jietuba::ResourceManager::get_icon_path(names.value(id));
    if(id=="delete") return jietuba::FluentIcon::Delete;
    if(id=="restore_region") return jietuba::FluentIcon::History;
    if(id=="translate") return jietuba::FluentIcon::Language;
    return jietuba::FluentIcon::CommandPrompt;
}
}
LinuxShortcutsPage::LinuxShortcutsPage(LinuxSettings &settings,QWidget *parent):QWidget(parent),settings_(settings) {
    QFont pageFont=font(); pageFont.setPixelSize(14); setFont(pageFont);
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(16);
    auto *globals=new jietuba::ShortcutSection(jietuba::ResourceManager::get_icon_path(QStringLiteral("热键.svg")),QStringLiteral("全局快捷键"),QStringLiteral("在任何时候都能触发 · 由桌面授权"),this);
    struct Feature {const char *id,*title,*tone; jietuba::FluentIcon icon;};
    for(const auto &feature:QList<Feature>{{"capture","截图","capture",jietuba::FluentIcon::Camera},{"pin_clipboard","钉图","pin",jietuba::FluentIcon::Pin},
        {"clipboard","剪贴板（尚未接入）","clipboard",jietuba::FluentIcon::Paste},{"translate","翻译","translate",jietuba::FluentIcon::Language},
        {"screen_annotation","屏幕标注","capture",jietuba::FluentIcon::Brush},{"ocr","文字识别","capture",jietuba::FluentIcon::Search}}) {
        auto *row=new GlobalRow(globals,QString::fromUtf8(feature.title),QString::fromUtf8(feature.tone),feature.icon);
        row->setObjectName(QStringLiteral("globalShortcutRow_")+feature.id); globalRows_[feature.id]=row;
        if(QString::fromLatin1(feature.id)=="ocr") {
            auto *extra=new jietuba::PushButton(QStringLiteral("其他动作：文字识别"),globals); extra->setCheckable(true);
            globals->addWidget(extra); connect(extra,&QPushButton::toggled,row,&QWidget::setVisible); row->hide();
        }
        for(int i=0;i<4;++i) {
            auto *edit=row->findChild<LinuxShortcutEdit *>(QStringLiteral("globalChip_%1").arg(i));
            connect(edit,&LinuxShortcutEdit::commitRequested,this,[this,id=QString::fromLatin1(feature.id),i](const QString &value) {
                if(shortcut_) shortcut_->changeSlot(id,i,value);
            });
            connect(edit,&LinuxShortcutEdit::recordingChanged,this,[this](bool recording) {if(shortcut_) shortcut_->setRecordingKey(recording);});
        }
        globals->addRow(row);
    }
    auto *desktop=new QWidget(globals); auto *desktopLayout=new QHBoxLayout(desktop); desktopLayout->setContentsMargins(0,8,0,8);
    configure_=new jietuba::PushButton(QStringLiteral("配置全局快捷键"),desktop); configure_->setObjectName("configureShortcut");
    retry_=new jietuba::PushButton(QStringLiteral("重试注册"),desktop); retry_->setObjectName("retryShortcut");
    desktopLayout->addStretch(); desktopLayout->addWidget(configure_); desktopLayout->addWidget(retry_); globals->addWidget(desktop); layout->addWidget(globals);
    auto *local=new jietuba::ShortcutSection(jietuba::FluentIcon::Layout,QStringLiteral("应用内快捷键"),QStringLiteral("只在对应的应用窗口中生效"),this);
    auto *tabs=new jietuba::SegmentedWidget(local); tabs->setObjectName("linuxInAppShortcutTabs"); tabs->setLarge(true);
    auto *tabRow=new QWidget(local); auto *tabLayout=new QHBoxLayout(tabRow); tabLayout->setContentsMargins(0,0,0,10); tabLayout->addWidget(tabs); tabLayout->addStretch(); local->addWidget(tabRow);
    auto *stack=new ShortcutStack(local); stack->setObjectName("InAppShortcutStack");
    connect(stack,&QStackedWidget::currentChanged,this,[stack] {stack->updateGeometry();});
    const QStringList routes{"capture","tools","pin","clipboard"},titles{QStringLiteral("截图"),QStringLiteral("标注工具"),QStringLiteral("钉图"),QStringLiteral("剪贴板")};
    for(int i=0;i<routes.size();++i) {
        const auto route=routes[i]; auto *page=new QWidget(stack); auto *rows=new QVBoxLayout(page); rows->setContentsMargins(0,0,0,0); rows->setSpacing(0);
        stack->addWidget(page); tabs->addItem(route,titles[i],[stack,i] {stack->setCurrentIndex(i);});
        for(const auto &action:linuxShortcutActions()) {
            if(tabFor(action)!=route) continue;
            auto *row=new QWidget(page); row->setMinimumHeight(44); auto *line=new QHBoxLayout(row); line->setContentsMargins(0,4,30,4); line->setSpacing(14);
            line->addWidget(new jietuba::IconBadge(iconFor(action.id),QString(),row,28,18));
            auto *name=new QLabel(action.id=="copy" ? QStringLiteral("复制到剪贴板") : action.title,row); name->setWordWrap(true); line->addWidget(name,1);
            auto *editor=new LinuxShortcutEdit(row); editor->setPlaceholderText(QStringLiteral("添加快捷键")); editor->setMaximumWidth(jietuba::CHIP_WIDTH);
            editor->setObjectName(QStringLiteral("localShortcut_")+action.id); editor->setCommittedValue(settings_.shortcut(action.id).toLower());
            editor->setToolTip(QStringLiteral("先点 × 清除，再点击空框录入组合键或中键；Esc 撤销录入。")); editors_[action.id]=editor; line->addWidget(editor);
            connect(editor,&LinuxShortcutEdit::recordingChanged,this,[this](bool recording) {if(shortcut_) shortcut_->setRecordingKey(recording);});
            connect(editor,&LinuxShortcutEdit::commitRequested,this,[this,editor,id=action.id](const QString &value) {
                if(value.endsWith('+') && !value.endsWith("++")) return;
                QString error;
                if(!settings_.setShortcut(id,value,&error)) {
                    editor->setErrorState(true); message_->setText(error); editor->setCommittedValue(settings_.shortcut(id).toLower()); editor->setErrorState(true);
                } else {editor->setCommittedValue(settings_.shortcut(id).toLower()); message_->clear();}
            });
            rows->addWidget(row);
        }
        if(route=="clipboard") {
            auto *hint=new QLabel(QStringLiteral("剪贴板历史管理尚未接入，以下快捷键暂不可用。"),page); hint->setWordWrap(true); rows->addWidget(hint);
            for(const auto &entry:QList<QPair<QString,QString>>{{QStringLiteral("快捷编辑"),"Tab"},{QStringLiteral("搜索"),"Ctrl+F"},{QStringLiteral("保存编辑"),"Ctrl+Enter"}}) {
                auto *row=new QWidget(page); auto *line=new QHBoxLayout(row); line->addWidget(new QLabel(entry.first,row),1);
                auto *chip=new jietuba::KeyChipLineEdit(row); chip->setText(entry.second); chip->setEnabled(false); chip->setClearButtonVisible(false); chip->setMaximumWidth(jietuba::CHIP_WIDTH); line->addWidget(chip); rows->addWidget(row);
            }
        }
        rows->addStretch();
    }
    tabs->setCurrentItem("capture"); local->addWidget(stack); layout->addWidget(local);
    auto *reset=new jietuba::PushButton(QStringLiteral("重置应用内快捷键"),this); reset->setObjectName("resetLocalShortcuts"); layout->addWidget(reset,0,Qt::AlignRight);
    connect(reset,&QPushButton::clicked,this,[this] {
        settings_.resetShortcuts(); for(auto it=editors_.begin();it!=editors_.end();++it) {const QSignalBlocker block(it.value()); it.value()->setCommittedValue(settings_.shortcut(it.key()).toLower()); it.value()->setErrorState(false);} message_->clear();
    });
    message_=new QLabel(this); message_->setObjectName("shortcutStatusMessage"); message_->setWordWrap(true); layout->addWidget(message_);
    connect(configure_,&QPushButton::clicked,this,&LinuxShortcutsPage::configureGlobal);
    connect(retry_,&QPushButton::clicked,this,[this] {if(shortcut_) shortcut_->start();}); refreshGlobals();
}
void LinuxShortcutsPage::setShortcut(GlobalShortcut *shortcut) {
    if(shortcut_) disconnect(shortcut_,nullptr,this,nullptr); shortcut_=shortcut;
    if(shortcut_) {
        QMap<QString,QStringList> saved;
        for(const auto &action:GlobalShortcut::actions()) if(settings_.hasGlobalShortcutSlots(action.id)) saved[action.id]=settings_.globalShortcutSlots(action.id);
        shortcut_->setConfiguredSlots(saved);
        connect(shortcut_,&GlobalShortcut::shortcutCaptured,this,[this](const QString &key) {
            if(auto *edit=qobject_cast<LinuxShortcutEdit *>(QApplication::focusWidget())) edit->captureFromDesktop(key);
        });
        connect(shortcut_,&GlobalShortcut::editFinished,this,[this](const QString &id,bool success,const QString &error) {
            if(success) {settings_.setGlobalShortcutSlots(id,shortcut_->slotBindings(id)); settings_.sync(); message_->clear();}
            else message_->setText(error);
            refreshGlobals();
        });
        connect(shortcut_,&GlobalShortcut::bindingChanged,this,&LinuxShortcutsPage::refreshGlobals);
        connect(shortcut_,&GlobalShortcut::statusChanged,this,[this](const QString &message) {message_->setText(message);});
    }
    refreshGlobals();
}
void LinuxShortcutsPage::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if(auto *tabs=findChild<jietuba::SegmentedWidget *>("linuxInAppShortcutTabs")) {
        tabs->setLarge(width()>=600); tabs->setCompact(width()<600,8);
    }
}
void LinuxShortcutsPage::configureGlobal() {
    if(shortcut_ && shortcut_->canConfigure()) shortcut_->configure();
    else message_->setText(QStringLiteral("当前桌面未提供可用的全局快捷键配置。可重试门户注册，或在桌面设置中绑定本程序动作。"));
}
void LinuxShortcutsPage::refreshGlobals() {
    const bool ready=shortcut_ && shortcut_->canConfigure(); configure_->setEnabled(ready && !shortcut_->isSaving()); retry_->setVisible(!shortcut_ || shortcut_->canRetry());
    QSet<QString> implemented; for(const auto &entry:GlobalShortcut::actions()) implemented.insert(entry.id);
    for(auto it=globalRows_.begin();it!=globalRows_.end();++it) {
        const bool supported=implemented.contains(it.key());
        for(int i=0;i<4;++i) {
            auto *chip=it.value()->findChild<LinuxShortcutEdit *>(QStringLiteral("globalChip_%1").arg(i));
            auto *status=it.value()->findChild<jietuba::StatusIcon *>(QStringLiteral("globalStatus_%1").arg(i));
            const auto actual=shortcut_ ? shortcut_->slotBindings(it.key()).value(i) : settings_.globalShortcutSlots(it.key()).value(i);
            chip->setCommittedValue(actual.toLower()); chip->setEnabled(supported && ready && !shortcut_->isSaving());
            chip->setToolTip(!supported ? QStringLiteral("功能尚未接入，不注册快捷键。") : QStringLiteral("先点 × 清除，再点击空框录入；Esc 撤销。桌面确认后保存。"));
            status->setState(supported && shortcut_ && shortcut_->slotConfirmed(it.key(),i) ? "ok" : "idle",
                supported && shortcut_ ? shortcut_->bindingText(it.key()) : QStringLiteral("尚未接入"));
        }
    }
}
