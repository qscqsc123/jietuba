#include "gif_capture_window.h"
#include "annotation_canvas.h"
#include "linux_settings.h"
#include "scroll_capture_overlay.h"
#include "wayland_surface_support.h"
#include "gif/_widgets.h"
#include "gif/video_settings_panel.h"
#include "core/ui_scale.h"
#include "ui/base_settings_panel.h"
#include "ui/wait_bar.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QUndoStack>
#include <algorithm>
#include <utility>
#include <QUuid>

namespace {
QPushButton *iconButton(QWidget *parent, const char *name, const QString &tip) {
    auto *button = new QPushButton(parent);
    using M = jietuba::RecordToolbarMetrics;
    button->setIcon(jietuba::svg_icon(QString::fromUtf8(name), jietuba::scaled(M::ICON)));
    button->setToolTip(tip); button->setAccessibleName(tip); button->setFixedSize(jietuba::scaled(M::BUTTON_WIDTH), jietuba::scaled(M::BUTTON_HEIGHT));
    button->setIconSize(QSize(jietuba::scaled(M::ICON), jietuba::scaled(M::ICON)));
    return button;
}
/**
 * 透明录制边框只绘制在选区之外；mask 同时限制 Wayland 鼠标输入区域。
 * 光标模式下选区内穿透，绘图模式下接收输入；原版画布、工具栏和设置面板共用此表面。
 * Transparent recording outline outside the crop. Its mask also restricts Wayland
 * input. Cursor mode passes through; drawing mode receives crop input. Canvas,
 * toolbar and original settings panels share this native surface.
 */
class RecordingSurface final : public QWidget {
public:
    QRect crop, controls;
    QWidget *canvas=nullptr;
    bool drawingActive=false;
    bool maskQueued=false;
    bool showCropOutline = true;
    explicit RecordingSurface(const QSize &size, const QRect &area, const QRect &bar) : crop(area), controls(bar) {
        setObjectName(QStringLiteral("linuxRecordingSurface"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); resize(size);
        // 留两逻辑像素空隙，分数缩放的抗锯齿也不会把边框混进选区。
        // A two-logical-pixel gap prevents fractional-scale filtering bleeding in.
        refreshMask();
        qApp->installEventFilter(this);
    }
    void setRegion(QWidget *toolbar,const QRect &region) {
        crop=region;
        // 工具栏只按选区避让规则跟随；移动按钮控制的是采集区域，不是任意拖动工具栏。
        // The grip moves the capture rectangle; toolbar placement follows the region's avoidance rules.
        const QRect next=LinuxGifToolbar::position(size(),crop,toolbar->size());
        if(!next.isEmpty()) controls=next;
        toolbar->move(controls.topLeft());
        if(canvas) canvas->setGeometry(crop);
        refreshMask(); update();
    }
    void refreshMask() {
        QRegion input;
        if (showCropOutline) input=QRegion(crop.adjusted(-5,-5,5,5)).subtracted(QRegion(crop.adjusted(-2,-2,2,2)));
        if(drawingActive) input|=crop;
        // 样式面板和二级菜单都在此原生表面，只有可见控件参与输入区域。
        // Panels/popups share this surface; only visible controls receive input outside active drawing mode.
        for(auto *child:findChildren<QWidget *>(QString(),Qt::FindDirectChildrenOnly))
            if(child!=canvas && !child->isHidden() && !child->isWindow()
                && !child->testAttribute(Qt::WA_TransparentForMouseEvents)) input|=child->geometry();
        input&=rect();
        // QWidget::setMask 同时裁剪绘制：旧边框离开新遮罩后永远无法清除，拖动会堆叠残影。
        // 仅设置原生窗口输入区域，让整个透明 QWidget 的缓冲仍可完整重绘。
        // QWidget masks clip painting too, trapping old outlines outside the new mask. Set only native
        // input instead, so the whole alpha backing buffer remains repaintable when the crop moves.
        if(windowHandle()) windowHandle()->setMask(input.isEmpty() ? QRegion(QRect(-2,-2,1,1)) : input);
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event) override {
        auto *widget=qobject_cast<QWidget *>(watched);
        if(widget && widget!=this && widget->window()==this && (event->type()==QEvent::Show || event->type()==QEvent::Hide
            || event->type()==QEvent::Move || event->type()==QEvent::Resize) && !maskQueued) {
            maskQueued=true; QTimer::singleShot(0,this,[this] {maskQueued=false; refreshMask();});
        }
        return QWidget::eventFilter(watched,event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(),Qt::transparent);
        if (!showCropOutline) return;
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver); painter.setPen(Qt::NoPen); painter.setBrush(QColor("#40e0d0"));
        painter.setClipRegion(QRegion(crop.adjusted(-5,-5,5,5)).subtracted(QRegion(crop.adjusted(-2,-2,2,2))));
        painter.drawRect(crop.adjusted(-5,-5,5,5));
    }
};
}

