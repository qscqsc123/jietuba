#include "quick_translation_window.h"
#include "translation_result_window.h"
#include "linux_settings.h"
#include "pointer_locator.h"
#include "speech_dispatcher.h"
#include "core/resource_manager.h"
#include "wayland_surface_support.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>
#include <QTimer>
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
#include <QTextToSpeech>
#endif
#include <algorithm>
namespace {
/**
 * 固定大小的透明输出宿主：只让翻译面板范围接收鼠标，透明区域穿透。
 * 与贴图一致，原生输入区域和绘制缓冲分开；移动子面板后清除旧区域，避免边框残影。
 * Fixed transparent output host. Only the panel receives input; native input masks never clip
 * painting. Child moves remain synchronous and repaint old bounds without border trails.
 */
class TranslationSurface final : public QWidget {
public:
    TranslationSurface(QScreen *screen,QWidget *owner) {
        setObjectName("linuxTranslationSurface"); setWindowTitle(QStringLiteral("截图吧 — 翻译"));
        setWindowFlags(Qt::Window|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_QuitOnClose,false);
        resize(screen->geometry().size()); winId(); windowHandle()->setScreen(screen);
#ifdef JIETUBA_HAS_LAYER_SHELL
        if(LinuxWayland::supportsLayerShell()) if(auto *layer=LayerShellQt::Window::get(windowHandle())) {
            layer->setScope("jietuba-quick-translation"); layer->setScreen(screen); layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)|LayerShellQt::Window::AnchorBottom|LayerShellQt::Window::AnchorLeft|LayerShellQt::Window::AnchorRight);
            layer->setDesiredSize(size()); layer->setExclusiveZone(-1);
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        }
#endif
        move(screen->geometry().topLeft());
        connect(screen,&QScreen::geometryChanged,owner,&QWidget::close);
        connect(screen,&QObject::destroyed,owner,&QWidget::close);
    }
    void setPanel(const QRect &panel) {
        const auto input=QRegion(panel).intersected(rect());
        if(windowHandle()) windowHandle()->setMask(input.isEmpty() ? QRegion(QRect(-2,-2,1,1)) : input);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setCompositionMode(QPainter::CompositionMode_Source); painter.fillRect(rect(),Qt::transparent);
    }
};
}
LinuxQuickTranslationWindow::LinuxQuickTranslationWindow(LinuxSettings &settings):settings_(settings),client_(this) {
    setObjectName("linuxQuickTranslation"); setWindowTitle(QStringLiteral("截图吧 — 翻译"));
    setWindowFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_TranslucentBackground);
    auto popupFont=font(); popupFont.setPixelSize(16); setFont(popupFont); resize(440,150);
    setStyleSheet("#linuxQuickTranslation {background:transparent;border:none;}"
        "QLabel {color:#66717c;font-size:13px;}"
        "QLabel#quickTranslateStatus {font-size:12px;} QLabel#quickTranslateStatus[error=\"true\"] {color:#b54949;}"
        "QPlainTextEdit {background:white;color:#222;border:1px solid #b8c1cd;border-radius:7px;padding:8px;}"
        "QPlainTextEdit:focus {border:2px solid #1682d9;}"
        "QPushButton {font-size:13px;color:#187cc5;background:transparent;border:1px solid transparent;border-radius:6px;padding:4px 6px;}"
        "QPushButton:hover {background:#e1eaf2;} QPushButton:pressed {background:#c7d8e7;}"
        "QPushButton:disabled {color:#9aa3ac;}"
        "QPushButton#quickTranslateSubmit {background:#e0effb;color:#126bbb;}"
        "QPushButton#quickTranslateSubmit:hover {background:#c8e2f7;}"
        "QPushButton#quickTranslateSubmit:pressed, QPushButton[feedback=\"true\"] {background:#b5d4ec;}"
        "QPushButton[speaking=\"true\"] {background:#c8e2f7;}"
        "QPushButton#quickTranslateSubmit[feedback=\"true\"], QPushButton#quickTranslateSpeak[feedback=\"true\"] {background:#b5d4ec;}"
        "QComboBox {font-size:14px;color:#207548;background:#e3eee9;border:none;border-radius:6px;padding:4px;}"
        "QComboBox::drop-down {width:0;border:none;background:transparent;} QComboBox::down-arrow {image:none;}"
        "QComboBox QAbstractItemView {background:white;color:#222;selection-background-color:#d5e9f8;}");
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(14,10,14,10); layout->setSpacing(8);
    header_=new QWidget(this); header_->setObjectName("quickTranslateDragArea"); header_->setCursor(Qt::OpenHandCursor); header_->installEventFilter(this);
    auto *top=new QHBoxLayout(header_); top->setContentsMargins(0,0,0,0); top->setSpacing(8);
    provider_=new QComboBox(header_); provider_->setObjectName("quickTranslateProvider");
    for(const auto &item:QList<QPair<QString,QString>>{{"Google","google"},{QStringLiteral("有道"),"youdao"},{"DeepL Free","deepl_free"},{"DeepL Pro","deepl_pro"},{"LibreTranslate","libretranslate"}}) provider_->addItem(item.first,item.second);
    provider_->setCurrentIndex(std::max(0,provider_->findData(settings_.translationProvider()))); top->addWidget(provider_);
    // 始终占据头部剩余空间，状态变化只改文字；长错误省略显示，悬停可读完整原因。
    // Reserve header space permanently. Status changes never resize the popup; long errors use a tooltip.
    status_=new QLabel(header_); status_->setObjectName("quickTranslateStatus"); status_->setFixedHeight(24);
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed); status_->setAlignment(Qt::AlignCenter);
    status_->setAttribute(Qt::WA_TransparentForMouseEvents); top->addWidget(status_,1);
    auto *close=new QPushButton(QStringLiteral("×"),header_); close->setToolTip(QStringLiteral("关闭（Esc）")); close->setObjectName("quickTranslateClose");
    close->setStyleSheet("font-size:20px;color:#78828b;padding:0;"); close->setFixedSize(26,26); top->addWidget(close); layout->addWidget(header_);
    input_=new QPlainTextEdit(this); input_->setObjectName("quickTranslateInput"); input_->setPlaceholderText(QStringLiteral("输入要翻译的文字…")); input_->setFixedHeight(56); input_->installEventFilter(this); layout->addWidget(input_);
    auto *bottom=new QHBoxLayout; bottom->setSpacing(6); bottom->addWidget(new QLabel(QStringLiteral("目标语言："),this));
    target_=new QComboBox(this); target_->setObjectName("quickTranslateTarget");
    for(const auto &item:QList<QPair<QString,QString>>{{QStringLiteral("中文"),"zh"},{QStringLiteral("英语"),"en"},{QStringLiteral("日语"),"ja"},{QStringLiteral("韩语"),"ko"}}) target_->addItem(item.first,item.second);
    target_->setCurrentIndex(std::max(0,target_->findData(settings_.translationTarget()))); bottom->addWidget(target_); bottom->addStretch();
    // 朗读移到底栏，输入/输出不再为右侧小按钮让出一列，左右边界完全一致。
    // Keep speech in the action footer, leaving both editors aligned to the same full-width margins.
    speak_=new QPushButton(this); speak_->setObjectName("quickTranslateSpeak"); speak_->setAccessibleName(QStringLiteral("朗读原文"));
    speak_->setIcon(jietuba::ResourceManager::get_icon(jietuba::ResourceManager::get_icon_path(QStringLiteral("录制声音.svg")),20)); speak_->setIconSize(QSize(20,20)); speak_->setToolTip(QStringLiteral("朗读原文")); speak_->setFixedSize(30,30); bottom->addWidget(speak_);
    submit_=new QPushButton(QStringLiteral("翻译 ↵"),this); submit_->setObjectName("quickTranslateSubmit"); submit_->setFixedHeight(30); bottom->addWidget(submit_);
    auto *full=new QPushButton(QStringLiteral("打开完整窗口"),this); full->setObjectName("quickTranslateFull"); full->setFixedHeight(30); bottom->addWidget(full); layout->addLayout(bottom);
    output_=new QPlainTextEdit(this); output_->setObjectName("quickTranslateOutput"); output_->setReadOnly(true); output_->setFixedHeight(96); output_->hide(); layout->addWidget(output_);
    auto *copy=new QPushButton(QStringLiteral("复制译文"),this); copy->setObjectName("quickTranslateCopy"); copy->setFixedHeight(30); copy->hide(); layout->addWidget(copy,0,Qt::AlignRight);
    for(auto *button:{close,speak_,submit_,full,copy}) {button->setFocusPolicy(Qt::NoFocus); button->setCursor(Qt::PointingHandCursor);}
    connect(copy,&QPushButton::clicked,this,[this,copy] {QApplication::clipboard()->setText(output_->toPlainText()); flashButton(copy); setStatus(QStringLiteral("译文已复制"));});
    connect(close,&QPushButton::clicked,this,&QWidget::close);
    connect(full,&QPushButton::clicked,this,[this] {auto *window=new LinuxTranslationResultWindow(input_->toPlainText(),settings_); window->show(); this->close();});
    connect(provider_,&QComboBox::currentIndexChanged,this,[this] {settings_.setTranslationProvider(provider_->currentData().toString()); settings_.sync(); fitChoices();});
    connect(target_,&QComboBox::currentIndexChanged,this,[this] {settings_.setTranslationTarget(target_->currentData().toString()); settings_.sync(); fitChoices();});
    connect(submit_,&QPushButton::clicked,this,&LinuxQuickTranslationWindow::translate);
    connect(speak_,&QPushButton::clicked,this,&LinuxQuickTranslationWindow::speak);
    if(LinuxSpeechDispatcher::supported()) {
        dispatcher_=new LinuxSpeechDispatcher(this);
        connect(dispatcher_,&LinuxSpeechDispatcher::activeChanged,this,[this] {refreshSpeechState();});
    }
    connect(input_,&QPlainTextEdit::textChanged,this,[this] {
        submit_->setEnabled(!input_->toPlainText().trimmed().isEmpty() && !client_.isWorking()); refreshSpeechState();
    }); submit_->setEnabled(false); refreshSpeechState();
    connect(&client_,&LinuxTranslationClient::finished,this,[this,copy](const LinuxTranslationResult &result) {
        submit_->setEnabled(!input_->toPlainText().trimmed().isEmpty()); setStatus(result.error,!result.error.isEmpty());
        if(result.error.isEmpty()) {
            const bool expand=output_->isHidden(); output_->setPlainText(result.text); output_->show(); copy->show();
            if(expand) fitContent(); // 只有首次真实译文展开结果区；加载和失败绝不改变窗口大小。
                                   // Expand only for the first real result, never for loading or errors.
        }
    });
    fitChoices(); fitContent();
}
LinuxQuickTranslationWindow::~LinuxQuickTranslationWindow() {
    // 当前对象仍是宿主的子对象，不能在其析构栈中同步删除父窗口。
    // Defer parent-surface deletion until this child finishes destruction.
    if(dispatcher_) {disconnect(dispatcher_,nullptr,this,nullptr); delete dispatcher_.data();}
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
    for(auto *speech:findChildren<QTextToSpeech *>()) {disconnect(speech,nullptr,this,nullptr); delete speech;}
#endif
    if(surface_) {surface_->hide(); surface_->deleteLater();}
}
QRect LinuxQuickTranslationWindow::placement(const QPoint &cursor,const QRect &available,const QSize &size) {
    if(available.isEmpty() || size.isEmpty()) return {};
    const QSize bounded=size.boundedTo(available.size());
    int x=cursor.x()+12,y=cursor.y()+16;
    if(x+bounded.width()>available.right()+1) x=cursor.x()-bounded.width()-12;
    if(y+bounded.height()>available.bottom()+1) y=cursor.y()-bounded.height()-16;
    return {QPoint(std::clamp(x,available.left(),available.right()+1-bounded.width()),std::clamp(y,available.top(),available.bottom()+1-bounded.height())),bounded};
}
void LinuxQuickTranslationWindow::showNearCursor() {
    if(locator_) return;
    endDrag(); hide(); if(surface_) surface_->hide();
    locator_=new LinuxPointerLocator(this); locator_->locate([this](QScreen *screen,const QPoint &position) {locator_=nullptr; showAt(screen,position);});
}
void LinuxQuickTranslationWindow::showAt(QScreen *screen,const QPoint &cursor) {
    anchor_=cursor; outputScreen_=screen; manualPosition_=false;
    if(!outputScreen_) return;
    const bool wayland=QGuiApplication::platformName().startsWith("wayland");
    if(LinuxWayland::supportsLayerShell() || !wayland) {
        if(!surface_ || surface_->windowHandle()->screen()!=screen) {
            auto *old=surface_.data(); auto *surface=new TranslationSurface(screen,this); surface_=surface;
            setParent(surface,Qt::Widget); surface->setFocusProxy(input_);
            if(old) {old->hide(); old->deleteLater();}
        }
        setMaximumSize(screen->availableGeometry().size()); fitContent(); show(); surface_->show(); raise();
    } else {
        if(surface_) {auto *old=surface_.data(); surface_=nullptr; setParent(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint); old->hide(); old->deleteLater();}
        setMaximumSize(screen->availableGeometry().size()); fitContent(); winId(); windowHandle()->setScreen(screen); place(); show();
    }
    activateWindow(); input_->setFocus();
}
void LinuxQuickTranslationWindow::fitChoices() {
    for(auto *choice:{provider_,target_}) {choice->ensurePolished(); choice->setFixedWidth(choice->fontMetrics().horizontalAdvance(choice->currentText())+18); choice->setFixedHeight(26);}
    header_->layout()->activate(); refreshStatus();
}
void LinuxQuickTranslationWindow::fitContent() {
    layout()->activate(); const int width=outputScreen_ ? std::min(440,outputScreen_->availableGeometry().width()) : 440;
    resize(width,layout()->totalSizeHint().height()); if(surface_ || windowHandle()) place(); refreshStatus();
}
void LinuxQuickTranslationWindow::place() {
    if(!outputScreen_) return;
    const auto available=outputScreen_->availableGeometry();
    QRect target=manualPosition_ ? QRect(manualTopLeft_,size()) : placement(anchor_,available,size());
    target.setSize(target.size().boundedTo(available.size()));
    target.moveLeft(std::clamp(target.x(),available.left(),available.right()+1-target.width()));
    target.moveTop(std::clamp(target.y(),available.top(),available.bottom()+1-target.height()));
    if(surface_) {
        setGeometry(QRect(target.topLeft()-outputScreen_->geometry().topLeft(),target.size()));
        static_cast<TranslationSurface *>(surface_.data())->setPanel(geometry());
    } else {
        resize(target.size());
        // xdg-shell 的拖动交给合成器；不能用其不可查询的全局位置在状态更新后重新定位。
        // Compositor-owned xdg moves must not be overwritten by synthetic global coordinates.
        if(!manualPosition_) move(target.topLeft());
    }
}
void LinuxQuickTranslationWindow::setStatus(const QString &text,bool error) {
    statusText_=text; status_->setProperty("error",error); status_->style()->unpolish(status_); status_->style()->polish(status_);
    status_->setToolTip(text); header_->setToolTip(text); refreshStatus();
}
void LinuxQuickTranslationWindow::refreshStatus() {if(status_) status_->setText(status_->fontMetrics().elidedText(statusText_.section('\n',0,0),Qt::ElideRight,std::max(0,status_->width())));}
void LinuxQuickTranslationWindow::flashButton(QPushButton *button) {
    button->setProperty("feedback",true); button->style()->unpolish(button); button->style()->polish(button); button->update();
    QTimer::singleShot(160,button,[button] {button->setProperty("feedback",false); button->style()->unpolish(button); button->style()->polish(button); button->update();});
}
void LinuxQuickTranslationWindow::translate() {
    flashButton(submit_); if(client_.isWorking()) return;
    const auto text=input_->toPlainText(); if(text.trimmed().isEmpty()) {input_->setFocus(); return;}
    const LinuxTranslationConfig config{settings_.translationProvider(),settings_.translationApiKey(),settings_.translationEndpoint(),settings_.translationTarget(),settings_.googleApiKey(),settings_.youdaoAppId(),settings_.youdaoAppSecret()};
    const auto prepared=LinuxTranslationClient::prepare(text,config);
    setStatus(prepared.error.isEmpty() ? QStringLiteral("正在翻译…") : prepared.error,!prepared.error.isEmpty());
    if(prepared.error.isEmpty()) {submit_->setEnabled(false); client_.translate(text,config);}
}
void LinuxQuickTranslationWindow::speechUnavailable(const QString &details) {
    // 简明中文保留在固定状态位；完整依赖说明与原始诊断可在头部悬停查看，不撑大窗口。
    // Keep the reserved header concise; retain setup guidance and raw diagnostics in its tooltip.
    setStatus(QStringLiteral("系统朗读服务未就绪，请检查声音配置。\n"
                             "请确认系统语音服务已启动，并提供当前文字语言的声音。")
                  +(details.isEmpty() ? QString() : QStringLiteral("\n\n诊断信息：\n")+details),true);
}
void LinuxQuickTranslationWindow::refreshSpeechState() {
    bool active=dispatcher_ && dispatcher_->isSpeaking();
    bool supported=LinuxSpeechDispatcher::supported();
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
    supported=true;
    if(auto *speech=findChild<QTextToSpeech *>()) active|=speech->state()==QTextToSpeech::Speaking || speech->state()==QTextToSpeech::Paused;
#endif
    speak_->setEnabled(supported && (active || !input_->toPlainText().trimmed().isEmpty()));
    speak_->setProperty("speaking",active);
    speak_->setToolTip(active ? QStringLiteral("停止朗读") : (supported ? QStringLiteral("朗读原文") : QStringLiteral("此构建未启用系统语音支持。")));
    speak_->style()->unpolish(speak_); speak_->style()->polish(speak_); speak_->update();
}
void LinuxQuickTranslationWindow::speak() {
    flashButton(speak_);
    if(dispatcher_ && dispatcher_->isSpeaking()) {dispatcher_->stop(); return;}
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
    if(auto *speech=findChild<QTextToSpeech *>(); speech && (speech->state()==QTextToSpeech::Speaking || speech->state()==QTextToSpeech::Paused)) {speech->stop(); return;}
#endif
    if(dispatcher_ && dispatcher_->speak(input_->toPlainText())) {if(!client_.isWorking()) setStatus({}); return;}

#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
    auto *speech=findChild<QTextToSpeech *>();
    if(speech && (speech->state()==QTextToSpeech::Speaking || speech->state()==QTextToSpeech::Paused)) {speech->stop(); return;}
    // 失败对象不能一直复用：用户补装依赖/启动服务后，下一次点击必须重新初始化。
    // Recreate failed objects so installing dependencies or starting the service can be retried.
    if(speech && speech->state()==QTextToSpeech::Error) {delete speech; speech=nullptr;}
    if(!speech) {
        auto available=QTextToSpeech::availableEngines();
        available.removeIf([](const QString &engine) {return engine.compare(QStringLiteral("mock"),Qt::CaseInsensitive)==0;});
        QStringList candidates;
        for(const auto &preferred:{QStringLiteral("speechd"),QStringLiteral("flite")})
            if(available.removeOne(preferred)) candidates.append(preferred);
        candidates.append(available);
        if(dispatcher_) candidates.removeAll("speechd"); // 原生失败后只尝试不同实现 / Try a distinct fallback, not the same Qt locale path.
        QStringList errors;
        if(dispatcher_) errors.append(QStringLiteral("Speech Dispatcher: ")+dispatcher_->error());
        // Qt 的 mock 插件只用于测试，不能把它当作系统中真的存在可播放的声音。
        // Explicitly prefer real Linux backends, excluding Qt's test-only mock engine.
        for(const auto &engine:candidates) {
            auto *candidate=new QTextToSpeech(engine,this);
            if(candidate->state()!=QTextToSpeech::Error && !candidate->availableVoices().isEmpty()) {speech=candidate; break;}
            errors.append(engine+QStringLiteral(": ")+(candidate->errorString().isEmpty() ? QStringLiteral("没有可用的声音，请检查后端服务及语音包。") : candidate->errorString()));
            delete candidate;
        }
        if(!speech) {speechUnavailable(errors.isEmpty() ? QStringLiteral("未找到可用的系统语音引擎。") : errors.join('\n')); return;}
        speech->setObjectName("quickTranslateSpeech");
        connect(speech,&QTextToSpeech::errorOccurred,this,[this](QTextToSpeech::ErrorReason reason,const QString &error) {
            if(reason==QTextToSpeech::ErrorReason::Playback) setStatus(QStringLiteral("朗读音频输出失败，请检查输出设备。\n")+error,true);
            else if(reason==QTextToSpeech::ErrorReason::Input) setStatus(QStringLiteral("当前文字无法朗读。\n")+error,true);
            else speechUnavailable(error);
        });
        connect(speech,&QTextToSpeech::stateChanged,this,[this] {refreshSpeechState();});
    }
    if(!client_.isWorking()) setStatus({});
    speech->say(input_->toPlainText());
#else
    speechUnavailable(dispatcher_ ? dispatcher_->error() : QStringLiteral("此构建未启用可用语音后端。"));
#endif
}
bool LinuxQuickTranslationWindow::beginDrag(const QPointF &position) {
    if(!surface_ && QGuiApplication::platformName().startsWith("wayland")) {
        if(windowHandle() && windowHandle()->startSystemMove()) {manualPosition_=true; return true;}
        return false;
    }
    // 固定输出宿主内只改变子控件位置，局部坐标可重复计算，不累积原生边距的异步误差。
    // Move only the child in a fixed output host; local geometry avoids native-margin feedback and jumps.
    manualTopLeft_=(outputScreen_ ? outputScreen_->geometry().topLeft() : QPoint())+pos();
    dragging_=true; dragOffset_=position; manualPosition_=true; header_->setCursor(Qt::ClosedHandCursor); return true;
}
void LinuxQuickTranslationWindow::dragTo(const QPointF &position) {
    if(!dragging_ || !outputScreen_) return;
    manualTopLeft_=outputScreen_->geometry().topLeft()+pos()+(position-dragOffset_).toPoint(); place();
}
void LinuxQuickTranslationWindow::endDrag() {dragging_=false; if(header_) header_->setCursor(Qt::OpenHandCursor);}
bool LinuxQuickTranslationWindow::eventFilter(QObject *object,QEvent *event) {
    if(object==header_) {
        auto *mouse=dynamic_cast<QMouseEvent *>(event);
        if(mouse && event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton) {if(beginDrag(header_->mapTo(this,mouse->position()))) {event->accept(); return true;}}
        if(mouse && event->type()==QEvent::MouseMove && dragging_) {if(mouse->buttons() & Qt::LeftButton) dragTo(header_->mapTo(this,mouse->position())); else endDrag(); event->accept(); return true;}
        if(mouse && event->type()==QEvent::MouseButtonRelease && dragging_) {endDrag(); event->accept(); return true;}
    }
    if(event->type()==QEvent::KeyPress) {
        auto *key=static_cast<QKeyEvent *>(event);
        if(key->key()==Qt::Key_Escape) {close(); return true;}
        if((key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter) && key->modifiers()==Qt::NoModifier) {translate(); return true;}
    }
    return QWidget::eventFilter(object,event);
}
void LinuxQuickTranslationWindow::mousePressEvent(QMouseEvent *event) {if(event->button()==Qt::LeftButton && beginDrag(event->position())) event->accept(); else QWidget::mousePressEvent(event);}
void LinuxQuickTranslationWindow::mouseMoveEvent(QMouseEvent *event) {if(dragging_ && (event->buttons() & Qt::LeftButton)) {dragTo(event->position()); event->accept();} else {endDrag(); QWidget::mouseMoveEvent(event);}}
void LinuxQuickTranslationWindow::mouseReleaseEvent(QMouseEvent *event) {if(dragging_ && event->button()==Qt::LeftButton) {endDrag(); event->accept();} else QWidget::mouseReleaseEvent(event);}
void LinuxQuickTranslationWindow::closeEvent(QCloseEvent *event) {endDrag(); if(surface_) surface_->hide(); QWidget::closeEvent(event);}
void LinuxQuickTranslationWindow::focusOutEvent(QFocusEvent *event) {
    QWidget::focusOutEvent(event);
    if (settings_.closeTranslationOnClickOutside()) {
        close();
    }
}
void LinuxQuickTranslationWindow::resizeEvent(QResizeEvent *event) {QWidget::resizeEvent(event); refreshStatus();}
void LinuxQuickTranslationWindow::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor("#d2d5db")); painter.setBrush(QColor("#f5f5f8")); painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),12,12);
}
