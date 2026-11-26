#include "settings_window.h"
#include "autostart.h"
#include "pin_window.h"
#include "linux_settings.h"
#include "global_shortcut.h"
#include "shortcut_bindings.h"
#include "shortcuts_page.h"
#include "core/theme.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include <QAction>
#include "core/ui_theme.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include <QComboBox>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QDir>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <initializer_list>

namespace {
QString settingsText(const char *source) {
    return jietuba::i18n_tr(source, "SettingsDialog");
}
// 沿用原版设置窗口的分组卡片，让 Linux 页面的结构和视觉保持一致。
// Reuse the original grouped cards and navigation rather than a separate visual system.
QVBoxLayout *pageLayout(QStackedWidget *stack, const QString &title) {
    auto *scroll = new QScrollArea(stack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(32, 24, 32, 24); layout->setSpacing(18);
    auto *heading = new QLabel(title, content);
    heading->setStyleSheet(QStringLiteral("font-size: 25px; font-weight: 600;"));
    layout->addWidget(heading);
    scroll->setWidget(content); stack->addWidget(scroll);
    return layout;
}
jietuba::SettingCard *card(jietuba::SettingCardGroup *group, jietuba::FluentIcon icon, const QString &title, const QString &description) {
    auto *result = new jietuba::SettingCard(icon, title, description, group);
    group->addSettingCard(result);
    return result;
}

struct PlannedSetting {
    jietuba::FluentIcon icon;
    const char *title;
    const char *description;
};

// Windows 原版的分组和名称仍然可见；尚未接线的控件只显示状态，不给用户一个无效开关。
// Keep original sections visible, but show an honest status instead of a switch that does nothing.
void pendingGroup(QVBoxLayout *layout, const QString &title, std::initializer_list<PlannedSetting> rows) {
    auto *group = new jietuba::SettingCardGroup(title, layout->parentWidget());
    for (const auto &item : rows) {
        auto *row = card(group, item.icon, QString::fromUtf8(item.title), QString::fromUtf8(item.description));
        auto *status = new QLabel(QStringLiteral("尚未接入"), row);
        status->setObjectName(QStringLiteral("pendingSettingStatus"));
        status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addControl(status, Qt::AlignRight);
    }
    layout->addWidget(group);
}

void fixedSetting(jietuba::SettingCardGroup *group, jietuba::FluentIcon icon, const QString &title,
                  const QString &description, const QString &value) {
    auto *row = card(group, icon, title, description);
    auto *status = new QLabel(value, row);
    status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    row->addControl(status, Qt::AlignRight);
}
}
LinuxSettingsWindow::LinuxSettingsWindow(LinuxSettings &settings, QWidget *parent) : QWidget(parent), settings_(settings) {
    setWindowTitle(settingsText("Settings") + QStringLiteral(" — jietuba"));
    resize(1050, 750); setMinimumSize(600, 400);
    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    buildPages();
}
void LinuxSettingsWindow::buildPages() {
    content_ = new QWidget(this);
    rootLayout_->addWidget(content_);
    auto *outer = new QHBoxLayout(content_);
    outer->setContentsMargins(16, 16, 16, 16); outer->setSpacing(12);
    auto *sidebar = new QWidget(content_);
    sidebar->setFixedWidth(216); sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    auto *brand = new QWidget(sidebar);
    auto *brandLayout = new QHBoxLayout(brand);
    auto *logo = new QLabel(brand);
    logo->setPixmap(jietuba::ResourceManager::get_icon(jietuba::ResourceManager::get_icon_path(QStringLiteral("托盘.svg")), 26).pixmap(26, 26));
    brandLayout->addWidget(logo);
    auto *brandName=new QLabel(QStringLiteral("jietuba"),brand); QFont brandFont=brandName->font(); brandFont.setPixelSize(16); brandFont.setWeight(QFont::DemiBold); brandName->setFont(brandFont);
    brandLayout->addWidget(brandName,1); brandLayout->setContentsMargins(10,8,10,16);
    sidebarLayout->addWidget(brand);
    auto *search = new jietuba::LineEdit(sidebar);
    search->setPlaceholderText(settingsText("Search settings")); search->setToolTip(QStringLiteral("设置搜索尚未接入"));
    search->setEnabled(false);
    sidebarLayout->addWidget(search);
    auto *navigation = new jietuba::NavigationInterface(sidebar);
    navigation_ = navigation;
    sidebarLayout->addWidget(navigation, 1);
    pages_ = new QStackedWidget(content_);
    outer->addWidget(sidebar); outer->addWidget(pages_, 1);
    auto addPage = [this, navigation](const QString &route, const QString &title, const jietuba::IconSource &icon, bool bottom = false) {
        if(route=="capture" || route=="appearance") navigation->addSeparator();
        const int index = pages_->count();
        auto *layout = pageLayout(pages_, title);
        const QMap<QString,QString> tones{{"capture","capture"},{"pin","pin"},{"annotation","capture"},{"clipboard","clipboard"},{"translation","translate"}};
        navigation->addItem(route, icon, title, [this, index] { pages_->setCurrentIndex(index); },
                            bottom ? jietuba::NavigationItemPosition::BOTTOM : jietuba::NavigationItemPosition::TOP,{},tones.value(route));
        return layout;
    };
    shortcutsPageIndex_=pages_->count();
    auto *shortcuts = addPage(QStringLiteral("shortcuts"), settingsText("Shortcut Settings"), jietuba::ResourceManager::get_icon_path(QStringLiteral("热键.svg")));
    shortcutsPage_=new LinuxShortcutsPage(settings_,shortcuts->parentWidget());
    // 原版把重置按钮放在页标题右侧；它只重置本地绑定，全局键仍由桌面授权管理。
    // Match the title-row reset placement; local reset never silently overwrites desktop bindings.
    auto *heading=shortcuts->takeAt(0); auto *header=new QWidget(shortcuts->parentWidget()); auto *headerRow=new QHBoxLayout(header);
    headerRow->setContentsMargins(0,0,0,0); headerRow->addWidget(heading->widget(),1); delete heading;
    auto *reset=shortcutsPage_->findChild<QPushButton *>("resetLocalShortcuts"); shortcutsPage_->layout()->removeWidget(reset); headerRow->addWidget(reset);
    shortcuts->addWidget(header);
    shortcuts->addWidget(shortcutsPage_); shortcuts->addStretch();
    auto *mouse = addPage(QStringLiteral("mouse"), settingsText("Mouse Shortcuts"), jietuba::ResourceManager::get_icon_path(QStringLiteral("鼠标.svg")));
    auto *mouseCurrent = new jietuba::SettingCardGroup(QStringLiteral("当前固定手势"), mouse->parentWidget());
    fixedSetting(mouseCurrent, jietuba::FluentIcon::Pin, QStringLiteral("贴图移动"), QStringLiteral("在贴图上按住左键拖动。"), QStringLiteral("左键拖动"));
    fixedSetting(mouseCurrent, jietuba::FluentIcon::Layout, QStringLiteral("贴图缩放"), QStringLiteral("按光标所在区域固定相应的缩放锚点。"), QStringLiteral("滚轮"));
    mouse->addWidget(mouseCurrent);
    pendingGroup(mouse, QStringLiteral("全局鼠标快捷键"), {
        {jietuba::FluentIcon::Camera, "区域截图", "在其他程序中用鼠标手势直接启动截图。"},
        {jietuba::FluentIcon::Paste, "贴图剪贴板", "用鼠标手势贴出当前剪贴板内容。"},
    });
    pendingGroup(mouse, QStringLiteral("应用内鼠标快捷键"), {
        {jietuba::FluentIcon::Brush, "截图画布手势", "可配置标注、取色和关闭动作。"},
        {jietuba::FluentIcon::Pin, "贴图手势", "可配置缩放、透明度和关闭动作。"},
    });
    mouse->addStretch();
    capturePageIndex_=pages_->count();
    auto *capture = addPage(QStringLiteral("capture"), settingsText("Screenshot"), jietuba::FluentIcon::Camera);
    auto *captureGroup = new jietuba::SettingCardGroup(settingsText("Screenshot"), capture->parentWidget(), QStringLiteral("capture"));
    fixedSetting(captureGroup, jietuba::FluentIcon::Camera, settingsText("Capture Engine"),
        QStringLiteral("优先 KWin、ext-image-copy-capture、wlr-screencopy；不可用时通过门户授权共享屏幕。"), QStringLiteral("自动选择"));
    auto *saveGroup = new jietuba::SettingCardGroup(QStringLiteral("保存设置"), capture->parentWidget(), QStringLiteral("capture"));
    auto *directoryCard = card(saveGroup, jietuba::FluentIcon::Folder, settingsText("Save Folder"), QStringLiteral("快速保存和另存为图片的默认目录。"));
    auto *directoryButton = new jietuba::PushButton(QStringLiteral("选择目录"), directoryCard);
    directoryButton->setObjectName(QStringLiteral("saveDirectoryButton")); directoryButton->setToolTip(settings_.saveDirectory());
    directoryCard->setContent(settings_.saveDirectory()); directoryCard->addControl(directoryButton);
    connect(directoryButton, &QPushButton::clicked, this, [this, directoryCard, directoryButton] {
        const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择截图保存目录"), settings_.saveDirectory());
        if (path.isEmpty()) return;
        settings_.setSaveDirectory(path); directoryCard->setContent(settings_.saveDirectory()); directoryButton->setToolTip(settings_.saveDirectory()); persist();
    });
    auto *formatCard = card(saveGroup, jietuba::FluentIcon::Save, settingsText("Save Format"), QStringLiteral("快速保存和保存对话框默认使用的图片格式。"));
    auto *format = new jietuba::ComboBox(formatCard); format->setObjectName(QStringLiteral("imageFormat"));
    format->addItem(QStringLiteral("PNG"), QStringLiteral("png")); format->addItem(QStringLiteral("JPEG"), QStringLiteral("jpg")); format->addItem(QStringLiteral("BMP"), QStringLiteral("bmp"));
    format->setCurrentIndex(format->findData(settings_.imageFormat())); formatCard->addControl(format);
    connect(format, &QComboBox::currentIndexChanged, this, [this, format] { settings_.setImageFormat(format->currentData().toString()); persist(); });
    auto *screenCard = card(captureGroup, jietuba::FluentIcon::Layout, QStringLiteral("截图屏幕"), QStringLiteral("选择所有屏幕或仅截取主屏幕。"));
    auto *screens = new jietuba::ComboBox(screenCard); screens->setObjectName(QStringLiteral("captureScreens"));
    screens->addItem(QStringLiteral("所有屏幕"), true); screens->addItem(QStringLiteral("仅主屏幕"), false);
    screens->setCurrentIndex(settings_.captureAllScreens() ? 0 : 1); screenCard->addControl(screens);
    connect(screens, &QComboBox::currentIndexChanged, this, [this, screens] { settings_.setCaptureAllScreens(screens->currentData().toBool()); persist(); });
    capture->addWidget(captureGroup);
    capture->addWidget(saveGroup);
    using F = LinuxSettings::CaptureFlag;
    using I = jietuba::FluentIcon;
    auto group = [&](const QString &name) { auto *g=new jietuba::SettingCardGroup(name,capture->parentWidget(),"capture"); capture->addWidget(g); return g; };
    auto toggle = [this](auto *g, I icon,const QString &title,const QString &hint,F flag) {
        auto *sw=new jietuba::SwitchSettingCard(icon,title,hint,g); sw->setChecked(settings_.captureFlag(flag)); g->addSettingCard(sw);
        connect(sw,&jietuba::SwitchSettingCard::checkedChanged,this,[this,flag](bool on){ settings_.setCaptureFlag(flag,on); persist(); });
    };
    auto choice = [this](auto *g,I icon,const QString &title,const QString &hint,const QList<QPair<QString,QString>> &entries,
                         const QString &current,void (LinuxSettings::*setter)(const QString &)) {
        auto *row=card(g,icon,title,hint); auto *box=new jietuba::ComboBox(row);
        for(const auto &e:entries) box->addItem(e.first,e.second);
        box->setCurrentIndex(box->findData(current)); row->addControl(box);
        connect(box,&QComboBox::currentIndexChanged,this,[this,box,setter]{ (settings_.*setter)(box->currentData().toString()); persist(); });
    };
    auto *assist=group(QStringLiteral("截图辅助"));
    toggle(assist,I::Camera,"包含鼠标指针","请求采集后端把鼠标指针绘入截图。",F::Cursor);
    toggle(assist,I::Layout,"全屏十字线","框选时显示横向与纵向定位线。",F::Crosshair);
    auto *automatic=group(QStringLiteral("自动保存"));
    toggle(automatic,I::Save,"截图完成时自动保存","确认复制时，另将图片写入保存文件夹。",F::AutoSave);
    toggle(automatic,I::Pin,"创建贴图时自动保存","创建贴图时将图片写入保存文件夹。",F::SavePin);
    auto *names=card(automatic,I::Document,"文件名设置",settings_.captureNamePattern());
    auto *editNames=new jietuba::PushButton("配置",names); names->addControl(editNames);
    connect(editNames,&QPushButton::clicked,this,[this,names]{
        bool ok=false; const auto rule=QInputDialog::getText(this,"截图文件名","{time} 表示日期和时间，{ms} 表示毫秒；重名自动加序号。",QLineEdit::Normal,settings_.captureNamePattern(),&ok);
        if (!ok) return;
        if (!settings_.setCaptureNamePattern(rule)) { QMessageBox::warning(this,"文件名无效","请使用文件名而非路径，不含扩展名；可使用 {time} 和 {ms}。"); return; }
        names->setContent(rule); persist();
    });
    toggle(automatic,I::Document,"另存未标注原图","保存有标注或水印的截图时，另存不含标注的 original 副本。",F::SaveOriginal);
    toggle(automatic,I::CommandPrompt,"将文件路径写入剪贴板","确认复制并成功自动保存后，同时提供图片和本地文件链接。",F::FileUrl);
    auto *quick = new jietuba::SettingCardGroup(QStringLiteral("快捷操作"), capture->parentWidget(), "capture");
    auto *cross = new jietuba::SwitchSettingCard(jietuba::FluentIcon::Edit, QStringLiteral("Ctrl 跨工具选择"),
        QStringLiteral("按住 Ctrl 点击其他工具绘制的标注，即可选中并调整。"), quick);
    cross->setObjectName("crossToolSelection"); cross->setChecked(settings_.crossToolSelection()); quick->addSettingCard(cross);
    connect(cross, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool on) { settings_.setCrossToolSelection(on); persist(); });
    auto *angleRow = card(quick, jietuba::FluentIcon::Brush, QStringLiteral("Shift 直线角度"),
        QStringLiteral("画笔或荧光笔绘制途中按住 Shift 画直线，松开后继续自由绘制。"));
    auto *angle = new jietuba::ComboBox(angleRow); angle->setObjectName("shiftLineAngle");
    for (int n : {90,45,30,15,0}) angle->addItem(n==90 ? QStringLiteral("水平或垂直") : n==0 ? QStringLiteral("任意角度") : QStringLiteral("每 %1°").arg(n), n);
    angle->setCurrentIndex(angle->findData(settings_.shiftLineAngle())); angleRow->addControl(angle);
    connect(angle, &QComboBox::currentIndexChanged, this, [this, angle] { settings_.setShiftLineAngle(angle->currentData().toInt()); persist(); });
    auto *onTop = new jietuba::SwitchSettingCard(jietuba::FluentIcon::Font, QStringLiteral("文字标注始终置顶"),
        QStringLiteral("新建的文字显示在普通标注上方，包括后来绘制的图形。"), quick);
    onTop->setObjectName("textAlwaysOnTop"); onTop->setChecked(settings_.textAlwaysOnTop()); quick->addSettingCard(onTop);
    connect(onTop, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool on) { settings_.setTextAlwaysOnTop(on); persist(); });
    capture->addWidget(quick);
    auto *snap=group("智能选区");
    choice(snap,I::Camera,"检测精度","单击接受吸附区域，拖动自由框选。无法检测时仍可手动框选。",{{"关闭","off"},{"窗口","window"},{"控件","element"}},settings_.smartSelectionMode(),&LinuxSettings::setSmartSelectionMode);
    toggle(snap,I::Sync,"选区切换动画","吸附目标变化时平滑移动选框。",F::SnapAnimation);
    auto *magnifierGroup=new jietuba::SettingCardGroup(QStringLiteral("放大镜与取色"),capture->parentWidget());
    for (const auto &entry:QList<QPair<QString,QString>>{{"enabled","显示放大镜"},{"grid","显示像素网格"},{"selection","显示选区边缘"},{"hints","显示按键提示"}}) {
        auto *toggle=new jietuba::SwitchSettingCard(jietuba::FluentIcon::Search,entry.second,
            QStringLiteral("框选时显示原始像素；Shift 切换颜色格式，C 复制颜色，方向键微调。"),magnifierGroup);
        toggle->setObjectName(QStringLiteral("magnifier_")+entry.first); toggle->setChecked(settings_.magnifierOption(entry.first));
        magnifierGroup->addSettingCard(toggle);
        connect(toggle,&jietuba::SwitchSettingCard::checkedChanged,this,[this,name=entry.first](bool value) { settings_.setMagnifierOption(name,value); persist(); });
    }
    auto *zoomRow=card(magnifierGroup,jietuba::FluentIcon::Search,QStringLiteral("放大倍数"),QStringLiteral("原版默认 4 倍，范围 2–10 倍；滚轮或 PageUp / PageDown 每次调整 0.25 倍并记忆。"));
    auto *zoom=new jietuba::DoubleSpinBox(zoomRow); zoom->setObjectName(QStringLiteral("magnifierZoom"));
    zoom->setRange(2,10); zoom->setSingleStep(.25); zoom->setValue(settings_.magnifierZoom()); zoomRow->addControl(zoom);
    connect(zoom,&QDoubleSpinBox::valueChanged,this,[this](double value) { settings_.setMagnifierZoom(value); persist(); });
    // 保留至少一个格式；取色复制使用正在显示的格式，而非另外一套隐含默认值。
    // Keep at least one format; copying uses the displayed format, with no separate hidden default.
    QMap<QString,QCheckBox *> colorFormats;
    for (const QString name:{"RGB","HEX","HEX without #","CSS rgb()","CSS hsl()"}) {
        auto *row=card(magnifierGroup,jietuba::FluentIcon::Palette,name,QStringLiteral("启用后可在截图时按 Shift 循环切换。"));
        auto *check=new QCheckBox(row); check->setObjectName(QStringLiteral("magnifierFormat_")+name);
        check->setChecked(settings_.magnifierFormats().contains(name)); row->addControl(check); colorFormats[name]=check;
    }
    // 回调持有 QMap 副本；使用只读迭代器，防止隐式共享分离使循环迭代器失效。
    // Callbacks retain a QMap copy; const iterators prevent copy-on-write detachment invalidating this loop.
    for (auto it=colorFormats.cbegin();it!=colorFormats.cend();++it) connect(it.value(),&QCheckBox::toggled,this,[this,colorFormats,check=it.value()](bool enabled) {
        QStringList selected;
        for (auto item=colorFormats.begin();item!=colorFormats.end();++item) if (item.value()->isChecked()) selected.append(item.key());
        if (selected.isEmpty() && !enabled) { const QSignalBlocker blocker(check); check->setChecked(true); return; }
        settings_.setMagnifierFormats(selected); persist();
    });
    capture->addWidget(magnifierGroup);
    auto *ocrGroup = new jietuba::SettingCardGroup(QStringLiteral("OCR 设置"), capture->parentWidget(), QStringLiteral("capture"));
    fixedSetting(ocrGroup, jietuba::FluentIcon::Font, settingsText("OCR Engine"),
                 QStringLiteral("使用系统 Tesseract 和已安装的语言数据。"), QStringLiteral("Tesseract"));
    capture->addWidget(ocrGroup);
    auto *recognition=group("识别结果");
    choice(recognition,I::Layout,"OCR 结果窗口位置","支持定位的桌面按此位置打开结果。",{{"选区旁","region"},{"光标旁","cursor"},{"屏幕中央","center"}},settings_.ocrPosition(),&LinuxSettings::setOcrPosition);
    toggle(recognition,I::Document,"直接复制识别文字","有识别结果时直接复制文字，不打开窗口。",F::CopyOcr);
    toggle(recognition,I::Search,"单个二维码直接复制","识别到单个码时复制内容；多码仍打开列表。",F::CopyCode);
    auto *scrollGroup = new jietuba::SettingCardGroup(settingsText("Long Screenshot"), capture->parentWidget(), QStringLiteral("capture"));
    fixedSetting(scrollGroup, jietuba::FluentIcon::Camera, settingsText("Long Screenshot"),
                 QStringLiteral("框选后手动向下滚动，程序自动采集并拼接。"), QStringLiteral("工具栏启动"));
    capture->addWidget(scrollGroup);
    // GIF 与 MP4 共用选区和标注；各自的帧率、视频声音/码率在录制条保存。
    // GIF and MP4 share capture/annotations while keeping their format-specific preferences separate.
    auto *recordGroup = new jietuba::SettingCardGroup(settingsText("Screen Recording"), capture->parentWidget());
    fixedSetting(recordGroup, jietuba::FluentIcon::Camera, QStringLiteral("GIF 录制"),
        QStringLiteral("框选后点 GIF；停止后显示静止首帧和工具栏，复制或打开文件夹后关闭。最长 60 秒。"), QStringLiteral("已接入"));
    auto *fpsRow = card(recordGroup, jietuba::FluentIcon::Camera, QStringLiteral("GIF 默认帧率"), QStringLiteral("实际帧率取决于桌面采集速度；导出保持真实时长。"));
    auto *gifFps = new QComboBox(fpsRow);
    for (int value : {5,10,16,24,30}) gifFps->addItem(QStringLiteral("%1 fps").arg(value), value);
    gifFps->setCurrentIndex(gifFps->findData(settings_.gifFps())); fpsRow->addControl(gifFps);
    connect(gifFps, &QComboBox::currentIndexChanged, this, [this,gifFps] { settings_.setGifFps(gifFps->currentData().toInt()); persist(); });
    auto *delayRow = card(recordGroup, jietuba::FluentIcon::Camera, QStringLiteral("录制倒计时"), QStringLiteral("点击开始后等待指定时间，再采集真实桌面。"));
    auto *gifDelay = new QComboBox(delayRow);
    for (int value : {0,1,2,3,5,10}) gifDelay->addItem(value ? QStringLiteral("%1 秒").arg(value) : QStringLiteral("无延迟"), value);
    gifDelay->setCurrentIndex(gifDelay->findData(settings_.gifDelay())); delayRow->addControl(gifDelay);
    connect(gifDelay, &QComboBox::currentIndexChanged, this, [this,gifDelay] { settings_.setGifDelay(gifDelay->currentData().toInt()); persist(); });
    fixedSetting(recordGroup, jietuba::FluentIcon::Hide, QStringLiteral("录制控制栏"),
        QStringLiteral("优先显示在选区外；可从工具栏或托盘暂停、停止。录制中仍可按截图快捷键框选。"), QStringLiteral("自动避让"));
    fixedSetting(recordGroup, jietuba::FluentIcon::Camera, QStringLiteral("MP4 录制"),
        QStringLiteral("录制条切换 MP4；支持系统声音、鼠标指针、视频设置和标注，停止后自动保存。最长 60 秒。"), QStringLiteral("已接入"));
    capture->addWidget(recordGroup);
    auto *watermarkGroup = new jietuba::SettingCardGroup(QStringLiteral("水印"), capture->parentWidget(), QStringLiteral("capture"));
    fixedSetting(watermarkGroup, jietuba::FluentIcon::Edit, QStringLiteral("水印文字与样式"),
                 QStringLiteral("在截图工具栏的水印面板中编辑，并保存到 Linux 设置。"), QStringLiteral("工具栏编辑"));
    capture->addWidget(watermarkGroup);
    toggle(group("水印自动化"),I::Sync,"自动添加上次水印","新截图使用工具栏中设置的水印文字和样式，时间按本次截图更新。",F::AutoWatermark);
    auto *captureButton = new jietuba::PrimaryPushButton(QStringLiteral("开始截图")); captureButton->setObjectName(QStringLiteral("startCapture"));
    connect(captureButton, &QPushButton::clicked, this, &LinuxSettingsWindow::captureRequested);
    capture->addWidget(captureButton, 0, Qt::AlignLeft); capture->addStretch();
    auto *pin = addPage(QStringLiteral("pin"), settingsText("Pin"), jietuba::FluentIcon::Pin);
    auto *pinBehavior = new jietuba::SettingCardGroup(QStringLiteral("行为"), pin->parentWidget(), QStringLiteral("pin"));
    fixedSetting(pinBehavior, jietuba::FluentIcon::Pin, QStringLiteral("贴图操作"),
                 QStringLiteral("可拖动、滚轮缩放、复制、保存，并通过右键菜单关闭。"), QStringLiteral("已启用"));
    pin->addWidget(pinBehavior);
    auto pinGroup=[&](const QString &title){ auto *g=new jietuba::SettingCardGroup(title,pin->parentWidget(),"pin"); pin->addWidget(g); return g; };
    auto pinSwitch=[this](auto *g,I icon,const QString &key,const QString &title,const QString &hint){
        auto *sw=new jietuba::SwitchSettingCard(icon,title,hint,g); sw->setObjectName("pin_"+key); sw->setChecked(settings_.pinFlag(key)); g->addSettingCard(sw);
        connect(sw,&jietuba::SwitchSettingCard::checkedChanged,this,[this,key](bool on){ settings_.setPinFlag(key,on); persist(); if(key=="rounded_corners") LinuxPinWindow::refreshOpenAppearance(settings_); });
        return sw;
    };
    auto pinPercent=[this](auto *g,I icon,const QString &key,const QString &title,const QString &hint,QList<int> steps){
        auto *row=card(g,icon,title,hint); auto *box=new jietuba::ComboBox(row); box->setObjectName("pin_"+key);
        for(int v:steps) box->addItem(QString::number(v)+"%",v);
        box->setCurrentIndex(box->findData(settings_.pinPercent(key))); row->addControl(box);
        connect(box,&QComboBox::currentIndexChanged,this,[this,box,key]{settings_.setPinPercent(key,box->currentData().toInt());persist();});
    };
    auto *behavior=pinGroup("行为扩展");
    auto *ocrSwitch=pinSwitch(behavior,I::Font,"auto_ocr","贴图后自动识别文字","后台识别图片，可从贴图右键菜单复制识别文字。");