LinuxGifToolbar::LinuxGifToolbar(LinuxSettings &settings, QWidget *parent) : QWidget(parent), settings_(settings), delay_(settings.gifDelay()),format_(settings.recordingMode()),videoOptions_(settings.videoOptions()) {
    using M = jietuba::RecordToolbarMetrics;
    setObjectName(QStringLiteral("linuxGifToolbar"));
    setAttribute(Qt::WA_TranslucentBackground); setFixedHeight(jietuba::scaled(M::HEIGHT)); setFocusPolicy(Qt::NoFocus);
    setStyleSheet(jietuba::toolbar_style());
    auto *outer=new QVBoxLayout(this); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    // 浮动宿主按当前控件计算宽度，不能保留切换模式前的布局最小尺寸。
    // The floating host sizes the bar from visible controls, not the previous layout minimum.
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    controlsContainer_=new QWidget(this); outer->addWidget(controlsContainer_);
    auto *row = new QHBoxLayout(controlsContainer_); controlsLayout_=row;
    const int inset = (jietuba::scaled(M::HEIGHT)-jietuba::scaled(M::BUTTON_HEIGHT))/2;
    row->setContentsMargins(jietuba::scaled(M::MARGIN),inset,jietuba::scaled(M::MARGIN),inset); row->setSpacing(jietuba::scaled(M::SPACING));
    // 原版时间按钮就是倒计时入口，不再单独摆放一个占宽度的系统下拉框。
    // As on Windows, the time button opens countdown choices, replacing the extra native combo.
    time_ = new QPushButton(QStringLiteral("00:00"),this); time_->setObjectName(QStringLiteral("gifDelay"));
    time_->setStyleSheet(jietuba::recording_time_style()); time_->setToolTip(QStringLiteral("录制延迟"));
    // 为最长倒计时保留宽度，防止开始后挤动停止/关闭按钮或越过覆盖层的安全边界。
    // Reserve countdown text width so start/stop controls never shift outside the safe overlay slot.
    time_->ensurePolished();
    time_->setFixedWidth(std::max(time_->sizeHint().width(),time_->fontMetrics().horizontalAdvance(QStringLiteral("倒计时 10"))+jietuba::scaled(8)));
    connect(time_,&QPushButton::clicked,this,&LinuxGifToolbar::showDelayMenu); row->addWidget(time_);
    mode_ = new jietuba::ClickMenuButton({{QStringLiteral("GIF"),QStringLiteral("gif")},{QStringLiteral("MP4"),QStringLiteral("mp4")}},format_=="mp4" ? 1 : 0,this);
    mode_->setObjectName(QStringLiteral("gifFormat")); mode_->setToolTip(QStringLiteral("录制格式")); row->addWidget(mode_);
    connect(mode_,&jietuba::ClickMenuButton::option_selected,this,[this](const QVariant &value) {
        format_=value.toString(); settings_.setRecordingMode(format_); settings_.sync(); refreshOptions(); emit optionsChanged();
    });
    std::vector<std::pair<QString,QVariant>> fpsOptions; int current = 0;
    for (int fps : {5,10,16,24,30}) {
        if (fps == settings.gifFps()) current = int(fpsOptions.size());
        fpsOptions.emplace_back(QStringLiteral("%1 fps").arg(fps),fps);
    }
    fps_ = new jietuba::ClickMenuButton(std::move(fpsOptions),current,this); fps_->setObjectName(QStringLiteral("gifFps")); row->addWidget(fps_);
    connect(fps_,&jietuba::ClickMenuButton::option_selected,this,[this](const QVariant &value) {
        if(!value.isValid()) {showVideoSettings(); return;}
        if(format_=="mp4") {videoOptions_.fps=value.toInt(); settings_.setVideoOptions(videoOptions_);}
        else settings_.setGifFps(value.toInt());
        settings_.sync();
    });
    record_ = iconButton(this, "开始录制.svg", QStringLiteral("开始录制")); record_->setObjectName(QStringLiteral("gifStartStop")); row->addWidget(record_);
    pause_ = iconButton(this, "暂停不可.svg", QStringLiteral("暂停录制")); pause_->setObjectName(QStringLiteral("gifPause")); row->addWidget(pause_);
    audio_=iconButton(this,"录制声音.svg",QStringLiteral("录制系统声音")); audio_->setObjectName("videoAudioMenu"); row->addWidget(audio_);
    cursor_=iconButton(this,"鼠标指针.svg",QStringLiteral("录制鼠标指针")); cursor_->setObjectName("videoCursorToggle"); cursor_->setCheckable(true); row->addWidget(cursor_);
    videoSettings_=iconButton(this,"录制设置.svg",QStringLiteral("视频设置")); videoSettings_->setObjectName("videoSettings"); row->addWidget(videoSettings_);
    connect(audio_,&QPushButton::clicked,this,&LinuxGifToolbar::showAudioMenu);
    connect(videoSettings_,&QPushButton::clicked,this,&LinuxGifToolbar::showVideoSettings);
    connect(cursor_,&QPushButton::clicked,this,[this](bool checked) {videoOptions_.cursor=checked; settings_.setVideoOptions(videoOptions_); settings_.sync();});
    auto separator = [this,row] {
        auto *line = new QWidget(this); line->setObjectName(QStringLiteral("gifSeparator"));
        line->setFixedSize(1,jietuba::scaled(M::SEPARATOR_HEIGHT)); line->setStyleSheet(QStringLiteral("background:rgba(15,23,42,0.12); border:none;")); row->addWidget(line);
        regionalControls_.append(line);
    };
    separator();
    // 复用原版工具顺序，实际绘图由共享 CanvasScene/ToolController/设置面板处理。
    // Original tool order delegates to the shared canvas, tool controller and style panels.
    for (const auto &tool : jietuba::RECORDING_DRAW_TOOLS) {
        auto *button = iconButton(this,tool.svg,QString::fromUtf8(tool.tip));
        button->setObjectName(QStringLiteral("gifDraw_")+QString::fromLatin1(tool.id)); button->setCheckable(true); button->setEnabled(false); row->addWidget(button);
        regionalControls_.append(button);
        const auto id=QString::fromLatin1(tool.id); tools_.insert(id,button);
        connect(button,&QPushButton::clicked,this,[this,id] {emit toolRequested(id);});
    }
    separator();
    undo_=iconButton(this,"撤回.svg",QStringLiteral("撤销")); undo_->setObjectName(QStringLiteral("gifUndo"));
    redo_=iconButton(this,"复原.svg",QStringLiteral("重做")); redo_->setObjectName(QStringLiteral("gifRedo"));
    for(auto *button:{undo_,redo_}) {button->setEnabled(false); row->addWidget(button); regionalControls_.append(button);}
    connect(undo_,&QPushButton::clicked,this,&LinuxGifToolbar::undoRequested);
    connect(redo_,&QPushButton::clicked,this,&LinuxGifToolbar::redoRequested);
    row->addStretch();
    moveHandle_ = new QLabel(this); moveHandle_->setObjectName(QStringLiteral("gifMoveHandle")); moveHandle_->setAlignment(Qt::AlignCenter);
    moveHandle_->setFixedSize(jietuba::scaled(M::HANDLE_WIDTH),jietuba::scaled(M::BUTTON_HEIGHT));
    moveHandle_->setPixmap(jietuba::svg_icon(QStringLiteral("移动窗口.svg"),jietuba::scaled(M::HANDLE_ICON)).pixmap(jietuba::scaled(M::HANDLE_ICON),jietuba::scaled(M::HANDLE_ICON)));
    moveHandle_->installEventFilter(this); row->addWidget(moveHandle_);
    regionalControls_.append(moveHandle_);
    // 暂停按钮在普通 Wayland 窗口回退中也保留。 / Keep pause available in the xdg fallback.
    trayHint_ = new QLabel(QStringLiteral("窗口控制"), this);
    trayHint_->setObjectName("recordingTrayHint");
    trayHint_->setToolTip(QStringLiteral("录制会保留屏幕上可见的窗口和工具栏。"));
    row->addWidget(trayHint_);
    trayHint_->hide();
    auto *close = iconButton(this, "结束截图.svg", QStringLiteral("取消并关闭")); close->setObjectName(QStringLiteral("gifClose")); row->addWidget(close);
    connect(record_, &QPushButton::clicked, this, &LinuxGifToolbar::startStopRequested);
    connect(pause_, &QPushButton::clicked, this, &LinuxGifToolbar::pauseRequested);
    connect(close, &QPushButton::clicked, this, &LinuxGifToolbar::closeRequested);
    // 完成条沿用截图工具栏的图标尺寸和悬停提示，避免一排文字挤占画面。
    // Use capture-toolbar icon metrics and hover hints for a compact result row.
    resultContainer_=new QWidget(this); resultContainer_->setObjectName("videoResultControls");
    auto *results=new QHBoxLayout(resultContainer_); results->setContentsMargins(jietuba::scaled(M::MARGIN),inset,jietuba::scaled(M::MARGIN),inset); results->setSpacing(jietuba::scaled(M::SPACING));
    resultLabel_=new QLabel(QStringLiteral("视频已保存"),resultContainer_); results->addWidget(resultLabel_);
    const std::pair<const char *,void(LinuxGifToolbar::*)()> actions[]={{"播放预览",&LinuxGifToolbar::resultPlayRequested},
        {"打开文件夹",&LinuxGifToolbar::resultFolderRequested},{"复制文件",&LinuxGifToolbar::resultCopyRequested},{"重新录制",&LinuxGifToolbar::resultRerecordRequested}};
    const char *resultIcons[]={"播放预览.svg","fluent_icons/Folder.svg","复制.svg","重新录制.svg"};
    for(int i=0;i<4;++i) {auto *button=iconButton(resultContainer_,resultIcons[i],QString::fromUtf8(actions[i].first)); button->setObjectName(QStringLiteral("videoResult_%1").arg(i));
        if(i==0) resultPlay_=button;
        results->addWidget(button); connect(button,&QPushButton::clicked,this,actions[i].second);}
    resultSave_ = iconButton(resultContainer_, "下载.svg", QStringLiteral("另存保留区间…"));
    resultSave_->setObjectName("gifSaveTrim");
    results->insertWidget(3, resultSave_);
    connect(resultSave_, &QPushButton::clicked, this, &LinuxGifToolbar::resultSaveRequested);
    auto *resultClose=iconButton(resultContainer_,"结束截图.svg",QStringLiteral("关闭")); resultClose->setObjectName("recordingResultClose"); results->addWidget(resultClose);
    connect(resultClose,&QPushButton::clicked,this,&LinuxGifToolbar::closeRequested);
    outer->addWidget(resultContainer_); resultContainer_->hide();
    refreshOptions();
    setState(QStringLiteral("00:00"), false, false, false);
}
int LinuxGifToolbar::fps() const { return fps_->current_value().toInt(); }
int LinuxGifToolbar::delay() const { return delay_; }
void LinuxGifToolbar::paintEvent(QPaintEvent *) { jietuba::paint_rounded_panel(this); }
void LinuxGifToolbar::setState(const QString &time, bool recording, bool paused, bool busy) {
    busy_=busy;
    time_->setText(time); fps_->set_enabled(!recording && !busy); mode_->set_enabled(!recording && !busy); time_->setEnabled(!recording && !busy);
    for(auto *button:{audio_,cursor_,videoSettings_}) button->setEnabled(!recording && !busy);
    if((recording || busy) && videoDialog_) videoDialog_->reject();
    time_->setToolTip(QStringLiteral("录制延迟：%1").arg(delay_ ? QStringLiteral("%1 秒").arg(delay_) : QStringLiteral("无延迟")));
    moveHandle_->setEnabled(!busy); moveHandle_->setCursor(busy ? Qt::ArrowCursor : Qt::SizeAllCursor);
    moveHandle_->setToolTip(QStringLiteral("按住拖动录制选框"));
    for(auto *button:tools_) button->setEnabled(drawingAvailable_ && !busy);
    if (busy) { dragPosition_.reset(); if (delayMenu_) delayMenu_->close(); }
    else if(recording && delayMenu_) delayMenu_->close();
    record_->setEnabled(!busy); pause_->setEnabled(recording && !busy);
    record_->setIcon(jietuba::svg_icon(QString::fromUtf8(recording ? "结束录制.svg" : "开始录制.svg"), 24));
    record_->setToolTip(recording ? QStringLiteral("停止录制并自动保存") : trayOnly_ ? trayHint_->toolTip() : QStringLiteral("开始录制（最长 60 秒）"));
    pause_->setIcon(jietuba::svg_icon(QString::fromUtf8(paused ? "重开录制.svg" : recording ? "暂停录制.svg" : "暂停不可.svg"), 24));
    pause_->setToolTip(paused ? QStringLiteral("继续录制") : QStringLiteral("暂停录制"));
}
void LinuxGifToolbar::refreshOptions() {
    const bool video=format_=="mp4";
    for(auto *button:{audio_,cursor_,videoSettings_}) button->setVisible(video);
    cursor_->setChecked(videoOptions_.cursor);
    audio_->setIcon(jietuba::svg_icon(videoOptions_.system_audio ? QStringLiteral("录制声音.svg") : QStringLiteral("录制静音.svg"),24));
    std::vector<std::pair<QString,QVariant>> entries; int index=0;
    if(video) {
        QList<int> values{15,24,30,48,60,videoOptions_.fps}; std::sort(values.begin(),values.end()); values.erase(std::unique(values.begin(),values.end()),values.end());
        for(int value:values) {if(value==videoOptions_.fps) index=int(entries.size()); entries.emplace_back(QStringLiteral("%1 fps").arg(value),value);}
        entries.emplace_back(QStringLiteral("自定义帧率…"),QVariant{});
    } else for(int value:{5,10,16,24,30}) {if(value==settings_.gifFps()) index=int(entries.size()); entries.emplace_back(QStringLiteral("%1 fps").arg(value),value);}
    fps_->set_options(std::move(entries),index);
    controlsLayout_->setSpacing(jietuba::scaled(jietuba::RecordToolbarMetrics::SPACING));
    controlsLayout_->activate(); layout()->activate();
    if(screen() && sizeHint().width()>screen()->geometry().width()) controlsLayout_->setSpacing(2);
    adjustSize();
}
void LinuxGifToolbar::showAudioMenu() {
    auto *menu=new QMenu(this); auto *action=menu->addAction(QStringLiteral("录制系统声音")); action->setCheckable(true); action->setChecked(videoOptions_.system_audio);
    connect(action,&QAction::triggered,this,[this](bool checked) {videoOptions_.system_audio=checked; settings_.setVideoOptions(videoOptions_); settings_.sync(); refreshOptions();});
    menu->addSeparator(); menu->addAction(QStringLiteral("系统声音不包含麦克风"))->setEnabled(false);
    jietuba::popup_menu(menu,audio_,popupBelow_);
}
void LinuxGifToolbar::showVideoSettings() {
    if(videoDialog_) {videoDialog_->raise(); return;}
    auto *host=window(); auto *dialog=new QDialog(host); videoDialog_=dialog; dialog->setObjectName("linuxVideoSettings");
    dialog->setWindowTitle(QStringLiteral("视频设置")); auto *layout=new QVBoxLayout(dialog);
    auto *panel=new jietuba::VideoSettingsPanel(videoOptions_,dialog); layout->addWidget(panel);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(dialog,&QDialog::accepted,this,[this,panel] {videoOptions_=panel->options(); settings_.setVideoOptions(videoOptions_); settings_.sync(); refreshOptions(); emit optionsChanged();});
    connect(dialog,&QDialog::finished,this,[this,dialog] {videoDialog_=nullptr; dialog->deleteLater();});
    // layer-shell 表面上的设置采用同表面子对话框，避免普通模态窗口被覆盖层挡住。
    // Embed settings on the layer surface so a native modal cannot be hidden behind it.
    if(host->objectName()=="linuxRecordingSurface") {
        dialog->setWindowFlags(Qt::Widget); dialog->setStyleSheet(QStringLiteral("QDialog {background:palette(window); border:1px solid #9ca3af; border-radius:6px;}"));
        dialog->adjustSize(); dialog->move(QPoint(std::max(0,(host->width()-dialog->width())/2),std::max(0,(host->height()-dialog->height())/2))); dialog->show(); dialog->raise();
    } else dialog->open();
}
void LinuxGifToolbar::showRecordingResult(const QString &path) {
    controlsContainer_->hide(); resultLabel_->setToolTip(path);
    const bool gif = path.endsWith(".gif", Qt::CaseInsensitive);
    resultSave_->setVisible(gif);
    resultLabel_->setText(gif ? QStringLiteral("GIF 已保存") : QStringLiteral("MP4 已保存"));
    setResultPlaying(false); resultContainer_->show(); adjustSize();
}
void LinuxGifToolbar::setResultPlaying(bool playing) {
    const QString hint=playing ? QStringLiteral("暂停预览") : QStringLiteral("播放预览");
    resultPlay_->setIcon(jietuba::svg_icon(playing ? QStringLiteral("暂停预览.svg") : QStringLiteral("播放预览.svg"),jietuba::scaled(jietuba::RecordToolbarMetrics::ICON)));
    resultPlay_->setToolTip(hint); resultPlay_->setAccessibleName(hint);
}
void LinuxGifToolbar::setResultMessage(const QString &text,const QString &detail) {resultLabel_->setText(text); resultLabel_->setToolTip(detail);}
void LinuxGifToolbar::setResultBusy(bool busy) {
    for (auto *button : resultContainer_->findChildren<QPushButton *>()) button->setEnabled(!busy);
}
void LinuxGifToolbar::showRecordingControls() {resultContainer_->hide(); controlsContainer_->show(); refreshOptions();}
void LinuxGifToolbar::setDrawingState(const QString &tool,bool canUndo,bool canRedo,bool available) {
    drawingAvailable_=available;
    for(auto it=tools_.begin();it!=tools_.end();++it) {it.value()->setChecked(it.key()==tool); it.value()->setEnabled(available && !busy_);}
    undo_->setEnabled(available && canUndo && !busy_); redo_->setEnabled(available && canRedo && !busy_);
}
void LinuxGifToolbar::setPopupBelow(bool below) { popupBelow_ = below; fps_->popup_below = below; mode_->popup_below = below; }
void LinuxGifToolbar::setTrayOnly(bool trayOnly) {
    trayOnly_ = trayOnly;
    for (auto *control : regionalControls_) control->setVisible(!trayOnly);
    trayHint_->setVisible(trayOnly);
    controlsLayout_->activate();
    layout()->activate();
    adjustSize();
}
void LinuxGifToolbar::showDelayMenu() {
    if (!time_->isEnabled()) return;
    if (delayMenu_) { delayMenu_->close(); return; }
    auto *menu = new QMenu(this); delayMenu_ = menu; menu->setObjectName(QStringLiteral("gifDelayMenu")); menu->addSection(QStringLiteral("录制延迟"));
    for (int seconds : {0,1,2,3,5,10}) {
        auto *action = menu->addAction(seconds ? QStringLiteral("%1 秒").arg(seconds) : QStringLiteral("无延迟"));
        action->setData(seconds); action->setCheckable(true); action->setChecked(seconds == delay_);
        connect(action,&QAction::triggered,this,[this,seconds] {
            delay_ = seconds; settings_.setGifDelay(seconds); settings_.sync();
            time_->setToolTip(QStringLiteral("录制延迟：%1").arg(seconds ? QStringLiteral("%1 秒").arg(seconds) : QStringLiteral("无延迟")));
        });
    }
    connect(menu,&QMenu::aboutToHide,this,[this] { delayMenu_ = nullptr; });
    jietuba::popup_menu(menu,time_,popupBelow_);
}
bool LinuxGifToolbar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == moveHandle_ && moveHandle_->isEnabled()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {dragPosition_ = mouse->globalPosition().toPoint(); emit moveStarted();}
            return true;
        }
        if (event->type() == QEvent::MouseMove && dragPosition_) {
            const QPoint next = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            const QPoint delta = next - *dragPosition_; dragPosition_ = next; emit moveRequested(delta); return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && dragPosition_) { dragPosition_.reset(); emit moveFinished(); return true; }
    }
    return QWidget::eventFilter(watched,event);
}
QRect LinuxGifToolbar::position(const QSize &screen, const QRect &selection, const QSize &toolbar) {
    // 使用完整工具栏实际宽度，原版优先选区下方，再翻到上方和两侧；绝不侵入采集区。
    // Place the actual full row below first, then above or beside, never inside the crop.
    if (screen.isEmpty() || selection.isEmpty() || toolbar.isEmpty() || toolbar.width()>screen.width() || toolbar.height()>screen.height()) return {};
    constexpr int gap = 6;
    const QRect bounds(QPoint(),screen);
    const int x = std::clamp(selection.center().x()-toolbar.width()/2,0,screen.width()-toolbar.width());
    const int y = std::clamp(selection.center().y()-toolbar.height()/2,0,screen.height()-toolbar.height());
    for (const QPoint &origin : {QPoint(x,selection.bottom()+1+gap),QPoint(x,selection.top()-gap-toolbar.height()),
                                QPoint(selection.left()-gap-toolbar.width(),y),QPoint(selection.right()+1+gap,y)}) {
        const QRect result(origin,toolbar);
        if (bounds.contains(result) && !result.intersects(selection.adjusted(-2,-2,2,2))) return result;
    }
    return {};
}