#ifndef JIETUBA_HAS_TESSERACT
    ocrSwitch->setEnabled(false); ocrSwitch->setToolTip("此构建未启用 Tesseract");
#else
    Q_UNUSED(ocrSwitch);
#endif
    pinPercent(behavior,I::Transparent,"default_opacity","默认透明度","新贴图的不透明度。",{100,90,80,70,60,50,40,30});
    pinSwitch(behavior,I::Paste,"text_formatting","保留文字格式","从剪贴板贴出文字时保留段落、列表和字体样式。");
    pinSwitch(behavior,I::Pin,"point_out_repeats","提示重复贴图","重复贴出相同剪贴板内容时提示已有贴图，截图钉图总是新建。");
    auto *pinZoom = new jietuba::SettingCardGroup(QStringLiteral("缩放与大小"), pin->parentWidget(), QStringLiteral("pin"));
    auto *zoomCard = card(pinZoom, jietuba::FluentIcon::Search, QStringLiteral("滚轮缩放步长"), QStringLiteral("每格滚轮相对原图大小的变化百分比。"));
    auto *zoomCombo = new jietuba::ComboBox(zoomCard); zoomCombo->setObjectName(QStringLiteral("pinZoomStep"));
    for (int step : {5, 10, 15, 20, 25, 50}) zoomCombo->addItem(QStringLiteral("%1%").arg(step), step);
    zoomCombo->setCurrentIndex(zoomCombo->findData(settings_.pinZoomStep())); zoomCard->addControl(zoomCombo);
    connect(zoomCombo, &QComboBox::currentIndexChanged, this, [this, zoomCombo] {
        settings_.setPinZoomStep(zoomCombo->currentData().toInt()); persist();
    });
    choice(pinZoom,I::Layout,"缩放锚点","缩放时保持不动的位置。",{{"中央＋四角","adaptive"},{"左上","top_left"},{"右上","top_right"},{"左下","bottom_left"},{"右下","bottom_right"},{"中心","center"},{"光标","mouse"}},settings_.pinZoomAnchor(),&LinuxSettings::setPinZoomAnchor);
    pin->addWidget(pinZoom);
    auto *zoomExtra=pinGroup("缩放扩展");
    pinPercent(zoomExtra,I::Search,"fine_zoom_step","精细缩放步长","Alt＋滚轮精细缩放。",{1,2,3,5});
    pinPercent(zoomExtra,I::Transparent,"opacity_step","透明度步长","Ctrl＋滚轮调整当前贴图的不透明度。",{1,2,5,10});
    pinSwitch(zoomExtra,I::Layout,"border_resize","拖动边缘调整大小","拖动边或角等比调整大小。");
    pinSwitch(zoomExtra,I::FontSize,"auto_shrink","自动缩小超大图片","新贴图超过可用屏幕时等比缩小并居中。");
    auto *pinAppearance = new jietuba::SettingCardGroup(settingsText("Appearance"), pin->parentWidget(), QStringLiteral("pin"));
    auto *autoBorder = new jietuba::SwitchSettingCard(jietuba::FluentIcon::Palette, QStringLiteral("自动边框"),
                                                       QStringLiteral("新贴图默认显示浅蓝色边框；打开中的贴图仍可从右键菜单切换。"), pinAppearance);
    autoBorder->setObjectName(QStringLiteral("pinAutoBorder"));
    autoBorder->setChecked(settings_.pinBorderEnabled());
    pinAppearance->addSettingCard(autoBorder);
    connect(autoBorder, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool value) {
        settings_.setPinBorderEnabled(value); persist();
    });
    auto *hoverButtons = new jietuba::SwitchSettingCard(jietuba::FluentIcon::Close, QStringLiteral("悬停按钮"),
        QStringLiteral("移入贴图只显示右上角按钮；点击工具栏按钮展开或收起标注工具栏。"), pinAppearance);
    hoverButtons->setObjectName(QStringLiteral("pinHoverButtons"));
    hoverButtons->setChecked(settings_.pinHoverButtons());
    pinAppearance->addSettingCard(hoverButtons);
    connect(hoverButtons, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool value) {
        settings_.setPinHoverButtons(value); persist(); LinuxPinWindow::refreshOpenAppearance(settings_);
    });
    pin->addWidget(pinAppearance);
    auto *pinExtraAppearance=pinGroup("外观扩展");
    pinSwitch(pinExtraAppearance,I::Layout,"rounded_corners","圆角与阴影","仅改变显示效果，复制或保存仍使用完整原图。");
    pinPercent(pinExtraAppearance,I::Transparent,"click_through_opacity","鼠标穿透透明度","从右键菜单开启穿透后使用的不透明度。",{90,80,70,60,50,40,30,20});
    auto *lock=pinGroup("锁定");
    pinSwitch(lock,I::Pin,"lock_block_move","锁定时禁止移动与缩放","阻止拖动、滚轮缩放和边缘拉伸。");
    pinSwitch(lock,I::Close,"lock_block_click_close","锁定时禁止鼠标关闭","锁定后右上角关闭按钮无效；右键菜单仍可明确关闭。");
    pinSwitch(lock,I::CommandPrompt,"lock_block_key_close","锁定时禁止键盘关闭","锁定后 Esc 不关闭贴图。");
    pinSwitch(lock,I::Info,"lock_hint","显示锁定提示","锁定、解锁及阻止操作时显示简短提示。");
    auto *clipboardPins = new jietuba::SettingCardGroup(QStringLiteral("从剪贴板贴图"), pin->parentWidget());
    fixedSetting(clipboardPins, jietuba::FluentIcon::Pin, QStringLiteral("贴图剪贴板内容"),
                 QStringLiteral("在全局快捷键中分配按键，可贴出图片、文字或文件路径列表。"), QStringLiteral("已接入"));
    pin->addWidget(clipboardPins);
    pin->addStretch();
    auto *annotation = addPage(QStringLiteral("annotation"), settingsText("Screen Annotation"), jietuba::FluentIcon::Brush);
    auto *annotationGroup=new jietuba::SettingCardGroup(settingsText("Screen Annotation"),annotation->parentWidget(),QStringLiteral("capture"));
    fixedSetting(annotationGroup,jietuba::FluentIcon::Brush,QStringLiteral("独立屏幕标注"),QStringLiteral("Alt+4 或托盘启动；工具栏在当前屏幕底部居中，绘图后按 Esc 恢复鼠标穿透。"),QStringLiteral("已接入"));
    fixedSetting(annotationGroup,jietuba::FluentIcon::Layout,QStringLiteral("工具栏按钮"),QStringLiteral("复用原版工具、白板、撤销、清空与隐藏；通过工具栏的“更多 → 调整”修改布局。"),QStringLiteral("工具栏编辑"));
    annotation->addWidget(annotationGroup);
    pendingGroup(annotation, QStringLiteral("标注扩展设置"), {
        {jietuba::FluentIcon::Brush, "保留屏幕标注", "退出截图选区后仍保留画笔标注图层。"},
        {jietuba::FluentIcon::Camera, "截图时隐藏工具栏", "在录制或截屏结果中排除画笔工具栏。"},
    });
    pendingGroup(annotation, QStringLiteral("鼠标效果"), {
        {jietuba::FluentIcon::Palette, "开始时高亮光标", "启动屏幕标注时显示光标高亮。"},
    });
    auto *pointerEffects = new jietuba::SettingCardGroup(QStringLiteral("光标效果支持"), annotation->parentWidget());
    fixedSetting(pointerEffects, jietuba::FluentIcon::Brush, QStringLiteral("点击波纹"),
                 QStringLiteral("开启工具栏的光标高亮后，标注层中的左键显示黄色波纹，右键显示蓝色波纹。KDE 下穿透模式支持光标位置跟踪；其它应用的点击暂不提供波纹。"), QStringLiteral("工具栏开启"));
    annotation->addWidget(pointerEffects);
    annotation->addStretch();
    auto *clipboard = addPage(QStringLiteral("clipboard"), settingsText("Clipboard"), jietuba::FluentIcon::Paste);
    pendingGroup(clipboard, QStringLiteral("基本设置"), {
        {jietuba::FluentIcon::Paste, "启用剪贴板管理", "监听并管理复制的图片、文字和文件历史。"},
        {jietuba::FluentIcon::Hide, "记录私密内容", "决定是否记录应用标记为私密的数据。"},
        {jietuba::FluentIcon::Layout, "窗口位置", "跟随鼠标或记住上一次位置。"},
        {jietuba::FluentIcon::Search, "重新打开时保留搜索", "记住上一次搜索内容。"},
    });
    pendingGroup(clipboard, QStringLiteral("数据管理"), {
        {jietuba::FluentIcon::Folder, "数据保存位置", "剪贴板历史数据库的存储目录。"},
        {jietuba::FluentIcon::Document, "单文件数据库", "将历史存入单一数据库文件。"},
        {jietuba::FluentIcon::History, "历史上限", "限制保留条数与磁盘占用。"},
        {jietuba::FluentIcon::Delete, "清空历史", "删除所有已保存的剪贴板内容。"},
    });
    pendingGroup(clipboard, QStringLiteral("列表与粘贴"), {
        {jietuba::FluentIcon::DateTime, "显示时间", "显示每条内容的复制时间。"},
        {jietuba::FluentIcon::Camera, "图片缩略图大小", "调节列表中的图片预览。"},
        {jietuba::FluentIcon::Paste, "选中后自动粘贴", "把选中内容发送给当前应用。"},
        {jietuba::FluentIcon::Close, "粘贴后关闭窗口", "连续粘贴多项时可保持窗口打开。"},
        {jietuba::FluentIcon::Alignment, "多选粘贴顺序与分隔符", "控制合并内容的排列和分隔方式。"},
    });
    clipboard->addStretch();
    translationPageIndex_ = pages_->count();
    auto *translation = addPage(QStringLiteral("translation"), settingsText("Translation"), jietuba::FluentIcon::Language);
    auto *translationGroup = new jietuba::SettingCardGroup(QStringLiteral("截图翻译"), translation->parentWidget());
    auto *providerCard = card(translationGroup, jietuba::FluentIcon::Language, settingsText("Translation Service"), QStringLiteral("选择 DeepL、Google、有道，或连接自建 LibreTranslate。"));
    auto *provider = new jietuba::ComboBox(providerCard); provider->setObjectName(QStringLiteral("translationProvider"));
    provider->addItem(QStringLiteral("DeepL API Free"), QStringLiteral("deepl_free"));
    provider->addItem(QStringLiteral("DeepL API Pro"), QStringLiteral("deepl_pro"));
    provider->addItem(QStringLiteral("LibreTranslate"), QStringLiteral("libretranslate"));
    provider->addItem(QStringLiteral("Google Cloud Translation"), QStringLiteral("google"));
    provider->addItem(QStringLiteral("有道智云"), QStringLiteral("youdao"));
    provider->setCurrentIndex(provider->findData(settings_.translationProvider())); providerCard->addControl(provider);
    auto *keyCard = card(translationGroup, jietuba::FluentIcon::Edit, QStringLiteral("DeepL / LibreTranslate 密钥"), QStringLiteral("自建 LibreTranslate 可留空。"));
    auto *key = new jietuba::LineEdit(keyCard); key->setObjectName(QStringLiteral("translationApiKey"));
    // SettingCard 的控制列最多 200px；330px 的固定宽度会裁掉输入框右侧和焦点边框。
    // The card caps its control column at 200px, so the line edit must grow within that column.
    key->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    key->setEchoMode(QLineEdit::Password); key->setText(settings_.translationApiKey()); keyCard->addControl(key);
    auto *googleCard = card(translationGroup, jietuba::FluentIcon::Edit, QStringLiteral("Google API Key"), QStringLiteral("使用 Google Cloud Translation Basic v2 的 API 密钥。"));
    auto *googleKey = new jietuba::LineEdit(googleCard); googleKey->setObjectName(QStringLiteral("googleApiKey"));
    googleKey->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    googleKey->setEchoMode(QLineEdit::Password); googleKey->setText(settings_.googleApiKey()); googleCard->addControl(googleKey);
    auto *youdaoIdCard = card(translationGroup, jietuba::FluentIcon::Edit, QStringLiteral("有道应用 ID"), QStringLiteral("在有道智云创建并绑定文本翻译服务的应用。"));
    auto *youdaoId = new jietuba::LineEdit(youdaoIdCard); youdaoId->setObjectName(QStringLiteral("youdaoAppId"));
    youdaoId->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    youdaoId->setText(settings_.youdaoAppId()); youdaoIdCard->addControl(youdaoId);
    auto *youdaoSecretCard = card(translationGroup, jietuba::FluentIcon::Edit, QStringLiteral("有道应用密钥"), QStringLiteral("与应用 ID 配对，用于生成 v3 签名。"));
    auto *youdaoSecret = new jietuba::LineEdit(youdaoSecretCard); youdaoSecret->setObjectName(QStringLiteral("youdaoAppSecret"));
    youdaoSecret->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    youdaoSecret->setEchoMode(QLineEdit::Password); youdaoSecret->setText(settings_.youdaoAppSecret()); youdaoSecretCard->addControl(youdaoSecret);
    auto *endpointCard = card(translationGroup, jietuba::FluentIcon::Application, QStringLiteral("LibreTranslate 地址"), QStringLiteral("填写服务的完整 /translate 地址，例如 http://localhost:5000/translate。"));
    auto *endpoint = new jietuba::LineEdit(endpointCard); endpoint->setObjectName(QStringLiteral("translationEndpoint"));
    endpoint->setMaximumWidth(jietuba::SettingCard::CONTROL_COLUMN_MAX);
    endpoint->setPlaceholderText(QStringLiteral("http://localhost:5000/translate"));
    endpoint->setToolTip(QStringLiteral("填写完整的 /translate 地址"));
    endpoint->setText(settings_.translationEndpoint()); endpointCard->addControl(endpoint);
    auto *targetCard = card(translationGroup, jietuba::FluentIcon::Language, QStringLiteral("默认目标语言"), QStringLiteral("截图翻译结果窗口也可以临时切换。"));
    auto *target = new jietuba::ComboBox(targetCard); target->setObjectName(QStringLiteral("translationTarget"));
    target->addItem(QStringLiteral("简体中文"), QStringLiteral("zh")); target->addItem(QStringLiteral("英语"), QStringLiteral("en"));
    target->addItem(QStringLiteral("日语"), QStringLiteral("ja")); target->addItem(QStringLiteral("韩语"), QStringLiteral("ko"));
    target->setCurrentIndex(target->findData(settings_.translationTarget())); targetCard->addControl(target);
    const auto updateFields = [this, provider, translationGroup, keyCard, googleCard, youdaoIdCard, youdaoSecretCard, endpointCard] {
        const QString selected = provider->currentData().toString();
        keyCard->setVisible(selected == QLatin1String("deepl_free") || selected == QLatin1String("deepl_pro") || selected == QLatin1String("libretranslate"));
        googleCard->setVisible(selected == QLatin1String("google"));
        youdaoIdCard->setVisible(selected == QLatin1String("youdao"));
        youdaoSecretCard->setVisible(selected == QLatin1String("youdao"));
        endpointCard->setVisible(selected == QLatin1String("libretranslate"));
        translationGroup->adjustSize();
    };
    connect(provider, &QComboBox::currentIndexChanged, this, [this, provider, updateFields] {
        settings_.setTranslationProvider(provider->currentData().toString());
        updateFields(); persist();
    });
    updateFields();
    connect(key, &QLineEdit::editingFinished, this, [this, key] { settings_.setTranslationApiKey(key->text()); persist(); });
    connect(googleKey, &QLineEdit::editingFinished, this, [this, googleKey] { settings_.setGoogleApiKey(googleKey->text()); persist(); });
    connect(youdaoId, &QLineEdit::editingFinished, this, [this, youdaoId] { settings_.setYoudaoAppId(youdaoId->text()); persist(); });
    connect(youdaoSecret, &QLineEdit::editingFinished, this, [this, youdaoSecret] { settings_.setYoudaoAppSecret(youdaoSecret->text()); persist(); });
    connect(endpoint, &QLineEdit::editingFinished, this, [this, endpoint] { settings_.setTranslationEndpoint(endpoint->text()); persist(); });
    connect(target, &QComboBox::currentIndexChanged, this, [this, target] { settings_.setTranslationTarget(target->currentData().toString()); persist(); });
    translation->addWidget(translationGroup);
    pendingGroup(translation, QStringLiteral("翻译选项"), {
        {jietuba::FluentIcon::Hide, "点击外部时关闭", "快捷键弹出的翻译窗失去焦点时自动关闭。"},
        {jietuba::FluentIcon::Alignment, "忽略换行", "把 OCR 的多行文字合并后翻译。"},
        {jietuba::FluentIcon::Document, "保留原文格式", "翻译时尽量保持段落与标点形式。"},
    });
    pendingGroup(translation, QStringLiteral("朗读"), {
        {jietuba::FluentIcon::Language, "原文与译文语音", "选择文本朗读语音。"},
        {jietuba::FluentIcon::StopWatch, "语速", "调整朗读速度。"},
        {jietuba::FluentIcon::FontSize, "音量", "调整朗读音量。"},
    });
    translation->addStretch();
    auto *appearance = addPage(QStringLiteral("appearance"), settingsText("Appearance"), jietuba::FluentIcon::Palette);
    auto *appearanceGroup = new jietuba::SettingCardGroup(QStringLiteral("应用外观"), appearance->parentWidget());
    auto *themeCard = card(appearanceGroup, jietuba::FluentIcon::Application, QStringLiteral("界面主题"), QStringLiteral("设置窗口和截图工具栏的显示主题。"));
    auto *theme = new jietuba::ComboBox(themeCard); theme->setObjectName(QStringLiteral("uiTheme"));
    theme->addItem(QStringLiteral("跟随系统"), QStringLiteral("system")); theme->addItem(settingsText("Light"), QStringLiteral("light")); theme->addItem(settingsText("Dark"), QStringLiteral("dark"));
    theme->setCurrentIndex(theme->findData(settings_.theme())); themeCard->addControl(theme);
    connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] {
        settings_.setTheme(theme->currentData().toString()); jietuba::get_ui_theme()->set_mode(settings_.theme(), false); persist();
    });
    appearance->addWidget(appearanceGroup);
    pendingGroup(appearance, QStringLiteral("应用主题扩展"), {
        {jietuba::FluentIcon::Palette, "主题色", "自定义截图选区和窗口强调色。"},
        {jietuba::FluentIcon::Brush, "选区外遮罩颜色", "自定义框选时选区外的遮罩颜色。"},
    });
    auto *captureAppearance = new jietuba::SettingCardGroup(QStringLiteral("截图外观"), appearance->parentWidget(), QStringLiteral("capture"));
    auto *borderCard = card(captureAppearance, jietuba::FluentIcon::Edit, QStringLiteral("选区边框宽度"), QStringLiteral("下一次截图时使用的边框宽度，单位为图片像素。"));
    auto *border = new jietuba::ComboBox(borderCard); border->setObjectName(QStringLiteral("borderWidth"));
    for (int width : jietuba::ThemeManager::BORDER_WIDTH_OPTIONS) border->addItem(QStringLiteral("%1 px").arg(width), width);
    const int widthIndex = border->findData(settings_.borderWidth()); border->setCurrentIndex(widthIndex < 0 ? border->findData(4) : widthIndex); borderCard->addControl(border);
    connect(border, &QComboBox::currentIndexChanged, this, [this, border] {
        settings_.setBorderWidth(border->currentData().toInt()); jietuba::get_theme().set_selection_border_width(settings_.borderWidth()); persist();
    });
    fixedSetting(captureAppearance,jietuba::FluentIcon::Palette,QStringLiteral("工具栏按钮布局"),
        QStringLiteral("截图工具栏的“更多 → 调整”中排序、设置显示模式或恢复默认；确认后自动保存。"),QStringLiteral("更多 → 调整"));
    appearance->addWidget(captureAppearance);
    pendingGroup(appearance, QStringLiteral("截图外观扩展"), {
        {jietuba::FluentIcon::Camera, "选区预览", "框选时显示所选内容的预览效果。"},
    });
    pendingGroup(appearance, QStringLiteral("剪贴板外观"), {
        {jietuba::FluentIcon::Paste, "剪贴板主题", "选择历史窗口的颜色和缩略图样式。"},
    });
    appearance->addStretch();
    auto *logs = addPage(QStringLiteral("logs"), settingsText("Log Settings"), jietuba::FluentIcon::History);
    auto *logCurrent = new jietuba::SettingCardGroup(QStringLiteral("当前日志"), logs->parentWidget());
    fixedSetting(logCurrent, jietuba::FluentIcon::History, QStringLiteral("日志输出"),
                 QStringLiteral("当前 Linux 版向启动它的终端输出运行信息。"), QStringLiteral("终端"));
    logs->addWidget(logCurrent);
    pendingGroup(logs, QStringLiteral("文件日志"), {
        {jietuba::FluentIcon::History, "保存日志", "把运行信息持续写入日志文件。"},
        {jietuba::FluentIcon::Filter, "日志级别", "选择调试、信息、警告等最小记录级别。"},
        {jietuba::FluentIcon::DateTime, "保留时间", "自动清理超过期限的旧日志。"},
        {jietuba::FluentIcon::Folder, "日志文件夹", "查看或修改日志存储位置。"},
    });
    logs->addStretch();
    otherPageIndex_ = pages_->count();
    auto *other = addPage(QStringLiteral("other"), settingsText("Other"), jietuba::FluentIcon::Setting);
    auto *startup = new jietuba::SettingCardGroup(settingsText("Startup"), other->parentWidget());
    autostart_ = new jietuba::SwitchSettingCard(jietuba::FluentIcon::PowerButton, settingsText("Launch on Startup"),
                                              QStringLiteral("XDG Autostart"), startup);
    autostart_->setObjectName("autostartToggle");
    autostart_->setChecked(LinuxAutostart::isEnabled());
    startup->addSettingCard(autostart_);
    connect(autostart_, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool checked) {
        const auto error = LinuxAutostart::setEnabled(checked);
        const QSignalBlocker blocker(autostart_);
        autostart_->setChecked(LinuxAutostart::isEnabled());
        if (!error.isEmpty()) QMessageBox::warning(this, settingsText("Launch on Startup"), error);
    });
    auto *showWindow = new jietuba::SwitchSettingCard(jietuba::FluentIcon::Application, settingsText("Show Main Window on Startup"),
                                                     settingsText("If off, starts in background."), startup);
    showWindow->setObjectName("showMainWindowToggle");
    showWindow->setChecked(settings_.showMainWindowOnStartup());
    startup->addSettingCard(showWindow);
    connect(showWindow, &jietuba::SwitchSettingCard::checkedChanged, this, [this](bool checked) {
        settings_.setShowMainWindowOnStartup(checked);
        persist();
    });
    other->addWidget(startup);

    auto *operation = new jietuba::SettingCardGroup(settingsText("Operation"), other->parentWidget());
    auto *languageCard = card(operation, jietuba::FluentIcon::Language, settingsText("Language"),
                              settingsText("Select display language. Restart required after change."));
    auto *language = new jietuba::ComboBox(languageCard);
    language->setObjectName("interfaceLanguage");
    for (const auto &[code, name] : jietuba::I18nManager::language_items()) language->addItem(name, code);
    language->setCurrentIndex(language->findData(settings_.language()));
    languageCard->addControl(language);
    connect(language, &QComboBox::currentIndexChanged, this, [this, language] {
        settings_.setLanguage(language->currentData().toString());
        persist();
    });
    other->addWidget(operation);

    auto *configuration = new jietuba::SettingCardGroup(settingsText("Settings Backup"), other->parentWidget());
    auto *configCard = card(configuration, jietuba::FluentIcon::Folder, settingsText("Settings File"),
                            settings_.fileName() + '\n' + settingsText("Copy this file to back up your settings or use them on another computer."));
    auto *openConfig = new jietuba::PushButton(settingsText("Open"), configCard);
    connect(openConfig, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(settings_.fileName()).absolutePath()));
        });
    });
    configCard->addControl(openConfig);
    auto *importCard = card(configuration, jietuba::FluentIcon::Download, settingsText("Import Settings"),
                           settingsText("Load settings from another settings.ini or a file exported by an earlier version. Folder locations stay unchanged."));
    auto *importButton = new jietuba::PushButton(settingsText("Import"), importCard);
    importButton->setObjectName("importSettingsButton");
    importCard->addControl(importButton);
    connect(importButton, &QPushButton::clicked, this, &LinuxSettingsWindow::importSettings);
    other->addWidget(configuration);
    other->addStretch();
    auto *about = addPage(QStringLiteral("about"), settingsText("About"), jietuba::FluentIcon::Info, true);
    auto *aboutGroup = new jietuba::SettingCardGroup(QStringLiteral("截图吧 Linux"), about->parentWidget());
    fixedSetting(aboutGroup, jietuba::FluentIcon::Info, QStringLiteral("平台"), QStringLiteral("当前仅支持 Wayland。"), QStringLiteral("Linux"));
    fixedSetting(aboutGroup, jietuba::FluentIcon::Application, QStringLiteral("界面来源"),
                 QStringLiteral("复用 Windows 版 Qt 工具栏和设置卡片，按模块迁移功能。"), QStringLiteral("Qt 6"));
    fixedSetting(aboutGroup, jietuba::FluentIcon::People, QStringLiteral("开发者"), QStringLiteral("原版作者与 Linux 移植项目。"), QStringLiteral("JYAARU"));
    fixedSetting(aboutGroup, jietuba::FluentIcon::GitHub, QStringLiteral("源代码"),
                 QStringLiteral("github.com/1003129155/jietuba"), QStringLiteral("开源项目"));
    fixedSetting(aboutGroup, jietuba::FluentIcon::Document, QStringLiteral("许可证"), QStringLiteral("项目与第三方组件以仓库声明为准。"), QStringLiteral("查看仓库"));
    about->addWidget(aboutGroup); about->addStretch();
    // 和原版一样从标识的右键菜单进入开发者页，保留常用导航的顺序。
    // Open developer options from the brand context menu, preserving the regular navigation order.
    auto *developer = pageLayout(pages_, settingsText("Developer Options"));
    auto *developerPage = pages_->widget(pages_->count() - 1);
    pendingGroup(developer, settingsText("Long Screenshot"), {
        {jietuba::FluentIcon::Setting, "拼接引擎", "选择用于长截图的拼接算法。"},
        {jietuba::FluentIcon::Filter, "忽略顶部像素", "匹配后续画面时跳过指定高度的顶部区域。"},
    });
    pendingGroup(developer, QStringLiteral("工具"), {
        {jietuba::FluentIcon::Setting, "设置向导", "重新运行首次设置向导。"},
    });
    pendingGroup(developer, QStringLiteral("启动预加载"), {
        {jietuba::FluentIcon::Camera, "预加载截图模块", "启动时加载截图引擎、画布和工具。"},
        {jietuba::FluentIcon::Layout, "预加载工具栏", "启动时创建工具栏控件。"},
        {jietuba::FluentIcon::Search, "预加载 OCR 引擎", "在后台加载文字识别引擎。"},
        {jietuba::FluentIcon::Setting, "预加载设置窗口", "启动时创建设置窗口。"},
        {jietuba::FluentIcon::Paste, "预加载剪贴板管理器", "启动时初始化剪贴板监听。"},
    });
    pendingGroup(developer, QStringLiteral("选区信息面板"), {
        {jietuba::FluentIcon::Hide, "拖动时隐藏面板", "拖动选区时隐藏选区信息。"},
    });
    developer->addStretch();
    brand->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto *openDeveloper = brand->addAction(settingsText("Developer Options"));
    openDeveloper->setObjectName("openDeveloperOptions");
    connect(openDeveloper, &QAction::triggered, this, [this, developerPage] {
        navigation_->clearCurrentItem();
        pages_->setCurrentWidget(developerPage);
    });
    navigation->setCurrentItem(QStringLiteral("capture"));
    pages_->setCurrentIndex(capturePageIndex_);
}
void LinuxSettingsWindow::showCapturePage() {
    // 截图设置专用入口明确进入截图页，不受上次浏览页或页面插入顺序影响。
    // The capture-specific entry opens Capture independently of the last page or page insertion order.
    pages_->setCurrentIndex(capturePageIndex_);
    navigation_->setCurrentItem(QStringLiteral("capture"));
    show(); raise(); activateWindow();
}
void LinuxSettingsWindow::showShortcutsPage() {
    // 托盘“设置”每次都定位到快捷键页；翻译、截图等专用入口仍独立指定目标页。
    // The tray settings entry always opens Shortcuts; explicit capture/translation entries keep their target pages.
    pages_->setCurrentIndex(shortcutsPageIndex_);
    navigation_->setCurrentItem(QStringLiteral("shortcuts"));
    show(); raise(); activateWindow();
}
void LinuxSettingsWindow::showTranslationPage() {
    if (translationPageIndex_ >= 0) {
        pages_->setCurrentIndex(translationPageIndex_);
        navigation_->setCurrentItem(QStringLiteral("translation"));
    }
    show(); raise(); activateWindow();
}
void LinuxSettingsWindow::setShortcut(GlobalShortcut *shortcut) {
    shortcut_ = shortcut;
    shortcutsPage_->setShortcut(shortcut);
}
void LinuxSettingsWindow::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && autostart_) {
        const QSignalBlocker blocker(autostart_);
        autostart_->setChecked(LinuxAutostart::isEnabled());
    }
}
void LinuxSettingsWindow::importSettings() {
    const auto title = settingsText("Import Settings");
    const auto path = QFileDialog::getOpenFileName(this, title, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                   settingsText("Settings File (*.ini *.json)"));
    if (path.isEmpty()) return;
    const auto error = settings_.importPreferences(path);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, title, error);
        return;
    }

    jietuba::get_ui_theme()->set_mode(settings_.theme(), false);
    jietuba::get_theme().set_selection_border_width(settings_.borderWidth());
    // Linux 各页立即保存；重建控件让导入值出现在其它页，不更改打开的设置窗口。
    // Rebuild the immediate-save controls from the imported values, keeping the same window open.
    QTimer::singleShot(0, this, [this, title] {
        for (auto *control : content_->findChildren<QObject *>()) control->disconnect(this);
        autostart_ = nullptr;
        delete content_;
        buildPages();
        setShortcut(shortcut_);
        navigation_->setCurrentItem("other");
        pages_->setCurrentIndex(otherPageIndex_);
        QMessageBox::information(this, title, settingsText("Settings imported."));
    });
}
void LinuxSettingsWindow::persist() {
    // QSettings 默认可能延迟写盘；显式同步并向用户报告不可写的配置目录。
    // Surface persistence errors instead of silently pretending settings were saved.
    if (settings_.sync() != QSettings::NoError)
        QMessageBox::warning(this, QStringLiteral("设置保存失败"), QStringLiteral("无法写入配置文件：\n%1").arg(settings_.fileName()));
}