LinuxGifCaptureWindow::LinuxGifCaptureWindow(QScreen *screen, const QSize &nativeSize, const QRect &region,
    LinuxSettings &settings, bool trayAvailable, QObject *parent,const QRect &logicalRegion,CaptureRequest request)
    : QObject(parent), screen_(screen), nativeSize_(nativeSize), region_(region), screenGeometry_(screen->geometry()),
      logicalRegion_(logicalRegion),captureRequest_(std::move(request)),
      output_(screen->name()), settings_(settings), trayAvailable_(trayAvailable) {
    router_.setLogSuccessfulCaptures(false);
    frameTimer_.setSingleShot(true); frameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&frameTimer_, &QTimer::timeout, this, &LinuxGifCaptureWindow::capture);
    connect(&router_, &CaptureRouter::frameReady, this, &LinuxGifCaptureWindow::acceptFrame);
    connect(&router_, &CaptureRouter::failed, this, [this](const QString &error) {
        pending_ = false;
        if (state_ != State::Recording) return;
        if (requestEpoch_ != epoch_) { frameTimer_.start(0); return; }
        failCapture(error);
    });
    connect(&frames_,&LinuxGifFrames::ready,this,[this] {
        framesReady_=true; finishRecording();
    });
    connect(&frames_, &LinuxGifFrames::exported, this, [this](const QString &path) {
        if (state_ == State::Finishing) showRecordingResult(path);
        else if (exportingTrim_) {
            const bool copy = bool(clipboardExport_);
            finishTrimExport({});
            if (copy) {
                copyRecording(path, false);
                clipboardExport_.reset();
            } else {
                cancel();
            }
        }
    });
    connect(&frames_, &LinuxGifFrames::exportProgress, this, [this](const QString &text, int percent) {
        if (state_ == State::Finishing) showFinishingProgress(text, percent);
        else if (exportingTrim_) toolbar_->setResultMessage(text);
    });
    connect(&audioRecorder_,&LinuxRecordingAudio::finished,this,[this](const QString &path) {audioReady_=true; audioPath_=path; finishRecording();});
    connect(&audioRecorder_,&LinuxRecordingAudio::failed,this,[this](const QString &reason) {
        audioReady_=true; audioPath_.clear();
        if(state_==State::Recording || state_==State::Paused) failCapture(reason);
        else if(state_==State::Finishing) {captureError_=reason; finishRecording();}
    });
    connect(&frames_, &LinuxGifFrames::failed, this, [this](const QString &reason) {
        if (state_ == State::Recording || state_ == State::Paused) failCapture(reason);
        else if (state_ == State::Finishing) {
            hideFinishingProgress();
            QMessageBox::warning(nullptr, QStringLiteral("录制失败"), reason);
            cancel();
        }
        else if (exportingTrim_) finishTrimExport(reason);
    });
    countdownTimer_.setInterval(1000);
    connect(&countdownTimer_, &QTimer::timeout, this, [this] {
        if (--countdown_ <= 0) { countdownTimer_.stop(); beginCapture(); } else updateControls();
    });
    uiTimer_.setInterval(100);
    connect(&uiTimer_, &QTimer::timeout, this, [this] {
        updateControls(); if (elapsed() >= 60000 && state_ == State::Recording) stop();
    });
    connect(screen, &QScreen::geometryChanged, this, [this](const QRect &geometry) {
        // 桌面布局通知不是停止命令；只有录制输出的实际坐标变化才使区域失效。
        // A layout notification only invalidates this recording if its output geometry actually changed.
        if (geometry == screenGeometry_) return;
        if (state_ == State::Recording || state_ == State::Paused) failCapture(QStringLiteral("显示器配置已改变，录制已停止。"));
        else if (state_ != State::Preview && state_ != State::Finishing) cancel();
    });
    connect(screen, &QObject::destroyed, this, [this] {
        if (state_ == State::Recording || state_ == State::Paused) failCapture(QStringLiteral("录制显示器已断开。"));
        else if (state_ != State::Preview && state_ != State::Finishing) cancel();
    });
    qApp->installEventFilter(this);
}
void LinuxGifCaptureWindow::acceptFrame(const QImage &image) {
    pending_=false;
    if (state_!=State::Recording) return;
    // 暂停/恢复改变代数：先丢弃旧请求，再校验新会话尺寸，不能把迟到帧当作当前帧报错。
    // Drop invalidated requests before validating current geometry; pause/resume must not admit stale frames.
    if (requestEpoch_!=epoch_) { frameTimer_.start(0); return; }
    const bool area=!logicalRegion_.isEmpty();
    const QSize expected=area ? region_.size() : nativeSize_;
    if (!screen_ || screen_->geometry()!=screenGeometry_ || image.isNull() || image.size()!=expected) {
        failCapture(QStringLiteral("采集返回 %1 × %2 像素，预期 %3 × %4；请重新框选录制。")
            .arg(image.width()).arg(image.height()).arg(expected.width()).arg(expected.height())); return;
    }
    // 区域帧就是录制内容，不得按屏幕坐标再裁一次；写盘和编码全部使用未缩放的原生像素。
    // Area replies already contain the recording. Never crop twice or resample native frame pixels.
    QImage frame=area ? image : image.copy(region_);
    frame.setDevicePixelRatio(1);
    if(drawing_ && (captureRequest_ || router_.excludesCallerWindows())) {
        // 真实桌面帧已经含标注与工具栏；只有不含调用方的帧才需要补画标注。
        // Composited desktop frames already contain the canvas. Only bare frames need manual annotation.
        const QImage annotations=drawing_->recordingImage();
        if(!annotations.isNull()) {QPainter painter(&frame); painter.drawImage(QPoint(),annotations);}
    }
    if (frames_.append(frame,requestTime_)) emit frameCaptured(frame.size());
    const int remaining=std::max(0,1000/toolbar_->fps()-int(clock_.elapsed()-lastRequest_));
    frameTimer_.start(remaining);
}
LinuxGifCaptureWindow::~LinuxGifCaptureWindow() {
    audioRecorder_.cancel();
    qApp->removeEventFilter(this); router_.stop();
    // 与屏幕标注一致：先释放画布，再释放承载样式面板的兄弟控件。
    // Release the canvas before its sibling controls and panels, as in screen annotation.
    if(drawing_) {drawing_->recordingChanged={}; delete drawing_.data();}
    if(auto *surface=dynamic_cast<RecordingSurface *>(overlay_.data())) surface->canvas=nullptr;
    if (preview_) { preview_->disconnect(this); delete preview_; }
    delete overlay_; delete fallback_;
}
void LinuxGifCaptureWindow::setScreenshotActive(bool active) {
    if (screenshotActive_ == active) return;
    screenshotActive_ = active;
    updateControlWindowVisibility();
}
void LinuxGifCaptureWindow::updateControlWindowVisibility() {
    if (auto *window = controlWindow()) {
        // 录制中始终保留工具栏；内层编辑期间只延后弹出外层完成预览，避免抢焦点。
        // Keep live recording controls mapped; defer only finishing/result windows over a nested editor.
        window->setVisible(!screenshotActive_ || (state_ != State::Finishing && state_ != State::Preview));
    }
}
void LinuxGifCaptureWindow::showControls() {
    if (!screen_ || nativeSize_.isEmpty() || region_.isEmpty() || !QRect(QPoint(),nativeSize_).contains(region_)) { cancel(); return; }
    const QRect selection = logicalRegion_.isEmpty()
        ? LinuxScrollCaptureOverlay::selectionInLogicalPixels(screenGeometry_.size(),nativeSize_,region_) : logicalRegion_;
    toolbar_ = new LinuxGifToolbar(settings_); toolbar_->adjustSize();
    const bool wayland=QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    const bool canPlace = !wayland || LinuxWayland::supportsLayerShell();
    toolbar_->setTrayOnly(!canPlace);
    if (canPlace) {
        const QRect controls = LinuxGifToolbar::position(screenGeometry_.size(), selection, toolbar_->sizeHint());
        auto *surface = new RecordingSurface(screenGeometry_.size(), selection, controls);
        surface->winId(); surface->windowHandle()->setScreen(screen_);
#ifdef JIETUBA_HAS_LAYER_SHELL
        if(wayland) {
        auto *layer = LayerShellQt::Window::get(surface->windowHandle());
        layer->setScope(QStringLiteral("jietuba-gif-recording")); layer->setScreen(screen_);
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom |
                          LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
        layer->setExclusiveZone(-1); layer->setDesiredSize(screenGeometry_.size());
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        }
#endif
        toolbar_->setParent(surface);
        overlay_=surface;
        {
            drawing_=LinuxAnnotationCanvas::createForRecording(surface,toolbar_,region_.size(),settings_);
            drawing_->setGeometry(selection); surface->canvas=drawing_;
            drawing_->recordingChanged=[this] { synchronizeDrawing(); };
            drawing_->show(); drawing_->lower();
        }
        toolbar_->show();
        positionRecordingControls();
        if (!wayland) surface->move(screenGeometry_.topLeft());
        synchronizeDrawing();
    } else {
        // 无法锚定到指定输出时，由合成器摆放普通窗口，但仍只显示圆角工具条。
        // Let the compositor place the fallback window; keep the same frameless toolbar.
        auto *dialog = new QDialog(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        dialog->setWindowTitle(QStringLiteral("截图吧 — 录屏"));
        dialog->setAttribute(Qt::WA_TranslucentBackground);
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        layout->addWidget(toolbar_, 0, Qt::AlignHCenter);
        connect(dialog, &QDialog::rejected, this, &LinuxGifCaptureWindow::cancel);
        fallback_ = dialog; toolbar_->show();
    }
    updateControlWindowVisibility();
    connect(toolbar_, &LinuxGifToolbar::startStopRequested, this, [this] {
        if (state_ == State::Ready) begin(); else stop();
    });
    connect(toolbar_, &LinuxGifToolbar::pauseRequested, this, &LinuxGifCaptureWindow::toggle);
    connect(toolbar_, &LinuxGifToolbar::closeRequested, this, &LinuxGifCaptureWindow::cancel);
    connect(toolbar_,&LinuxGifToolbar::moveStarted,this,&LinuxGifCaptureWindow::beginRegionMove);
    connect(toolbar_,&LinuxGifToolbar::moveRequested,this,&LinuxGifCaptureWindow::moveRegion);
    connect(toolbar_,&LinuxGifToolbar::moveFinished,this,&LinuxGifCaptureWindow::endRegionMove);
    connect(toolbar_,&LinuxGifToolbar::toolRequested,this,[this](const QString &tool) {
        if(drawing_) drawing_->activateRecordingTool(drawing_->recordingTool()==tool ? QStringLiteral("cursor") : tool);
    });
    connect(toolbar_,&LinuxGifToolbar::undoRequested,this,[this] {if(drawing_) drawing_->recordingUndoStack()->undo();});
    connect(toolbar_,&LinuxGifToolbar::redoRequested,this,[this] {if(drawing_) drawing_->recordingUndoStack()->redo();});
    connect(toolbar_,&LinuxGifToolbar::optionsChanged,this,[this] {
        if (overlay_) positionRecordingControls();
        else if(fallback_) fallback_->adjustSize();
        updateControls();
    });
    connect(toolbar_,&LinuxGifToolbar::resultPlayRequested,this,&LinuxGifCaptureWindow::toggleResultPreview);
    connect(toolbar_,&LinuxGifToolbar::resultFolderRequested,this,[this] {
        if(fileManager_.isOpening()) return;
        toolbar_->setResultBusy(true); fileManager_.reveal(resultPath_);
    });
    // 必须等桌面服务确认或系统打开器成功退出，再撤掉位于最上层的预览。
    // Close the topmost preview only after the file manager acknowledges or the opener exits successfully.
    connect(&fileManager_,&LinuxFileManager::opened,this,&LinuxGifCaptureWindow::cancel);
    connect(&fileManager_,&LinuxFileManager::failed,this,[this](const QString &reason) {
        toolbar_->setResultBusy(false); toolbar_->setResultMessage(QStringLiteral("打开文件夹失败"),reason);
        qWarning().noquote()<<reason;
    });
    connect(toolbar_,&LinuxGifToolbar::resultCopyRequested,this,[this] {
        if (recordingMp4_ || !preview_) { copyRecording(resultPath_, true); return; }
        const auto [first, last] = preview_->trimRange();
        if (last < 0 || (first == 0 && last == frames_.frames().size() - 1)) copyRecording(resultPath_, true);
        else exportTrimmedGif(true);
    });
    connect(toolbar_, &LinuxGifToolbar::resultSaveRequested, this, [this] { exportTrimmedGif(false); });
    connect(toolbar_,&LinuxGifToolbar::resultRerecordRequested,this,&LinuxGifCaptureWindow::rerecord);
    updateControls();
}
void LinuxGifCaptureWindow::positionRecordingControls() {
    auto *surface = static_cast<RecordingSurface *>(overlay_.data());
    const auto selection = logicalCaptureRegion();
    auto size = toolbar_->sizeHint();
    const auto outside = LinuxGifToolbar::position(surface->size(), selection, size);
    if (outside.isEmpty()) {
        // 全屏没有外侧空间，完整工具条放在底部，绘图画布仍可用。
        // Keep the full toolbar and drawing canvas available at the bottom during full-screen recording.
        const auto available = screen_ ? screen_->availableGeometry().translated(-screenGeometry_.topLeft()) : surface->rect();
        surface->controls = QRect(QPoint(available.center().x() - size.width()/2,
                                         std::max(available.top(), available.bottom() - size.height() - 15)), size);
        toolbar_->setGeometry(surface->controls);
        toolbar_->setPopupBelow(false);
        surface->showCropOutline = false;
        surface->refreshMask();
        surface->update();
    } else {
        surface->showCropOutline = true;
        toolbar_->resize(size);
        surface->setRegion(toolbar_, selection);
        toolbar_->setPopupBelow(outside.top() > selection.bottom());
    }
    if (drawing_) drawing_->layoutRecordingPanels();
}
void LinuxGifCaptureWindow::copyRecording(const QString &path, bool includeFileUrl) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { toolbar_->setResultMessage(QStringLiteral("复制失败：文件不可读")); return; }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.isEmpty()) { toolbar_->setResultMessage(QStringLiteral("读取文件失败")); return; }
    auto *mime = new QMimeData;
    // 剪辑复制只保留动画数据；不提供指向即将删除的临时文件的链接。
    // A trimmed copy owns its GIF bytes and never advertises a temporary file URL.
    if (includeFileUrl) mime->setUrls({QUrl::fromLocalFile(path)});
    mime->setData(recordingMp4_ ? QStringLiteral("video/mp4") : QStringLiteral("image/gif"), bytes);
    QApplication::clipboard()->setMimeData(mime);
    cancel();
}

void LinuxGifCaptureWindow::exportTrimmedGif(bool copy) {
    if (state_ != State::Preview || recordingMp4_ || !preview_ || exportingTrim_) return;
    const auto [first, last] = preview_->trimRange();
    if (last < first) return;
    preview_->pausePlayback();
    QString path;
    if (copy) {
        clipboardExport_ = std::make_unique<QTemporaryDir>();
        if (!clipboardExport_->isValid()) {
            clipboardExport_.reset();
            toolbar_->setResultMessage(QStringLiteral("无法创建剪贴板临时文件"));
            return;
        }
        path = clipboardExport_->filePath("trim.gif");
    } else {
        const QFileInfo original(resultPath_);
        QFileDialog dialog(preview_, QStringLiteral("另存 GIF"), original.absolutePath(), QStringLiteral("GIF 动画 (*.gif)"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("gif");
        dialog.selectFile(original.completeBaseName() + QStringLiteral("_剪辑.gif"));
        if (dialog.exec() != QDialog::Accepted) return;
        path = dialog.selectedFiles().value(0);
        const QFileInfo destination(path);
        if (destination.absoluteFilePath() == original.absoluteFilePath()
            || (!destination.canonicalFilePath().isEmpty() && destination.canonicalFilePath() == original.canonicalFilePath())) {
            toolbar_->setResultMessage(QStringLiteral("请选择新文件名，保留原录制"));
            return;
        }
    }
    exportingTrim_ = true;
    toolbar_->setResultBusy(true);
    preview_->setEnabled(false);
    // 从原始无损帧按闭区间重新编码，保留每帧时长，不二次量化已保存的 GIF。
    // Encode the inclusive interval from the original frames, retaining timing and avoiding double quantization.
    if (!frames_.exportGif(path, first, last) && exportingTrim_)
        finishTrimExport(QStringLiteral("无法导出所选区间"));
}

void LinuxGifCaptureWindow::finishTrimExport(const QString &error) {
    exportingTrim_ = false;
    toolbar_->setResultBusy(false);
    if (preview_) preview_->setEnabled(true);
    if (!error.isEmpty()) {
        clipboardExport_.reset();
        toolbar_->setResultMessage(QStringLiteral("GIF 导出失败"), error);
    } else {
        toolbar_->setResultMessage(QStringLiteral("GIF 已保存"), resultPath_);
    }
}
void LinuxGifCaptureWindow::synchronizeDrawing() {
    if(!toolbar_) return;
    if(drawing_) {
        const auto tool=drawing_->recordingTool();
        auto *stack=drawing_->recordingUndoStack();
        toolbar_->setDrawingState(tool,stack->canUndo(),stack->canRedo(),true);
        drawing_->setAttribute(Qt::WA_TransparentForMouseEvents,tool.isEmpty());
        if(auto *surface=dynamic_cast<RecordingSurface *>(overlay_.data())) {
            surface->drawingActive=!tool.isEmpty(); surface->refreshMask();
        }
    } else {
        toolbar_->setDrawingState({},false,false,false);
        if (overlay_) static_cast<RecordingSurface *>(overlay_.data())->drawingActive = false;
    }
}
QRect LinuxGifCaptureWindow::logicalCaptureRegion() const {
    return logicalRegion_.isEmpty() ? LinuxScrollCaptureOverlay::selectionInLogicalPixels(screenGeometry_.size(),nativeSize_,region_) : logicalRegion_;
}
void LinuxGifCaptureWindow::beginRegionMove() {
    if(state_!=State::Ready && state_!=State::Recording && state_!=State::Paused) return;
    movingRegion_=true; ++epoch_; frameTimer_.stop();
    if(drawing_) drawing_->activateRecordingTool("cursor");
}
void LinuxGifCaptureWindow::moveRegion(const QPoint &delta) {
    if(!movingRegion_ || !screen_ || delta.isNull()) return;
    const QRect old=logicalCaptureRegion();
    QRect next=old.translated(delta);
    next.moveLeft(std::clamp(next.x(),0,std::max(0,screenGeometry_.width()-next.width())));
    next.moveTop(std::clamp(next.y(),0,std::max(0,screenGeometry_.height()-next.height())));
    if(next==old) return;
    if (overlay_ && LinuxGifToolbar::position(overlay_->size(), next, toolbar_->size()).isEmpty()) return;
    // 录制尺寸不变，只移动原点；nativeSize_ 是选区首帧的缩放坐标系，不能改用 Qt 整数 DPR。
    // Keep frame dimensions fixed and move the origin in the first frame's native coordinate system.
    const QPoint native(qRound(double(next.x())*nativeSize_.width()/screenGeometry_.width()),
                        qRound(double(next.y())*nativeSize_.height()/screenGeometry_.height()));
    region_.moveTo(std::clamp(native.x(),0,std::max(0,nativeSize_.width()-region_.width())),
                   std::clamp(native.y(),0,std::max(0,nativeSize_.height()-region_.height())));
    if(!logicalRegion_.isEmpty()) logicalRegion_=next;
    if(auto *surface=dynamic_cast<RecordingSurface *>(overlay_.data())) {
        surface->setRegion(toolbar_,next); toolbar_->setPopupBelow(surface->controls.top()>next.bottom());
        if(drawing_) drawing_->layoutRecordingPanels();
    }
}
void LinuxGifCaptureWindow::endRegionMove() {
    if(!movingRegion_) return;
    movingRegion_=false;
    if(state_==State::Recording && !pending_) capture();
}
qint64 LinuxGifCaptureWindow::elapsed() const {
    if (!clock_.isValid()) return 0;
    return std::max<qint64>(0, (state_ == State::Paused ? pausedAt_ : clock_.elapsed()) - pausedTotal_);
}
void LinuxGifCaptureWindow::begin() {
    if (state_ != State::Ready) return;
    if (LinuxGifFrames::ffmpegPath().isEmpty() || !frames_.valid()) {
        QMessageBox::warning(fallback_, QStringLiteral("无法开始录制"),
            LinuxGifFrames::ffmpegPath().isEmpty() ? QStringLiteral("请先安装发行版提供的 FFmpeg（ffmpeg 软件包）。") : QStringLiteral("无法创建录制缓存目录。"));
        return;
    }
    movingRegion_=false;
    recordingMp4_=toolbar_->recordingMode()=="mp4"; recordingVideoOptions_=toolbar_->videoOptions();
    audioPath_.clear(); framesReady_=false; audioReady_=!recordingMp4_ || !recordingVideoOptions_.system_audio;
    state_ = State::Countdown; countdown_ = toolbar_->delay(); updateControls();
    if (countdown_ > 0) countdownTimer_.start(); else beginCapture();
}
void LinuxGifCaptureWindow::beginCapture() {
    if (state_ != State::Countdown) return;
    updateControlWindowVisibility();
    QTimer::singleShot(150, this, [this] {
        if (state_ != State::Countdown) return;
        if (!screen_ || screen_->geometry() != screenGeometry_) { cancel(); return; }
        state_ = State::Recording; ++epoch_; clock_.start(); uiTimer_.start(); updateControls();
        if(recordingMp4_ && recordingVideoOptions_.system_audio && !audioRecorder_.start()) return;
        capture();
    });
}
void LinuxGifCaptureWindow::capture() {
    if (state_ != State::Recording || pending_ || movingRegion_) return;
    if (!screen_ || screen_->geometry() != screenGeometry_) { failCapture(QStringLiteral("显示器配置已改变。")); return; }
    if (elapsed() >= 60000) { stop(); return; }
    pending_ = true; requestEpoch_ = epoch_; requestTime_ = elapsed(); lastRequest_ = clock_.elapsed();
    if(captureRequest_) captureRequest_();
    else if(!logicalRegion_.isEmpty())
        router_.captureRegion(output_,logicalRegion_.translated(screenGeometry_.topLeft()),screenGeometry_,CallerWindowPolicy::Include,recordingMp4_ && recordingVideoOptions_.cursor);
    else router_.capture(output_,CallerWindowPolicy::Include,recordingMp4_ && recordingVideoOptions_.cursor);
}
void LinuxGifCaptureWindow::toggle() {
    if (state_ == State::Ready) { begin(); return; }
    if (state_ == State::Recording) {
        pausedAt_ = clock_.elapsed(); state_ = State::Paused; ++epoch_; frameTimer_.stop();
        if(recordingMp4_ && recordingVideoOptions_.system_audio) audioRecorder_.setPaused(true);
    } else if (state_ == State::Paused) {
        pausedTotal_ += clock_.elapsed() - pausedAt_; state_ = State::Recording; ++epoch_;
        if(recordingMp4_ && recordingVideoOptions_.system_audio) audioRecorder_.setPaused(false);
        // 在途旧请求会自行结束并重启采集，不调用 stop() 导致回调消失。
        // Let an old request drain and reschedule rather than cancelling its callback.
        if (!pending_) capture();
    }
    updateControls();
}
void LinuxGifCaptureWindow::stop() {
    if (state_ == State::Countdown) { cancel(); return; }
    if (state_ != State::Recording && state_ != State::Paused) {
        if (preview_) { preview_->show(); preview_->raise(); }
        return;
    }
    const qint64 duration = elapsed(); state_ = State::Finishing; movingRegion_=false; ++epoch_;
    frameTimer_.stop(); uiTimer_.stop(); router_.stop();
    if (drawing_) {
        drawing_->activateRecordingTool(QStringLiteral("cursor"));
        drawing_->hide();
    }
    updateControls();
    showFinishingProgress(audioReady_ ? QStringLiteral("正在整理录制帧…") : QStringLiteral("正在整理录制帧和音频…"), -1);
    if(recordingMp4_ && recordingVideoOptions_.system_audio) audioRecorder_.finish();
    frames_.finish(duration);
}
void LinuxGifCaptureWindow::cancel() {
    // 导出期间关闭预览时由预览的交互约束负责阻止误删；退出应用仍会清理资源。
    // Preview guards closing during export; application shutdown still owns cleanup.
    if (frames_.exporting()) return;
    hideFinishingProgress();
    audioRecorder_.cancel();
    state_ = State::Finishing; movingRegion_=false; ++epoch_; frameTimer_.stop(); uiTimer_.stop(); countdownTimer_.stop(); router_.stop();
    if (overlay_) overlay_->hide(); if (fallback_) fallback_->hide(); if (preview_) preview_->hide();
    emit controlsChanged({}, {}); deleteLater();
}
void LinuxGifCaptureWindow::failCapture(const QString &reason) {
    captureError_=reason;
    stop();
    // 错误统一在写盘排空后显示一次；stop 可同步触发空录制预览，不再叠加第二个模态窗口。
    // Finalization reports the error once, after writes drain; stop may synchronously finish an empty recording.
}
void LinuxGifCaptureWindow::finishRecording() {
    if(state_!=State::Finishing || !framesReady_ || !audioReady_ || frames_.exporting()) return;
    if(frames_.frames().isEmpty()) {
        hideFinishingProgress();
        QMessageBox::warning(nullptr,QStringLiteral("录屏"),captureError_.isEmpty() ? QStringLiteral("没有录到画面，请重新开始录制。") : captureError_);
        cancel(); return;
    }
    QDir output(settings_.saveDirectory());
    if (!output.mkpath(".")) {
        hideFinishingProgress();
        QMessageBox::warning(nullptr, QStringLiteral("录屏"), QStringLiteral("无法创建录制保存目录。"));
        cancel();
        return;
    }
    // GIF 与 MP4 只在编码器分流，保存、文件名和完成入口完全一致，不再弹 GIF 二级确认。
    // Only encoding branches by format; naming, saving and completion are shared without a GIF confirmation.
    const auto name=QStringLiteral("录屏_%1_%2.%3").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"),
        QUuid::createUuid().toString(QUuid::Id128).left(8),recordingMp4_ ? QStringLiteral("mp4") : QStringLiteral("gif"));
    const bool started=recordingMp4_ ? frames_.exportMp4(output.filePath(name),recordingVideoOptions_,audioPath_)
        : frames_.exportGif(output.filePath(name),0,int(frames_.frames().size())-1);
    if(!started) cancel();
}
void LinuxGifCaptureWindow::showFinishingProgress(const QString &message, int percent) {
    auto *host = overlay_ ? overlay_.data() : fallback_.data();
    if (!host) return;
    const QString text = percent < 0 ? message : QStringLiteral("%1 %2%").arg(message).arg(percent);
    if (!finishingProgress_) {
        // 复用原版 WaitBar；先停止采集再显示，以免把进度面板录进视频。
        // Reuse the original WaitBar, shown only after capture has stopped.
        auto *panel = new QFrame(host);
        finishingProgress_ = panel;
        panel->setObjectName("recordingFinishingProgress");
        panel->setAttribute(Qt::WA_TransparentForMouseEvents);
        panel->setStyleSheet(QStringLiteral("QFrame#recordingFinishingProgress { background: #20252c; border: 1px solid #465365; border-radius: 10px; }"
                                           "QFrame#recordingFinishingProgress QLabel { color: #f1f5f9; background: transparent; border: none; }"));
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(14, 9, 14, 11);
        layout->setSpacing(6);
        auto *label = new QLabel(text, panel);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
        const auto available = screen_ ? screen_->availableGeometry() : host->rect();
        auto *bar = new jietuba::WaitBar(panel, QSize(std::clamp(available.width() - 48, 80, 280), 10));
        bar->setAccessibleName(QStringLiteral("录制处理进度"));
        layout->addWidget(bar, 0, Qt::AlignHCenter);

        toolbar_->hide();
        if (drawing_) drawing_->hide();
        panel->adjustSize();
        if (overlay_) {
            auto *surface = static_cast<RecordingSurface *>(host);
            surface->showCropOutline = false;
            surface->drawingActive = false;
            const auto local = screen_ ? available.translated(-screenGeometry_.topLeft()) : available;
            panel->move(local.x() + (local.width() - panel->width()) / 2, std::max(local.top(), local.bottom() - panel->height() - 23));
        } else {
            static_cast<QVBoxLayout *>(host->layout())->addWidget(panel, 0, Qt::AlignHCenter);
            host->resize(panel->size());
            host->move(available.center().x() - host->width() / 2, std::max(available.top(), available.bottom() - host->height() - 23));
        }
        panel->show();
        updateControlWindowVisibility();
        if (overlay_) {
            auto *surface = static_cast<RecordingSurface *>(host);
            surface->refreshMask();
            // 编码期间不再绘制选框，立即清空已提交的旧边框缓冲。
            // Clear the previously submitted crop outline immediately before encoding.
            surface->repaint();
        }
    }
    finishingProgress_->findChild<QLabel *>()->setText(text);
    finishingProgress_->findChild<jietuba::WaitBar *>()->set_progress(percent, percent < 0 ? 0 : 100);
}
void LinuxGifCaptureWindow::hideFinishingProgress() {
    delete finishingProgress_.data();
}
void LinuxGifCaptureWindow::showRecordingResult(const QString &path) {
    hideFinishingProgress();
    resultPath_=path; state_=State::Preview; emit controlsChanged({}, {});
    if(drawing_) {drawing_->activateRecordingTool("cursor"); drawing_->hide();}
    toolbar_->showRecordingResult(path);
    toolbar_->show();
    createResultPreview();
    if(!captureError_.isEmpty()) QMessageBox::warning(nullptr,QStringLiteral("录制已完成"),std::exchange(captureError_,{}));
}
void LinuxGifCaptureWindow::createResultPreview() {
    const auto *gifFrames = recordingMp4_ ? nullptr : &frames_;
    if(auto *surface=dynamic_cast<RecordingSurface *>(overlay_.data())) {
        // 结束界面独立于录制选区位置：预览与图标条一起放到录制屏幕中央稍下方。
        // Completion placement is independent of the crop: center the preview/icon row slightly lower.
        const QRect available=(screen_ ? screen_->availableGeometry().translated(-screenGeometry_.topLeft()) : surface->rect()).intersected(surface->rect());
        const auto placement=LinuxRecordingResultPreview::resultLayout(available,logicalCaptureRegion().size(),toolbar_->size());
        preview_=new LinuxRecordingResultPreview(resultPath_,surface,gifFrames); preview_->setGeometry(placement.preview); preview_->show();
        surface->showCropOutline=false; surface->drawingActive=false; surface->controls=placement.toolbar;
        toolbar_->setGeometry(placement.toolbar); surface->refreshMask(); surface->update();
    } else if(fallback_) {
        preview_=new LinuxRecordingResultPreview(resultPath_,fallback_,gifFrames); preview_->setMinimumSize(240,160);
        static_cast<QVBoxLayout *>(fallback_->layout())->insertWidget(0,preview_,1);
        fallback_->resize(std::max(600,toolbar_->sizeHint().width()+24),400);
        if(screen_) {
            const auto placement=LinuxRecordingResultPreview::resultLayout(screen_->availableGeometry(),QSize(600,300),toolbar_->size());
            fallback_->move(placement.preview.topLeft());
        }
    }
    updateControlWindowVisibility();
    if(preview_) connect(preview_,&LinuxRecordingResultPreview::playingChanged,toolbar_,&LinuxGifToolbar::setResultPlaying);
}
void LinuxGifCaptureWindow::toggleResultPreview() {
    if(state_!=State::Preview) return;
    if(preview_) {preview_->togglePlayback(); return;}
}
void LinuxGifCaptureWindow::rerecord() {
    if(state_!=State::Preview || frames_.exporting()) return;
    // 先停止播放，避免重置帧缓存期间预览继续取帧。
    // Pause the preview before resetting its frame store.
    if (preview_) preview_->pausePlayback();
    // 只删除当前会话生成的结果，不扫描保存目录，也不触碰其他已保存的录制。
    // Delete only this session's generated result, never other saved recordings in the directory.
    if(QFileInfo::exists(resultPath_) && !QFile::remove(resultPath_)) {
        toolbar_->setResultMessage(QStringLiteral("删除录制失败，请检查权限")); return;
    }
    if (!frames_.reset()) {
        toolbar_->setResultMessage(QStringLiteral("无法清理录制缓存")); return;
    }
    delete preview_;
    audioRecorder_.cancel(); audioPath_.clear(); captureError_.clear(); resultPath_.clear(); framesReady_=false; audioReady_=false;
    recordingMp4_=false; pending_=false; pausedTotal_=pausedAt_=0; clock_.invalidate(); ++epoch_; state_=State::Ready;
    if(drawing_) {drawing_->clearRecording(); drawing_->show();}
    toolbar_->showRecordingControls();
    if (overlay_) positionRecordingControls();
    else if(fallback_) fallback_->adjustSize();
    updateControlWindowVisibility();
    updateControls();
}
void LinuxGifCaptureWindow::updateControls() {
    const bool running = state_ == State::Recording || state_ == State::Paused;
    const bool busy = state_ == State::Countdown || state_ == State::Finishing || state_ == State::Preview;
    const qint64 ms = running ? elapsed() : 0;
    const QString time = state_ == State::Countdown ? QStringLiteral("倒计时 %1").arg(std::max(0, countdown_))
        : QStringLiteral("%1:%2").arg(ms / 60000, 2, 10, QLatin1Char('0')).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
    if (toolbar_) toolbar_->setState(time, running, state_ == State::Paused, busy);
    synchronizeDrawing();
    const bool mp4 = state_ == State::Ready ? toolbar_ && toolbar_->recordingMode() == "mp4" : recordingMp4_;
    const QString format = mp4 ? QStringLiteral("MP4") : QStringLiteral("GIF");
    QString action;
    if (state_ == State::Ready) action = QStringLiteral("开始 %1 录制").arg(format);
    else if (state_ == State::Recording) action = QStringLiteral("暂停 %1 录制").arg(format);
    else if (state_ == State::Paused) action = QStringLiteral("继续 %1 录制").arg(format);
    emit controlsChanged(action, running ? QStringLiteral("停止 %1 录制").arg(format) : QString());
}
bool LinuxGifCaptureWindow::eventFilter(QObject *watched, QEvent *event) {
    // Qt 原生 tooltip 是单独窗口，可能越过工具栏伸进录制区；录制期间禁止它。
    // Native tooltips can extend into the crop; suppress them while capturing.
    if (event->type() == QEvent::ToolTip && (state_ == State::Recording || state_ == State::Paused)) {
        const auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && toolbar_ && (widget == toolbar_ || toolbar_->isAncestorOf(widget))) return true;
    }
    return QObject::eventFilter(watched, event);
}

