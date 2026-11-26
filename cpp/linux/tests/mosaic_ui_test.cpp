#define main preview_main
#include "../ui_preview.cpp"
#undef main
#include <QTest>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QLibraryInfo>
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
#include <QTextToSpeech>
#endif
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDBusConnection>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextCursor>
#include <QStackedWidget>
#include <QTemporaryDir>
#include "../pin_toolbar.h"
#include <QTimer>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QKeySequenceEdit>
#include "ui/inapp_key_edit.h"
#include "../shortcut_edit.h"
#include "../pointer_locator.h"
#include "../speech_dispatcher.h"
#include "core/resource_manager.h"
#include "ui/fluent_lite/segmented.h"
#include "canvas/items/smart_erase_fill.h"
#include "../scroll_capture_overlay.h"
#include "stitch/scroll_toolbar.h"
#include "gif/_widgets.h"
#include "gif/range_slider.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/cards.h"
#include "core/save.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/toolbar_layout_dialog.h"
#include <cstdio>
#include "capture_reentry_case.h"
/**
 * 私有总线上的文件管理器，验证真实接口与工具栏异步关闭，不启动真实桌面程序。
 * File manager on a private bus verifies the real interface and asynchronous result close without desktop apps.
 */
class RecordingFolderProbe final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.freedesktop.FileManager1")
public:
    QStringList urls;
public slots:
    void ShowItems(const QStringList &items,const QString &) {urls=items;}
};
/**
 * 在离屏画布上操作马赛克 UI，检查导出像素、样式、移动及撤销重做。
 * Exercises mosaic drawing, exported pixels, edits and undo using an offscreen canvas.
 */
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    // 原生工具窗口只创建自身表面，禁止 Qt 将同级画布也隐式提升为原生子窗口。
    // Native siblings paint outside their parent's opacity effect and keep independent input regions.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    if (app.arguments().contains("--capture-reentry"))
        return capture_reentry_test::run(app, app.arguments().contains("--virtual-kwin"));
    {
        QTemporaryDir output;
        QImage longImage(100, 2400, QImage::Format_RGB32);
        longImage.fill(Qt::white);
        const auto path = output.filePath("long.pdf");
        if (!jietuba::SaveService::save_qimage_to_path(longImage, path)) return 1;
        QFile pdf(path);
        if (!pdf.open(QIODevice::ReadOnly) || !pdf.read(8).startsWith("%PDF-")) return 1;
    }
    if(app.arguments().contains("--popup-layout-only")) {
        const auto fail=[](const char *reason) {std::fprintf(stderr,"Popup/DPI layout: %s\n",reason); return 1;};
        // 输出坐标含负原点与不同缩放：enter 已是逻辑坐标；窗口在边缘应翻转而非跑去居中。
        // Output-local enter positions are logical already; flip the popup near edges, including negative origins.
        if(LinuxPointerLocator::globalPosition(QRect(-1920,-100,1920,1080),QPointF(230.4,180.4))!=QPoint(-1690,80)) return fail("output-local pointer mapping differs");
        if(LinuxQuickTranslationWindow::placement(QPoint(500,400),QRect(0,0,1920,1080),QSize(480,176)).topLeft()!=QPoint(512,416)
            || LinuxQuickTranslationWindow::placement(QPoint(1850,1000),QRect(0,0,1920,1080),QSize(480,176)).topLeft()!=QPoint(1358,808)
            || LinuxQuickTranslationWindow::placement(QPoint(-1000,400),QRect(-1920,0,1920,1080),QSize(480,176)).topLeft()!=QPoint(-988,416)
            || !QRect(0,0,320,240).contains(LinuxQuickTranslationWindow::placement(QPoint(315,235),QRect(0,0,320,240),QSize(480,176)))) return fail("cursor adjacency or edge flip differs");
        QTemporaryDir dir; LinuxSettings settings(dir.filePath("popup-layout.ini")); settings.setTranslationProvider("google");
        jietuba::get_ui_theme()->init(nullptr,&app); jietuba::get_ui_theme()->set_mode("light",false);
        auto *popup=new LinuxQuickTranslationWindow(settings); popup->showNearCursor(); QTest::qWait(20);
        auto *input=popup->findChild<QPlainTextEdit *>("quickTranslateInput");
        auto *provider=popup->findChild<QComboBox *>("quickTranslateProvider");
        if(!popup->isVisible() || popup->width()>440 || popup->height()>170 || input->height()!=56 || provider->width()>100
            || !app.primaryScreen()->availableGeometry().contains(popup->geometry())) return fail("initial popup proportions or bounds differ");
        const auto render=qgetenv("JIETUBA_LAYOUT_TEST_RENDER");
        if(!render.isEmpty()) popup->grab().save(QString::fromLocal8Bit(render)+"-translation.png");
        auto *header=popup->findChild<QWidget *>("quickTranslateDragArea");
        auto *status=popup->findChild<QLabel *>("quickTranslateStatus");
        auto *submit=popup->findChild<QPushButton *>("quickTranslateSubmit");
        auto *speak=popup->findChild<QPushButton *>("quickTranslateSpeak");
        auto *host=popup->window();
        const QRect hostGeometry=host->geometry(),beforeDrag=popup->geometry();
        const QPoint grip=header->rect().center();
        const QPoint fixedPointer=header->mapTo(host,grip)+QPoint(17,11);
        QTest::mousePress(header,Qt::LeftButton,Qt::NoModifier,grip);
        const auto movePointer=[&] {
            const auto local=header->mapFrom(host,fixedPointer);
            QMouseEvent move(QEvent::MouseMove,QPointF(local),QPointF(fixedPointer),QPointF(host->mapToGlobal(fixedPointer)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QCoreApplication::sendEvent(header,&move);
        };
        movePointer(); const auto afterDrag=popup->geometry(); movePointer();
        const auto outputArea=app.primaryScreen()->availableGeometry().translated(-app.primaryScreen()->geometry().topLeft());
        const QPoint expected(std::clamp(beforeDrag.x()+17,outputArea.left(),outputArea.right()+1-popup->width()),std::clamp(beforeDrag.y()+11,outputArea.top(),outputArea.bottom()+1-popup->height()));
        if(afterDrag.topLeft()!=expected || popup->geometry()!=afterDrag || host->geometry()!=hostGeometry) return fail("header drag jumped or moved the native host");
        QTest::mouseRelease(header,Qt::LeftButton,Qt::NoModifier,header->mapFrom(host,fixedPointer));
        if(!host->windowHandle()->mask().contains(popup->geometry().center())) return fail("drag left input mask at the old location");
        input->setPlainText("hello"); const auto idleSubmit=submit->grab().toImage().pixelColor(4,submit->height()/2);
        const auto stable=popup->geometry(); QTest::keyClick(input,Qt::Key_Return);
        if(popup->geometry()!=stable || status->isHidden() || status->text().isEmpty() || !submit->property("feedback").toBool()
            || submit->grab().toImage().pixelColor(4,submit->height()/2)==idleSubmit) return fail("Enter resized the popup or lacked pressed feedback");
        QTest::qWait(180);
        if(submit->property("feedback").toBool() || popup->geometry()!=stable) return fail("feedback remained active or changed geometry");
        if(speak->isEnabled()) {
            // 阻断 clicked，验证真实按压绘制但绝不在测试中启动用户的音频引擎。
            // Block clicked: exercise real pressed visuals without starting the user's speech engine.
            const QSignalBlocker quiet(speak);
            const auto idle=speak->grab().toImage().pixelColor(4,speak->height()/2);
            QTest::mousePress(speak,Qt::LeftButton);
            if(speak->grab().toImage().pixelColor(4,speak->height()/2)==idle) return fail("speech button lacked pressed feedback");
            QTest::mouseRelease(speak,Qt::LeftButton);
        }
        if(speak->y()!=submit->y() || input->x()!=14 || input->x()+input->width()!=popup->width()-14) return fail("footer actions or editor margins are misaligned");
#ifdef JIETUBA_HAS_TEXT_TO_SPEECH
        // 隔离插件目录，仅保留可选的 mock。绝不连接真实语音服务，验证缺依赖、排除 mock 和重试。
        // Isolate plugins to optional mock only: verify unavailable backends and retry without real audio.
        if(auto *native=popup->findChild<LinuxSpeechDispatcher *>()) native->setAutoSpawnEnabled(false);
        const auto oldSpeechAddress=qgetenv("SPEECHD_ADDRESS"),oldSpeechCommand=qgetenv("SPEECHD_CMD");
        const bool hadSpeechAddress=qEnvironmentVariableIsSet("SPEECHD_ADDRESS"),hadSpeechCommand=qEnvironmentVariableIsSet("SPEECHD_CMD");
        qputenv("SPEECHD_ADDRESS",("unix_socket:"+dir.filePath("no-speech.sock")).toUtf8()); qputenv("SPEECHD_CMD","/bin/false");
        const auto oldPluginPaths=QCoreApplication::libraryPaths();
        const auto pluginRoot=dir.filePath("speech-plugins"); QDir().mkpath(pluginRoot+"/texttospeech");
        const QDir installedSpeech(QLibraryInfo::path(QLibraryInfo::PluginsPath)+"/texttospeech");
        for(const auto &file:installedSpeech.entryList({"*mock*.so"},QDir::Files)) QFile::copy(installedSpeech.filePath(file),pluginRoot+"/texttospeech/"+file);
        QCoreApplication::setLibraryPaths({pluginRoot});
        auto isolated=QTextToSpeech::availableEngines(); isolated.removeAll("mock");
        if(!isolated.isEmpty()) return fail("speech fixture unexpectedly contains a real backend");
        QPointer<QTextToSpeech> broken=new QTextToSpeech("jietuba-missing-test-engine",popup);
        if(broken->state()!=QTextToSpeech::Error) return fail("invalid engine did not reproduce initialization failure");
        QTest::mouseClick(speak,Qt::LeftButton);
        if(broken || popup->findChild<QTextToSpeech *>() || popup->geometry()!=stable || !status->toolTip().contains(QStringLiteral("系统朗读服务未就绪"))) return fail("speech failure was not localized, recreated, or kept compact");
        QTest::mouseClick(speak,Qt::LeftButton);
        if(!speak->isEnabled() || popup->findChild<QTextToSpeech *>() || popup->geometry()!=stable) return fail("speech failure blocked retry or selected mock");
        QCoreApplication::setLibraryPaths(oldPluginPaths);
        if(hadSpeechAddress) qputenv("SPEECHD_ADDRESS",oldSpeechAddress); else qunsetenv("SPEECHD_ADDRESS");
        if(hadSpeechCommand) qputenv("SPEECHD_CMD",oldSpeechCommand); else qunsetenv("SPEECHD_CMD");
#endif
        QTest::keyClick(input,Qt::Key_Escape); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        LinuxSettingsWindow window(settings); window.showShortcutsPage(); QTest::qWait(20);
        auto *navigation=window.findChild<jietuba::NavigationInterface *>();
        if(!navigation || navigation->widget("capture")->iconSize()!=QSize(24,24)) return fail("sidebar icon metrics differ");
        const auto source=jietuba::ResourceManager::get_icon(jietuba::ResourceManager::get_icon_path(QStringLiteral("热键.svg"))).pixmap(64,64);
        const auto tinted=jietuba::tinted_icon(jietuba::ResourceManager::get_icon_path(QStringLiteral("热键.svg")),"#123456").pixmap(64,64);
        const auto area=[](const QPixmap &pixmap) {auto image=pixmap.toImage(); int count=0; for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) count+=image.pixelColor(x,y).alpha()>16; return count;};
        const auto originalArea=area(source),tintedArea=area(tinted);
        if(!originalArea || tintedArea<originalArea*.95 || tintedArea>originalArea*1.05 || source.devicePixelRatio()!=tinted.devicePixelRatio()) return fail("tint shrank the HiDPI glyph");
        std::fprintf(stderr,"UI layout DPR %.2f; icon alpha area %d/%d; glyph DPR %.2f/%.2f\n",app.primaryScreen()->devicePixelRatio(),tintedArea,originalArea,tinted.devicePixelRatio(),source.devicePixelRatio());
        if(!render.isEmpty()) {
            window.resize(1050,830); QTest::qWait(10); window.grab().save(QString::fromLocal8Bit(render)+"-settings.png");
            jietuba::get_ui_theme()->set_mode("dark",false); QTest::qWait(10); window.grab().save(QString::fromLocal8Bit(render)+"-settings-dark.png");
        }
        return 0;
    }
    {
        const auto fail=[](const char *reason) {std::fprintf(stderr,"Screen annotation/translation: %s\n",reason); return 1;};
        QTemporaryDir dir; LinuxSettings settings(dir.filePath("new-features.ini"));
        LinuxQuickTranslationWindow *popup=new LinuxQuickTranslationWindow(settings);
        popup->showNearCursor(); QTest::qWait(10);
        auto *input=popup->findChild<QPlainTextEdit *>("quickTranslateInput");
        auto *provider=popup->findChild<QComboBox *>("quickTranslateProvider");
        auto *target=popup->findChild<QComboBox *>("quickTranslateTarget");
        provider->setCurrentIndex(provider->findData("google")); target->setCurrentIndex(target->findData("ja"));
        if(settings.translationProvider()!="google" || settings.translationTarget()!="ja") return fail("quick translation choices were not stored");
        input->setPlainText("hello"); QTest::keyClick(input,Qt::Key_Return);
        if(popup->findChild<QLabel *>("quickTranslateStatus")->text().isEmpty()) return fail("missing API credentials did not produce a configuration error");
        if(!QRect(0,0,800,600).contains(LinuxQuickTranslationWindow::placement(QPoint(790,590),QRect(0,0,800,600),QSize(570,210)))) return fail("popup escaped output edges");
        // 私有回环 HTTP 服务验证真实输入→请求→译文显示，不访问互联网或用户密钥。
        // A loopback HTTP fixture verifies input/request/result without internet or user credentials.
        QTcpServer server; if(!server.listen(QHostAddress::LocalHost,0)) return fail("local translation fixture did not listen");
        int requests=0; bool responseError=false;
        QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
            auto *socket=server.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                QByteArray data=socket->property("request").toByteArray()+socket->readAll(); socket->setProperty("request",data);
                const auto end=data.indexOf("\r\n\r\n"); if(end<0) return;
                int length=0; for(auto header:data.left(end).split('\n')) if(header.trimmed().toLower().startsWith("content-length:")) length=header.mid(header.indexOf(':')+1).trimmed().toInt();
                if(data.size()<end+4+length || socket->property("answered").toBool()) return;
                socket->setProperty("answered",true); ++requests;
                const QByteArray body=responseError ? "{\"error\":\"fixture error\"}" : "{\"translatedText\":\"hello translated\"}";
                socket->write(QByteArray(responseError ? "HTTP/1.1 503 Service Unavailable" : "HTTP/1.1 200 OK")+"\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body); socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        settings.setTranslationEndpoint(QStringLiteral("http://127.0.0.1:%1/translate").arg(server.serverPort()));
        provider->setCurrentIndex(provider->findData("libretranslate"));
        if(requests) return fail("translation sent without explicit submit");
        const auto pendingGeometry=popup->geometry();
        QTest::keyClick(input,Qt::Key_Return);
        if(popup->geometry()!=pendingGeometry || popup->findChild<QLabel *>("quickTranslateStatus")->text()!=QStringLiteral("正在翻译…")) return fail("loading changed the reserved layout");
        QTest::keyClick(input,Qt::Key_Return); // 工作中重复 Enter 不应重新提交 / Repeated Enter must not restart an active request.
        for(int i=0;i<100 && popup->findChild<QPlainTextEdit *>("quickTranslateOutput")->toPlainText().isEmpty();++i) QTest::qWait(10);
        if(requests!=1 || popup->findChild<QPlainTextEdit *>("quickTranslateOutput")->toPlainText()!="hello translated") return fail("quick translation request/result flow failed");
        auto *out=popup->findChild<QPlainTextEdit *>("quickTranslateOutput");
        if(out->x()!=input->x() || out->width()!=input->width()) return fail("source/result editor edges differ");
        const auto resultGeometry=popup->geometry(); responseError=true;
        auto *submit=popup->findChild<QPushButton *>("quickTranslateSubmit"); QTest::mouseClick(submit,Qt::LeftButton);
        if(popup->geometry()!=resultGeometry) return fail("retry grew the popup while loading");
        for(int i=0;i<100 && !submit->isEnabled();++i) QTest::qWait(10);
        if(requests!=2 || !submit->isEnabled() || popup->geometry()!=resultGeometry || popup->findChild<QLabel *>("quickTranslateStatus")->text().isEmpty()) return fail("HTTP failure resized the popup or blocked retry");
        const auto popupRender=qgetenv("JIETUBA_NEW_FEATURES_TEST_RENDER");
        if(!popupRender.isEmpty()) popup->grab().save(QString::fromLocal8Bit(popupRender)+"-translation.png");
        popup->findChild<QPushButton *>("quickTranslateFull")->click();
        QPointer<LinuxTranslationResultWindow> full;
        for(auto *top:app.topLevelWidgets()) if(auto *window=qobject_cast<LinuxTranslationResultWindow *>(top)) full=window;
        if(!full || full->sourceText()!="hello" || full->isRecognizing()) return fail("full text window incorrectly started OCR");
        full->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        auto *overlay=new LinuxScreenAnnotation(app.primaryScreen(),settings);
        overlay->showPrepared(); QTest::qWait(10);
        auto *bar=overlay->findChild<jietuba::Toolbar *>("linuxAnnotationToolbar");
        auto *canvas=overlay->canvas(); auto *view=canvas->findChild<QGraphicsView *>();
        const QRect available=app.primaryScreen()->availableGeometry().translated(-app.primaryScreen()->geometry().topLeft());
        if(!bar || bar->geometry()!=LinuxScreenAnnotation::toolbarPlacement(available,bar->size())) return fail("annotation toolbar is not bottom-center with margin");
        if(!canvas->testAttribute(Qt::WA_TransparentForMouseEvents) || overlay->windowHandle()->mask().contains(QPoint(20,20))
            || !overlay->windowHandle()->mask().contains(bar->geometry().center())) return fail("annotation did not start with input restricted to controls");
        auto *board=bar->findChild<QPushButton *>("annotation_board"); board->click(); QTest::qWait(5);
        if(overlay->grab().toImage().pixelColor(20,overlay->height()/2)!=QColor(Qt::white)) return fail("whiteboard is not visible behind the canvas");
        board->click(); QTest::qWait(5);
        if(overlay->grab().toImage().pixelColor(20,overlay->height()/2).alpha()!=0) return fail("whiteboard left stale opaque pixels");
        bar->pen_btn->click(); QTest::qWait(5);
        if(canvas->recordingTool()!="pen" || canvas->testAttribute(Qt::WA_TransparentForMouseEvents) || !overlay->windowHandle()->mask().contains(QPoint(20,20))) return fail("pen did not enable desktop drawing");
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(50,50));
        QTest::mouseMove(view->viewport(),QPoint(160,100)); QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(160,100));
        if(canvas->recordingImage().isNull() || !canvas->recordingUndoStack()->canUndo()) return fail("screen pen did not produce undoable annotations");
        canvas->clearAnnotationsUndoable();
        if(!canvas->recordingImage().isNull()) return fail("clear left drawings behind");
        canvas->recordingUndoStack()->undo(); if(canvas->recordingImage().isNull()) return fail("clear was not undoable");
        // 点击动画独立于文档；消退后不能留下像素，也不能进入导出或撤销历史。
        // Click effects must decay without entering the document or its undo history.
        bar->findChild<QPushButton *>("annotation_cursor")->click();
        auto *effects=overlay->findChild<QWidget *>("annotationPointerEffects");
        const auto imageBefore=canvas->recordingImage();
        const int undoBefore=canvas->recordingUndoStack()->count();
        QTest::mousePress(overlay,Qt::LeftButton,Qt::NoModifier,QPoint(100,100));
        QTest::mouseRelease(overlay,Qt::LeftButton,Qt::NoModifier,QPoint(100,100));
        QTest::qWait(130);
        const auto rippleImage=effects->grab().toImage();
        QTest::qWait(400);
        if(rippleImage==effects->grab().toImage() || canvas->recordingImage()!=imageBefore
            || canvas->recordingUndoStack()->count()!=undoBefore) return fail("click ripple did not decay independently of annotations");
        bar->text_btn->click(); QTest::keyClick(view->viewport(),Qt::Key_Escape);
        if(!canvas->recordingTool().isEmpty()) return fail("Escape did not restore pass-through");
        if(overlay->windowHandle()->mask().contains(QPoint(100,100))) return fail("pointer effects blocked pass-through");
        const auto render=qgetenv("JIETUBA_NEW_FEATURES_TEST_RENDER");
        if(!render.isEmpty()) overlay->grab().save(QString::fromLocal8Bit(render)+"-annotation.png");
        bar->findChild<QPushButton *>("annotation_hide")->click();
        if(canvas->isVisible()) return fail("hide annotations did not hide the drawing");
        bar->findChild<QPushButton *>("annotation_hide")->click();
        if(!canvas->isVisible() || canvas->recordingImage().isNull()) return fail("show annotations lost the document");
        overlay->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    // 透明框选层不显示截图：验证透明像素、反向拖动、取消/全屏和分数缩放映射。
    // Validate an image-free selector: transparency, reverse drags, cancellation/fullscreen, and fractional DPI.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Live selection: %s\n",reason); return 1; };
        LinuxRegionSelector selector(app.primaryScreen());
        selector.showPrepared(); QTest::qWait(10);
        if (!selector.testAttribute(Qt::WA_TranslucentBackground)
            || selector.grab().toImage().pixelColor(10, selector.height()-10).alpha()!=0)
            return fail("unselected desktop is obscured");
        QSignalSpy selected(&selector, &LinuxRegionSelector::selected);
        QTest::mousePress(&selector, Qt::LeftButton, Qt::NoModifier, QPoint(200,160));
        QTest::mouseMove(&selector, QPoint(40,30));
        if(selector.grab().toImage().pixelColor(40,30).alpha()!=0)
            return fail("selector paints its border inside the requested crop");
        QTest::mouseRelease(&selector, Qt::LeftButton, Qt::NoModifier, QPoint(40,30));
        if (selected.count()!=1 || selected.first().first().toRectF()!=QRectF(40,30,160,130))
            return fail("reverse drag coordinates differ");
        QTest::keyClick(&selector, Qt::Key_Return);
        if (selected.count()!=1) return fail("completed selector emitted twice");
        selector.hide();
        // 撤场后的最后一帧必须完全透明，不能让合成器缓存旧绿框用于关闭动画。
        // The last submitted selector frame must be transparent, including its previous outline and hint.
        selector.withdraw();
        if(selector.grab().toImage().pixelColor(40,30).alpha()!=0
            || selector.grab().toImage().pixelColor(selector.width()/2,20).alpha()!=0)
            return fail("withdraw retained selector decorations");
        LinuxRegionSelector full(app.primaryScreen());
        QSignalSpy fullSelected(&full, &LinuxRegionSelector::selected);
        full.showPrepared(); QTest::keyClick(&full, Qt::Key_Return);
        if (fullSelected.count()!=1 || fullSelected.first().first().toRectF()!=QRectF(QPointF(),QSizeF(full.size())))
            return fail("Enter did not select whole output");
        full.hide();
        LinuxRegionSelector cancel(app.primaryScreen());
        QSignalSpy cancelled(&cancel, &LinuxRegionSelector::cancelled);
        cancel.showPrepared(); QTest::keyClick(&cancel, Qt::Key_Escape);
        if (cancelled.count()!=1) return fail("Escape did not cancel");
        cancel.hide();
        if (LinuxRegionSelector::nativeRegion(QRectF(10.2,20.6,100.1,80.2),QSize(800,600),QSize(1000,750))!=QRect(12,25,126,101)
            || LinuxRegionSelector::nativeRegion(QRectF(-10,-20,810,620),QSize(800,600),QSize(1200,900))!=QRect(0,0,1200,900)
            || !LinuxRegionSelector::nativeRegion(QRectF(900,0,10,10),QSize(800,600),QSize(1000,750)).isEmpty())
            return fail("fractional mapping or clipping differs");
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("live-selection.ini"));
        settings.setCaptureAllScreens(false);
        QWidget launcher; launcher.show();
        CaptureSession session(&launcher,settings,true);
        // 只启动然后取消，不应调用任何真实截图后端；并验证取消后可以立即再次框选。
        // Start and cancel before capture, then verify the session can be immediately reopened.
        for (int attempt=0; attempt<2; ++attempt) {
            session.start(); QTest::qWait(10);
            if(!launcher.isVisible()) return fail("starting a screenshot hid settings");
            QPointer<LinuxRegionSelector> live;
            for (auto *top:app.topLevelWidgets())
                if (auto *candidate=qobject_cast<LinuxRegionSelector *>(top); candidate && candidate->isVisible()) live=candidate;
            if (!live) return fail("session did not start with a live selector");
            QTest::keyClick(live,Qt::Key_Escape);
            QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
            if (live) return fail("cancel left a selector alive");
            if(!launcher.isVisible()) return fail("cancelling a screenshot hid settings");
        }
        session.startRecording(); QTest::qWait(10);
        LinuxRegionSelector *recordSelector = nullptr;
        for (auto *top : app.topLevelWidgets())
            if (auto *candidate = qobject_cast<LinuxRegionSelector *>(top); candidate && candidate->isVisible()) recordSelector = candidate;
        if (!recordSelector) return fail("recording did not open a live selector");
        const QRect logical(40, 40, 160, 80);
        QTest::mousePress(recordSelector, Qt::LeftButton, Qt::NoModifier, logical.topLeft());
        QTest::mouseRelease(recordSelector, Qt::LeftButton, Qt::NoModifier, QPoint(200, 120));
        QImage area(200, 100, QImage::Format_RGB32);
        area.fill(Qt::green);
        area.setDevicePixelRatio(1.25);
        auto *recordRouter = session.findChild<CaptureRouter *>();
        if (!recordRouter) return fail("recording has no capture router");
        emit recordRouter->frameReady(area);
        QTest::qWait(180);
        auto *recorder = session.findChild<LinuxGifCaptureWindow *>();
        if (!recorder || recorder->logicalCaptureRegion() != logical) return fail("recording lost the selected area");
        for (auto *top : app.topLevelWidgets())
            if (top->isVisible() && dynamic_cast<CapturePreview *>(top)) return fail("recording opened the screenshot editor");
        // 外层录制存在时仍可框选；取消截图不应销毁外层，第二次录制也独立保留。
        // Selection/cancellation and a second recording must leave the outer session alive.
        QPointer<LinuxGifCaptureWindow> outer=recorder;
        session.start(); QTest::qWait(10);
        LinuxRegionSelector *nestedSelector=nullptr;
        for(auto *top:app.topLevelWidgets())
            if(auto *candidate=qobject_cast<LinuxRegionSelector *>(top); candidate && candidate->isVisible()) nestedSelector=candidate;
        if(!nestedSelector || !outer) return fail("recording blocked a concurrent screenshot");
        QTest::keyClick(nestedSelector,Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(!outer) return fail("cancelling screenshot destroyed outer recorder");
        session.startRecording(); QTest::qWait(10);
        nestedSelector=nullptr;
        for(auto *top:app.topLevelWidgets())
            if(auto *candidate=qobject_cast<LinuxRegionSelector *>(top); candidate && candidate->isVisible()) nestedSelector=candidate;
        if(!nestedSelector) return fail("recording blocked nested recording selection");
        QTest::mousePress(nestedSelector,Qt::LeftButton,Qt::NoModifier,logical.topLeft());
        QTest::mouseRelease(nestedSelector,Qt::LeftButton,Qt::NoModifier,QPoint(200,120));
        if (nestedSelector->isVisible()) return fail("could not select the same recording area twice");
        emit recordRouter->frameReady(area); QTest::qWait(180);
        auto recorders=session.findChildren<LinuxGifCaptureWindow *>();
        if(recorders.size()!=2) return fail("nested recording replaced the outer recorder");
        QPointer<LinuxGifCaptureWindow> inner=recorders.last();
        outer->cancel();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(outer || !inner) return fail("ending outer recording destroyed inner recording");
        session.toggleGif(); // ready -> begin; avoid a real backend request by cancelling immediately.
        inner->cancel();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QImage source(1000,750,QImage::Format_RGB32); source.fill(QColor("#184256"));
        CapturePreview editor(source,&settings);
        editor.setOverlayMode([] {}); editor.prepareOverlayOnScreen(app.primaryScreen());
        const QRect native(12,25,126,101);
        editor.setInitialRegion(native); editor.showPreparedOverlay();
        auto *view=dynamic_cast<PreviewView *>(editor.findChild<QGraphicsView *>());
        if (!view || view->canvas->selection_model->rect()!=QRectF(native)
            || static_cast<LinuxAnnotationCanvas &>(editor).result().convertToFormat(QImage::Format_RGB32)!=source.copy(native))
            return fail("editor lost the live crop or changed native pixels");
        editor.hide();
    }
    // 同一次确认应产出带水印图片、干净底图和可粘贴的文件，三份结果必须对应同一选区。
    // Verify actual files and clipboard pixels, not just that the settings switches changed.
    {
        using F=LinuxSettings::CaptureFlag;
        const auto fail=[](const char *why){std::fprintf(stderr,"Capture delivery: %s\n",why); return 1;};
        QTemporaryDir out; LinuxSettings prefs(out.filePath("prefs.ini")); prefs.setSaveDirectory(out.path());
        if(!prefs.setCaptureNamePattern("delivery") || prefs.setCaptureNamePattern("../outside")) return fail("invalid naming rules");
        for (auto flag:{F::AutoSave,F::FileUrl,F::SaveOriginal,F::AutoWatermark}) prefs.setCaptureFlag(flag,true);
        prefs.setWatermarkText("WATERMARK"); prefs.sync();
        LinuxSettings saved(prefs.fileName()); auto *screen=app.primaryScreen(); const QRect crop(60,60,320,180);
        QImage raw(crop.size(),QImage::Format_RGB32); raw.fill(Qt::white);
        CapturePreview win(raw,&saved); win.setOverlayMode([]{}); win.configureRegionCapture(screen,crop,raw);
        win.prepareOverlayOnScreen(screen); win.showPreparedOverlay(); QTest::qWait(10);
        win.findChild<jietuba::Toolbar *>()->confirm_btn->click();
        const auto *mime=QApplication::clipboard()->mimeData(); const auto path=out.filePath("delivery.png");
        const QImage final=QImage(path).convertToFormat(QImage::Format_RGB32);
        if(!mime->hasImage() || !mime->urls().contains(QUrl::fromLocalFile(path)) || final==raw || final.size()!=raw.size()
            || QImage(out.filePath("delivery_original.png")).convertToFormat(QImage::Format_RGB32)!=raw
            || QImage::fromData(mime->data("image/png")).convertToFormat(QImage::Format_RGB32)!=final) return fail("clipboard, watermark or original pixels differ");
        QPointer<LinuxOcrResultWindow> ocr=new LinuxOcrResultWindow(raw,nullptr,[](const QImage &){return LinuxOcrResult{"direct OCR","en",{}};},true);
        if(ocr->isVisible() || !QTest::qWaitFor([&]{return !ocr;}) || QApplication::clipboard()->text()!="direct OCR") return fail("OCR direct copy");
        QPointer<LinuxBarcodeResultWindow> code=new LinuxBarcodeResultWindow(raw,nullptr,[](const QImage &){return LinuxBarcodeResult{{{"one code","QR Code",{}}},{}};},true);
        if(code->isVisible() || !QTest::qWaitFor([&]{return !code;}) || QApplication::clipboard()->text()!="one code") return fail("single code direct copy");
    }
    // 区域编辑：控制点拉伸、整体移动、重新框选，以及更新底图后只留下最新区域。
    // Area editing: resize handles, moving, replacement selection, and clearing the previous crop.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Area editor: %s\n",reason); return 1; };
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("area-editor.ini"));
        auto *screen=app.primaryScreen();
        const QRect logical(120,120,120,80);
        QImage crop(150,100,QImage::Format_RGB32); crop.fill(Qt::red); crop.setDevicePixelRatio(1.25);
        CapturePreview area(crop,&settings); area.setOverlayMode([] {});
        area.configureRegionCapture(screen,logical,crop);
        area.prepareOverlayOnScreen(screen); area.showPreparedOverlay(); QTest::qWait(10);
        auto *view=dynamic_cast<PreviewView *>(area.findChild<QGraphicsView *>());
        if (!view || !view->regionCapture) return fail("region mode not active");
        auto *scene=view->canvas;
        int commits=0; view->selectionFinished=[&] { ++commits; };
        const auto drag=[&](const QPointF &from,const QPointF &to) {
            QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(from));
            const QPoint target=view->mapFromScene(to);
            QMouseEvent move(QEvent::MouseMove,QPointF(target),QPointF(view->viewport()->mapToGlobal(target)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QCoreApplication::sendEvent(view->viewport(),&move);
            QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,target);
        };
        const auto near=[](const QRectF &a,const QRectF &b) {
            return std::abs(a.x()-b.x())<2 && std::abs(a.y()-b.y())<2
                && std::abs(a.width()-b.width())<2 && std::abs(a.height()-b.height())<2;
        };
        QRectF expected=scene->selection_model->rect();
        drag(expected.bottomRight(),expected.bottomRight()+QPointF(50,25));
        expected.setSize(expected.size()+QSizeF(50,25));
        if (!near(scene->selection_model->rect(),expected) || commits!=1) return fail("corner did not resize crop");
        drag(expected.center(),expected.center()+QPointF(25,25)); expected.translate(25,25);
        if (!near(scene->selection_model->rect(),expected) || commits!=2) return fail("body did not move crop");
        drag(QPointF(10,10),QPointF(50,60));
        if (!near(scene->selection_model->rect(),expected) || commits!=2) return fail("background drag replaced confirmed crop");
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(expected.center()));
        if (!near(scene->selection_model->rect(),expected) || commits!=2 || !scene->background->isVisible()) return fail("plain click recaptured or hid confirmed crop");
        auto *annotation=new jietuba::RectItem(QRectF(180,180,30,20),QPen(Qt::green,3));
        scene->undo_stack->push(new jietuba::AddItemCommand(scene,jietuba::ItemReference(annotation)));
        QImage blue(150,100,QImage::Format_RGB32); blue.fill(Qt::blue); blue.setDevicePixelRatio(1.25);
        const QRect next(400,300,120,80);
        // 走真实信号连接而非直接调用更新函数：重新框选的回复必须保留输出坐标和撤销栈。
        // Exercise the real router connection: a crop reply must not replace the document with a tiny scene.
        scene->preset_selection(QRectF(LinuxRegionSelector::nativeRegion(next,screen->geometry().size(),scene->background->image().size())));
        area.requestRegionFrame();
        auto *areaRouter=area.findChild<CaptureRouter *>();
        if (!areaRouter) return fail("region router missing");
        areaRouter->frameReady(blue);
        const auto image=scene->background->image();
        if (view->canvas!=scene || scene->undo_stack->count()!=1 || annotation->scene()!=scene
            || image.pixelColor(220,200).alpha()!=0 || image.pixelColor(550,400)!=QColor(Qt::blue))
            return fail("new capture kept old pixels or lost annotation history");
        QImage expectedBlue=blue; expectedBlue.setDevicePixelRatio(1);
        if (static_cast<LinuxAnnotationCanvas &>(area).result().convertToFormat(QImage::Format_RGB32)!=expectedBlue)
            return fail("area export included stale pixels or changed source resolution");
        int frames=0; for (auto *item:scene->items()) if (dynamic_cast<jietuba::SelectionItem *>(item)) ++frames;
        if (frames!=1) return fail("more than one selection frame remained");
        bool areaLongStarted=false;
        area.setLongCaptureHandler([&](QScreen *output,const QSize &pixels,const QRect &region,const QImage &first,const QRect &logicalRegion) {
            areaLongStarted=output==screen && pixels==image.size() && region==QRect(500,375,150,100)
                && logicalRegion==next && first.convertToFormat(QImage::Format_RGB32)==expectedBlue;
        });
        auto *areaBar=area.findChild<jietuba::Toolbar *>();
        areaBar->long_screenshot_btn->click();
        if (!areaLongStarted) return fail("long capture lost the logical area or native first frame after recapture");
        bool areaGifStarted=false;
        area.setGifCaptureHandler([&](QScreen *output,const QSize &pixels,const QRect &region,const QRect &logicalRegion) {
            areaGifStarted=output==screen && pixels==image.size() && region==QRect(500,375,150,100) && logicalRegion==next;
        });
        areaBar->gif_btn->click();
        if(!areaGifStarted) return fail("GIF capture entry lost its logical capture area");
        scene->undo_stack->undo();
        if (annotation->scene() || !scene->undo_stack->canRedo()) return fail("background refresh broke undo");
        areaBar->more_btn->click();
        area.withdrawOverlay();
        if(areaBar->more_popup() && !areaBar->more_popup()->isHidden()) return fail("withdraw retained the sibling More popup");
        if(view->viewport()->grab().toImage().pixelColor(view->mapFromScene(550,400)).alpha()!=0)
            return fail("editor withdrawal retained captured pixels or crop border");
        area.showPreparedOverlay();
        if(view->withdrawing) return fail("restoring editor retained withdrawal state");
        area.hide();
        // 奇数逻辑坐标和宽度在 125% 下会触发整屏外扩/区域取整差异，导出不得拉伸细字。
        // Odd bounds at 125% expose output-edge versus area rounding; preserve every source pixel.
        QImage oddCrop(121,101,QImage::Format_RGB32);
        for(int y=0;y<oddCrop.height();++y) for(int x=0;x<oddCrop.width();++x)
            oddCrop.setPixel(x,y,qRgb((x*37+y)%256,(y*41+x)%256,(x+y)%2 ? 255 : 0));
        oddCrop.setDevicePixelRatio(1.25);
        CapturePreview odd(oddCrop,&settings); odd.setOverlayMode([] {});
        odd.configureRegionCapture(screen,QRect(121,121,97,81),oddCrop);
        QImage nativeOdd=oddCrop; nativeOdd.setDevicePixelRatio(1);
        if (static_cast<LinuxAnnotationCanvas &>(odd).result().convertToFormat(QImage::Format_RGB32)!=nativeOdd)
            return fail("fractional area resampled its source pixels");
        // 没有区域接口的后端也必须按负坐标输出与实际缩放裁剪，不能返回整屏图冒充区域。
        // The generic fallback crops correctly for negative output origins and fractional scale.
        struct MemoryBackend final : CaptureBackend {
            QImage image;
            CaptureResult capture(const QString &,const std::atomic_bool &) override { return {image,"memory",{}}; }
        } backend;
        backend.image=QImage(100,80,QImage::Format_RGB32); backend.image.fill(Qt::red);
        std::atomic_bool cancelled{false};
        const auto result=backend.captureRegion("test",QRect(-72,8,16,16),QRect(-80,0,80,64),cancelled);
        if (result.image.size()!=QSize(20,20) || result.image.devicePixelRatio()!=1.25) return fail("fallback area size differs");
    }
    // 序号字号：自动选中后修改大小也成为后续默认值；读取旧对象/撤销不重置该偏好。
    // Number size edits survive auto-selection, inspection of old items, undo and a new capture session.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Number defaults: %s\n",reason); return 1; };
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("number-default.ini"));
        QImage source(800,600,QImage::Format_RGB32); source.fill(Qt::white);
        CapturePreview capture(source,&settings); capture.resize(800,600); capture.show(); QTest::qWait(10);
        auto *view=dynamic_cast<PreviewView *>(capture.findChild<QGraphicsView *>());
        auto *bar=capture.findChild<jietuba::Toolbar *>();
        auto *panel=capture.findChild<jietuba::NumberSettingsPanel *>();
        view->canvas->preset_selection(QRectF(20,20,760,560)); view->selectionChanged(); bar->number_btn->click();
        const auto click=[&](QPoint point) { QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(point)); };
        click(QPoint(150,100));
        auto *first=dynamic_cast<jietuba::NumberItem *>(view->editor->active_layer());
        if(!first || first->get_stroke_width()!=16) return fail("unexpected factory size");
        panel->size_spin->setValue(8);
        if(first->get_stroke_width()!=8 || settings.toolStyle("number").value("width").toInt()!=8) return fail("selected number size was not remembered");
        view->canvas->undo_stack->undo();
        if(first->get_stroke_width()!=16 || settings.toolStyle("number").value("width").toInt()!=8) return fail("undo changed the next-number preference");
        click(QPoint(300,100));
        auto *second=dynamic_cast<jietuba::NumberItem *>(view->editor->active_layer());
        if(!second || second==first || second->get_stroke_width()!=8) return fail("second number returned to size 16");
        click(QPoint(150,100)); click(QPoint(450,100));
        auto *third=dynamic_cast<jietuba::NumberItem *>(view->editor->active_layer());
        if(!third || third==first || third==second || third->get_stroke_width()!=8) return fail("inspecting an old number reset the default");
        capture.hide();
        LinuxSettings restarted(settings.fileName()); CapturePreview next(source,&restarted);
        next.findChild<jietuba::Toolbar *>()->number_btn->click();
        if(next.findChild<jietuba::NumberSettingsPanel *>()->current_size!=8) return fail("new session lost number size");
    }
    // 普通设置入口回到截图页；翻译的专用入口仍能明确进入翻译页。
    // The normal settings entry returns to Capture while the explicit translation entry still works.
    {
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("settings-entry.ini"));
        LinuxSettingsWindow window(settings);
        auto *pages=window.findChild<QStackedWidget *>();
        auto *navigation=window.findChild<jietuba::NavigationInterface *>();
        const int capture=pages->currentIndex();
        window.showTranslationPage();
        if(pages->currentIndex()==capture) return 1;
        window.hide(); window.showCapturePage();
        if(pages->currentIndex()!=capture || !navigation->widget("capture")->isChecked()) return 1;
        navigation->widget("pin")->click(); window.showCapturePage();
        if(pages->currentIndex()!=capture) return 1;
        window.showShortcutsPage();
        if(pages->currentIndex()!=0 || !navigation->widget("shortcuts")->isChecked()) return 1;

        // 走实际导入按钮与文件对话框，验证当前页和其它页的控件能立即读到新配置。
        // Exercise the real import action and ensure both this page and other pages refresh.
        const auto backup = directory.filePath("backup.ini");
        {
            QSettings source(backup, QSettings::IniFormat);
            source.setValue("app/language", "ja");
            source.setValue("app/show_main_window", false);
            source.setValue("app/screenshot_format", "JPG");
        }
        navigation->widget("other")->click();
        auto *importButton = window.findChild<QPushButton *>("importSettingsButton");
        if (!importButton || !importButton->isVisible()) return 1;
        bool imported = false;
        QTimer dismissDialogs;
        QObject::connect(&dismissDialogs, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(app.activeModalWidget())) {
                dialog->selectFile(backup);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *message = qobject_cast<QMessageBox *>(app.activeModalWidget())) {
                imported = message->icon() == QMessageBox::Information;
                message->accept();
            }
        });
        dismissDialogs.start(10);
        importButton->click();
        QTest::qWait(50);
        auto *language = window.findChild<QComboBox *>("interfaceLanguage");
        auto *showWindow = window.findChild<jietuba::SwitchSettingCard *>("showMainWindowToggle");
        auto *format = window.findChild<QComboBox *>("imageFormat");
        if (!imported || !language || !language->isVisible() || language->count() != 5 || language->currentData() != "ja"
            || !showWindow || showWindow->isChecked() || !format || format->currentData() != "jpg") {
            std::fprintf(stderr, "Other settings: import did not refresh the controls\n");
            return 1;
        }
        window.hide();
    }
    // 贴图工具栏沿用原版 PIN 布局；离屏验证悬停生命周期、穿透区域、布局隔离和已有导出。
    // Exercise the real PIN toolbar: hover lifetime, desktop input holes, independent layout and exports.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Pin toolbar: %s\n",reason); return 1; };
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("pin-toolbar.ini"));
        const QJsonArray captureLayout{QJsonArray{"pen","show"},QJsonArray{"save","more"}};
        settings.saveScreenshotToolbarLayout(captureLayout);
        const QSize output(800,600), barSize(500,40);
        if (LinuxPinToolbar::toolbarGeometry(output,QRectF(250,100,300,200),barSize)!=QRect(50,304,500,40)) return fail("default below/right positioning differs");
        if (LinuxPinToolbar::toolbarGeometry(output,QRectF(600,480,200,120),barSize)!=QRect(300,436,500,40)) return fail("bottom/right edge was not handled");
        for (const QRectF &rect : {QRectF(0,0,800,600),QRectF(-800,-600,2400,1800),QRectF(0,0,20,20)})
            if (!QRect(QPoint(),output).contains(LinuxPinToolbar::toolbarGeometry(output,rect,barSize))) return fail("toolbar escaped output bounds");
        QImage source(240,160,QImage::Format_RGB32); source.fill(QColor("#184256"));
        QPointer<LinuxPinWindow> pin=new LinuxPinWindow(source,settings);
        pin->showOnScreen(app.primaryScreen(),QRectF(100,120,240,160)); QTest::qWait(10);
        const auto enter=[](QWidget *widget) { QEnterEvent event(QPointF(20,20),QPointF(20,20),QPointF(widget->mapToGlobal(QPoint(20,20)))); QCoreApplication::sendEvent(widget,&event); };
        const auto leave=[](QWidget *widget) { QEvent event(QEvent::Leave); QCoreApplication::sendEvent(widget,&event); };
        enter(pin); QCoreApplication::processEvents();
        auto *closeButton=pin->findChild<QPushButton *>("pinClose");
        auto *toggle=pin->findChild<QPushButton *>("pinToolbarToggle");
        if (!closeButton || !toggle || closeButton->isVisible() || toggle->isVisible()
            || pin->window()->findChild<LinuxPinToolbar *>()) return fail("default hover exposed pin controls");
        QTest::keyClick(pin, Qt::Key_Space);
        QPointer<LinuxPinToolbar> host=pin->window()->findChild<LinuxPinToolbar *>();
        if (!host || !host->isVisible()) return fail("Space did not show toolbar");
        QTest::keyClick(host, Qt::Key_Space);
        if (host->isVisible()) return fail("Space did not hide toolbar from its own window");
        pin->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        settings.setPinAutoToolbar(true);
        pin=new LinuxPinWindow(source,settings);
        pin->showOnScreen(app.primaryScreen(),QRectF(100,120,240,160));
        enter(pin); QCoreApplication::processEvents();
        host=pin->window()->findChild<LinuxPinToolbar *>();
        if(host && host->isVisible()) return fail("legacy auto-toolbar expanded on hover");
        toggle=pin->findChild<QPushButton *>("pinToolbarToggle");
        if(!toggle->isVisible()) return fail("legacy auto-toolbar did not migrate to hover buttons");
        toggle->click(); host=pin->window()->findChild<LinuxPinToolbar *>();
        if(!host || !host->isVisible()) return fail("hover button did not open toolbar");
        auto *bar=host->toolbar();
        if (bar->layout_profile.setting_key!=jietuba::toolbar_layout::PIN.setting_key || !bar->copy_btn->isEnabled() || !bar->save_btn->isEnabled()) return fail("wrong profile or unavailable export actions");
        if (bar->confirm_btn->isVisible() || bar->gif_btn->isVisible() || bar->pin_btn->isVisible() || bar->undo_btn->isEnabled()) return fail("capture-only/unsupported action exposed");
        for (auto *button : bar->tool_buttons) if (!button->isEnabled()) return fail("pin annotation tool disabled");
        if (!host->windowHandle()->mask().contains(bar->geometry().center()) || host->windowHandle()->mask().contains(QPoint(0,0))) return fail("input mask blocks the desktop or omits toolbar");
        // 布局暂时没有可见控件时，空 Qt mask 会恢复全屏输入，必须保留屏幕外哨兵。
        // A temporary empty layout must not reset this transparent host to full-output input.
        bar->hide(); QCoreApplication::processEvents();
        if (host->windowHandle()->mask().isEmpty() || host->windowHandle()->mask().contains(pin->geometry().center())) return fail("empty toolbar layout intercepts the pin");
        bar->show(); QCoreApplication::processEvents();
        bar->copy_btn->click();
        if (QImage::fromData(app.clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32)!=source) return fail("toolbar copy changed image pixels");
        bar->more_btn->click(); QCoreApplication::processEvents();
        if (!bar->more_popup() || !bar->more_popup()->isVisible() || !host->windowHandle()->mask().contains(bar->more_popup()->geometry().center())) return fail("More popup outside input region");
        // 离屏平台也会根据合成光标位置补发 Enter，先把光标移到控件范围外再模拟离开。
        // Offscreen Qt synthesizes Enter from cursor position too; keep it outside controls for leave tests.
        QCursor::setPos(-1000,-1000); QCoreApplication::processEvents();
        leave(pin); enter(host); QTest::qWait(2100);
        if (!host->isVisible()) return fail("toolbar hid while pointer was on it");
        leave(host); QCoreApplication::processEvents();
        if(!host->isVisible()) return fail("hover exit closed a manually opened toolbar");
        pin->toggleToolbar();
        settings.setPinAutoToolbar(false);
        enter(pin); QCoreApplication::processEvents();
        if (host->isVisible()) return fail("disabled auto-toolbar still shows");
        settings.setPinHoverButtons(true);
        enter(pin);
        toggle=pin->findChild<QPushButton *>("pinToolbarToggle");
        if (!toggle || !toggle->isVisible()) return fail("manual toolbar control absent");
        toggle->click(); QCoreApplication::processEvents();
        if (!host->isVisible()) return fail("manual toolbar toggle failed");
        // 在放大的贴图上通过实际鼠标事件画线；确保显示倍率、边框和控制点不进入导出。
        // Draw on a zoomed pin via the viewport; exports keep source coordinates and omit window decorations.
        auto *view = dynamic_cast<PreviewView *>(pin->findChild<QGraphicsView *>());
        if (!view || !view->canvas || !view->canvas->selection_model->is_confirmed()) return fail("pin canvas missing");
        settings.setPinPercent("fine_zoom_step",3); settings.setPinPercent("opacity_step",10);
        const auto wheelOnCanvas=[&](int delta, Qt::KeyboardModifiers mods,bool horizontal=false){
            QPoint pt(20,30); QWheelEvent wheel(pt,view->viewport()->mapToGlobal(pt),{},horizontal ? QPoint(delta,0) : QPoint(0,delta),Qt::NoButton,mods,Qt::NoScrollPhase,false);
            QApplication::sendEvent(view->viewport(),&wheel);
        };
        wheelOnCanvas(120,Qt::AltModifier); if(qAbs(pin->zoom()-1.03)>1e-6) return fail("canvas lost fine zoom");
        // 原版特别兼容 Alt 导致的横向滚轮；还要保留同一手势内的高分辨率余量。
        // Alt can remap wheel input to X. Partial horizontal notches must accumulate, too.
        wheelOnCanvas(120,Qt::AltModifier,true); if(qAbs(pin->zoom()-1.06)>1e-6) return fail("horizontal Alt wheel ignored fine zoom");
        wheelOnCanvas(-60,Qt::AltModifier,true); if(qAbs(pin->zoom()-1.06)>1e-6) return fail("half notch zoomed early");
        wheelOnCanvas(-60,Qt::AltModifier,true); if(qAbs(pin->zoom()-1.03)>1e-6) return fail("horizontal fine zoom lost partial notch");
        wheelOnCanvas(-120,Qt::ControlModifier);
        if(qAbs(pin->pinOpacity()-.9)>1e-6) return fail("canvas lost opacity step");
        wheelOnCanvas(120,Qt::ControlModifier); pin->triggerShortcut("pin_reset");
        pin->triggerShortcut("pin_zoom_in"); QCoreApplication::processEvents();
        bar->pen_btn->click(); QCoreApplication::processEvents();
        if (view->tool != "pen" || !host->findChild<jietuba::PaintSettingsPanel *>("linuxPenPanel")->isVisible()) return fail("pin pen panel missing");
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(30,40));
        QTest::mouseMove(view->viewport(),view->mapFromScene(100,40));
        QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(150,40));
        const QImage marked = pin->image().convertToFormat(QImage::Format_RGB32);
        if (!bar->undo_btn->isEnabled() || marked.size()!=source.size() || marked.pixel(90,40)==source.pixel(90,40)) return fail("pin stroke missed native pixels");
        bar->undo_btn->click();
        if (pin->image().convertToFormat(QImage::Format_RGB32)!=source || !bar->redo_btn->isEnabled()) return fail("pin undo failed");
        QTest::keyClick(host, Qt::Key_Y, Qt::ControlModifier);
        pin->toggleToolbar(); pin->toggleToolbar();
        if (pin->image().convertToFormat(QImage::Format_RGB32)!=marked || !view->tool.isEmpty()) return fail("pin layers lost across toolbar toggle");
        const auto dragPin=[&](QWidget *target) {
            const QPoint before=pin->pos();
            const QPoint fixedPointer=target->mapToGlobal(QPoint(35,30));
            QTest::mousePress(target,Qt::LeftButton,Qt::NoModifier,QPoint(15,15));
            // 相同屏幕坐标会因 Qt 子控件重新定位再次到达；不可将其当作又移动了一次。
            // Qt can redeliver motion after child relocation; repeated absolute positions must not move again.
            for(int repeat=0;repeat<4;++repeat) {
                QMouseEvent move(QEvent::MouseMove,QPointF(35,30),QPointF(fixedPointer),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
                QCoreApplication::sendEvent(target,&move);
            }
            QTest::mouseRelease(target,Qt::LeftButton,Qt::NoModifier,QPoint(35,30));
            return pin->pos()==before+QPoint(20,15);
        };
        // 分别覆盖展开画布和收起画布的输入路径，两种情况都不能丢掉贴图拖动。
        // Exercise both the active viewport and the pin itself after collapsing annotations.
        if (!dragPin(view->viewport())) return fail("pin canvas swallowed window drag");
        const QRectF movedPin(pin->pos()-app.primaryScreen()->geometry().topLeft(),pin->size());
        if (bar->pos()!=LinuxPinToolbar::toolbarGeometry(host->size(),movedPin,bar->size()).topLeft())
            return fail("toolbar failed to follow pin drag");
        pin->toggleToolbar();
        if (!dragPin(pin)) return fail("collapsed pin failed to drag");
        bar->more_btn->click();
        if (bar->more_popup() && !bar->more_popup()->isHidden()) return fail("hidden toolbar reopened its popup");
        pin->toggleToolbar();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->mapFromScene(90,40));
        if (!view->editor->active_layer() || view->tool!="pen") return fail("reopened pin did not select existing stroke");
        bar->copy_btn->click();
        if (QImage::fromData(app.clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32)!=marked) return fail("copy omitted pin edits");
        const QPoint automatic=bar->pos();
        QTest::mousePress(bar->drag_handle,Qt::LeftButton,Qt::NoModifier,QPoint(5,20));
        QMouseEvent drag(QEvent::MouseMove,QPointF(35,40),QPointF(bar->drag_handle->mapToGlobal(QPoint(35,40))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QCoreApplication::sendEvent(bar->drag_handle,&drag);
        QTest::mouseRelease(bar->drag_handle,Qt::LeftButton,Qt::NoModifier,QPoint(5,20));
        if (bar->pos()==automatic || !host->rect().contains(bar->geometry())) return fail("toolbar drag did not move within output bounds");
        QTest::mouseDClick(bar->drag_handle,Qt::LeftButton,Qt::NoModifier,QPoint(5,20));
        if (bar->pos()!=automatic) return fail("double click failed to restore automatic positioning");
        bar->more_btn->click(); QCoreApplication::processEvents();
        bar->more_popup()->adjust_btn->click(); QCoreApplication::processEvents();
        auto *editor=host->findChild<jietuba::ToolbarLayoutDialog *>("linuxToolbarLayoutDialog");
        if (!editor || !editor->isVisible() || editor->isWindow() || QApplication::activeModalWidget() || !host->windowHandle()->mask().contains(QPoint(0,0))) return fail("layout editor is hidden/modal or misses input region");
        for (auto *row : editor->findChildren<jietuba::ToolbarLayoutRow *>()) if (row->key=="copy") row->set_mode("more");
        editor->accept(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); QCoreApplication::processEvents();
        if (host->editingLayout() || host->windowHandle()->mask().contains(QPoint(0,0)) || settings.screenshotToolbarLayout()!=captureLayout) return fail("layout editing leaked input or changed capture layout");
        bool savedCopy=false;
        for (const auto &value : settings.pinToolbarLayout()) if (value.toArray()==QJsonArray{"copy","more"}) savedCopy=true;
        if (!savedCopy) return fail("pin layout was not persisted");
        bar->more_btn->click(); QCoreApplication::processEvents();
        if (!bar->copy_btn->isVisible() || bar->copy_btn->parentWidget()!=bar->more_popup()) return fail("saved folded layout was not applied");
        const auto file=directory.filePath("pin-export.png");
        QTimer::singleShot(0,[file] { if (auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { dialog->selectFile(file); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection); } });
        bar->save_btn->click();
        if (QImage(file).convertToFormat(QImage::Format_RGB32)!=marked) return fail("toolbar save omitted pin edits");
        // 在工具栏自身的原生窗口收到快捷键时，仍应操作本贴图。
        // Keyboard focus on the toolbar surface must still target its owning pin.
        app.clipboard()->setText("unchanged"); host->activateWindow(); host->setFocus();
        QTest::keyClick(host,Qt::Key_C,Qt::ControlModifier);
        if (!app.clipboard()->mimeData()->hasFormat("image/png")) return fail("pin shortcuts failed on toolbar surface");
        bar->more_popup()->adjust_btn->click(); QCoreApplication::processEvents();
        if (!host->editingLayout()) return fail("layout editor could not reopen");
        QTest::keyClick(host,Qt::Key_Escape); QCoreApplication::processEvents();
        if (!pin->isVisible() || host->editingLayout()) return fail("Escape closed pin instead of layout editor");
        pin->hide(); QCoreApplication::processEvents();
        if (host->isVisible()) return fail("hiding pin left an input surface mapped");
        pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if (pin || host) return fail("closing pin left toolbar surface alive");
    }
    // 从全局入口触发，验证剪贴板图片和纯文字都能生成可见贴图。
    // Exercise the global action itself with both image and text clipboard payloads.
    {
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("global-actions.ini"));
        QWidget launcher;
        CaptureSession session(&launcher, settings, true);
        QImage source(32,48,QImage::Format_RGB32); source.fill(Qt::green);
        // 清空及不可识别的数据不得创建任何可见窗口，包括原来的“粘贴”提示框。
        // Empty/unsupported payloads must not map a pin or the former clipboard prompt.
        for (bool unsupported : {false, true}) {
            auto *mime = new QMimeData;
            if (unsupported) mime->setData("application/octet-stream", "no pin content");
            app.clipboard()->setMimeData(mime);
            const auto windows = app.topLevelWidgets();
            session.globalShortcutActivated("pin_clipboard", {});
            QCoreApplication::processEvents();
            for (auto *top : app.topLevelWidgets())
                if (top->isVisible() && !windows.contains(top)) return 203;
        }
        for (bool text : {false,true}) {
            if (text) app.clipboard()->setText("clipboard pin"); else app.clipboard()->setImage(source);
            session.globalShortcutActivated("pin_clipboard", {});
            LinuxPinWindow *pin = nullptr;
            for (auto *widget : app.allWidgets()) if ((pin = qobject_cast<LinuxPinWindow *>(widget))) break;
            if (!pin || !pin->isVisible() || pin->image().isNull() || (!text && pin->image()!=source)) return 202;
            auto *screen = pin->screen();
            const QPoint expected = screen->geometry().topLeft()
                + QPoint(qRound((screen->geometry().width() - pin->width()) / 2.0),
                         qRound((screen->geometry().height() - pin->height()) / 2.0));
            if (pin->pos() != expected) return 204;
            pin->toggleToolbar(); QCoreApplication::processEvents();
            auto *host = pin->window()->findChild<LinuxPinToolbar *>();
            if (!host || !host->isVisible()) return 205;
            const auto follows = [&] {
                const QRectF rect(pin->pos() - screen->geometry().topLeft(), pin->size());
                return host->toolbar()->geometry() == LinuxPinToolbar::toolbarGeometry(host->size(), rect, host->toolbar()->size());
            };
            if (!follows()) return 206;
            // 跟随移动与缩放，不只检查首次显示时计算出的静态矩形。
            // Follow the live pin after movement and zoom, not just its initial rectangle.
            pin->move(pin->pos() + QPoint(20, -40));
            pin->triggerShortcut("pin_zoom_in"); QCoreApplication::processEvents();
            if (!follows()) return 207;
            pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        }
    }
    // 用真实控件和画布验证：默认样式跨会话保留，旧图形编辑不污染默认值，键盘与取色坐标准确。
    // Exercise real controls/canvas: persist defaults, isolate item edits, and verify keyboard and pixel sampling.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Capture tools: %s\n",reason); return 1; };
        QTemporaryDir directory; const auto path=directory.filePath("capture-tools.ini");
        LinuxSettings settings(path);
        QImage source(1000,700,QImage::Format_RGB32);
        for (int y=0;y<source.height();++y) for (int x=0;x<source.width();++x) source.setPixel(x,y,qRgb(x%256,y%256,(x+y)%256));
        CapturePreview capture(source,&settings); capture.resize(1000,700); capture.show(); capture.activateWindow(); QTest::qWait(20);
        auto *view=dynamic_cast<PreviewView *>(capture.findChild<QGraphicsView *>());
        auto *bar=capture.findChild<jietuba::Toolbar *>();
        auto *paint=capture.findChild<jietuba::PaintSettingsPanel *>("linuxPenPanel");
        auto *shape=capture.findChild<jietuba::ShapeSettingsPanel *>();
        auto *textPanel=capture.findChild<jietuba::TextSettingsPanel *>();
        auto *mag=capture.findChild<jietuba::MagnifierOverlay *>();
        const QRectF crop(20,20,900,600);
        view->canvas->selection_model->initialize_confirmed_rect(crop); view->selectionChanged(); view->setFocus();
        const auto key=[&](Qt::Key k,Qt::KeyboardModifiers mods=Qt::NoModifier) { QTest::keyClick(view,k,mods); QCoreApplication::processEvents(); };
        const auto point=[&](int x,int y) { return view->mapFromScene(QPointF(x,y)); };
        const auto drag=[&](QPoint a,QPoint b) {
            QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,a); QTest::mouseMove(view->viewport(),b,5);
            QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,b); QCoreApplication::processEvents();
        };
        key(Qt::Key_P);
        if (view->tool!="pen") return fail("default tool shortcut did not run");
        paint->size_spin->setValue(9);
        QMetaObject::invokeMethod(paint->color_picker_btn,"color_changed",Q_ARG(QColor,QColor("#123456")));
        paint->line_style_combo->setCurrentIndex(1);
        const auto penDefault=settings.toolStyle("pen");
        if (penDefault.value("width").toInt()!=9 || QColor(penDefault.value("color").toString())!=QColor("#123456")) return fail("panel did not persist pen preferences");
        drag(point(100,100),point(250,150));
        jietuba::StrokeItem *stroke=nullptr;
        for (auto *item:view->canvas->items()) if (auto *candidate=dynamic_cast<jietuba::StrokeItem *>(item)) stroke=candidate;
        if (!stroke || stroke->pen().widthF()!=9 || stroke->pen().color()!=QColor("#123456")) return fail("drawing ignored pen preferences");
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,point(175,125));
        if (view->editor->active_layer()!=stroke) return fail("could not select stroke");
        paint->size_spin->setValue(13);
        if (stroke->pen().widthF()!=13 || settings.toolStyle("pen")!=penDefault) return fail("selected item contaminated new-item defaults");
        view->setFocus(); key(Qt::Key_Z,Qt::ControlModifier);
        if (stroke->pen().widthF()!=9) return fail("undo shortcut failed");
        key(Qt::Key_Y,Qt::ControlModifier); if (stroke->pen().widthF()!=13) return fail("redo shortcut failed");
        key(Qt::Key_Delete); if (stroke->scene()) return fail("delete shortcut failed");
        key(Qt::Key_Z,Qt::ControlModifier); if (!stroke->scene()) return fail("deleted item could not be restored");
        key(Qt::Key_R); shape->size_spin->setValue(4);
        key(Qt::Key_O); if (shape->current_size!=3) return fail("rectangle defaults leaked into ellipse");
        shape->size_spin->setValue(6); key(Qt::Key_R); if (shape->current_size!=4) return fail("rectangle defaults were not restored");
        key(Qt::Key_T); textPanel->size_spin->setValue(22); textPanel->bold_btn->click(); textPanel->background_btn->click();
        drag(point(110,260),point(110,260));
        auto *text=dynamic_cast<jietuba::TextItem *>(view->canvas->focusItem());
        if (!text || text->font().pointSize()!=22 || !text->font().bold() || !text->has_background) return fail("new text ignored persistent font/effects");
        QTest::keyClicks(view,"pardon");
        if (view->tool!="text" || text->toPlainText()!="pardon") return fail("tool keys stole text input");
        key(Qt::Key_A,Qt::ControlModifier); key(Qt::Key_C,Qt::ControlModifier);
        if (QApplication::clipboard()->text()!="pardon" || view->canvas->selection_model->is_empty()) return fail("copy shortcut stole text selection");
        bar->pen_btn->click(); key(Qt::Key_S);
        if (!view->tool.isEmpty()) return fail("cursor shortcut failed");
        QTest::mouseMove(view->viewport(),point(300,200)); QTest::qWait(5);
        if (!mag || !mag->isVisible() || !mag->cursor_scene_pos) return fail("magnifier did not follow pointer");
        const QColor color=source.pixelColor(int(mag->cursor_scene_pos->x()),int(mag->cursor_scene_pos->y()));
        key(Qt::Key_C);
        if (QApplication::clipboard()->text()!=QString("%1, %2, %3").arg(color.red()).arg(color.green()).arg(color.blue())) return fail("RGB sample is not a source pixel");
        key(Qt::Key_Shift); key(Qt::Key_C);
        if (QApplication::clipboard()->text()!=color.name().toUpper() || !QApplication::clipboard()->mimeData()->hasColor()) return fail("HEX cycling/color MIME failed");
        const double zoom=mag->get_zoom_factor(); key(Qt::Key_PageUp);
        if (mag->get_zoom_factor()!=zoom+.25 || settings.magnifierZoom()!=zoom+.25) return fail("zoom shortcut was not persisted");
        // 同一源图经过不同视口缩放，边缘取色仍一致，放大镜始终限制在当前输出内。
        // Different view scales preserve source samples and keep edge popups inside the output.
        for (QSize size:{QSize(900,650),QSize(640,480)}) {
            capture.resize(size); QTest::qWait(5);
            for (QPointF pos:{QPointF(.2,.2),QPointF(999.2,699.2),QPointF(300.2,200.2)}) {
                mag->update_cursor(pos);
                if (!capture.rect().contains(mag->geometry()) || mag->get_color_info_text()!=source.pixelColor(int(pos.x()),int(pos.y())).name().toUpper()) return fail("edge/scaled sampling or placement failed");
            }
        }
        view->setFocus(); key(Qt::Key_Right);
        if (mag->cursor_scene_pos->x()!=301.2) return fail("native pixel nudge failed");
        if (!settings.setShortcut("pen","F2")) return fail("custom shortcut could not be saved");
        key(Qt::Key_P); if (!view->tool.isEmpty()) return fail("old binding is still active");
        key(Qt::Key_F2); if (view->tool!="pen" || paint->current_size!=9) return fail("custom tool binding or default restoration failed");
        capture.hide();
        LinuxSettings restarted(path); CapturePreview next(source,&restarted); next.resize(1000,700); next.show(); next.activateWindow(); QTest::qWait(15);
        auto *nextView=dynamic_cast<PreviewView *>(next.findChild<QGraphicsView *>());
        nextView->canvas->selection_model->initialize_confirmed_rect(crop); nextView->selectionChanged(); nextView->setFocus();
        QTest::keyClick(nextView,Qt::Key_F2);
        auto *nextPaint=next.findChild<jietuba::PaintSettingsPanel *>("linuxPenPanel");
        if (nextView->tool!="pen" || nextPaint->current_size!=9 || nextPaint->current_color!=QColor("#123456")) {
            std::fprintf(stderr,"tool=%s width=%d color=%s stored-width=%d stored-color=%s key=%s\n",qPrintable(nextView->tool),nextPaint->current_size,qPrintable(nextPaint->current_color.name()),restarted.toolStyle("pen").value("width").toInt(),qPrintable(restarted.toolStyle("pen").value("color").toString()),qPrintable(restarted.shortcut("pen")));
            return fail("preferences lost across capture/settings instances");
        }
        auto *nextMag=next.findChild<jietuba::MagnifierOverlay *>();
        if (nextMag->get_zoom_factor()!=zoom+.25) return fail("magnifier zoom lost across sessions");
        auto *nextBar=next.findChild<jietuba::Toolbar *>(); nextBar->text_btn->click();
        auto *nextText=next.findChild<jietuba::TextSettingsPanel *>();
        if (nextText->current_font.pointSize()!=22 || !nextText->current_font.bold() || !nextText->background_enabled) return fail("text preferences lost across sessions");
        // 设置页写入、冲突拒绝、恢复默认全部经过真实编辑控件。
        // Exercise settings controls for persistence, conflict rejection and restoring defaults.
        LinuxSettingsWindow window(restarted);
        window.showShortcutsPage(); QTest::qWait(20);
        auto *tabs=window.findChild<jietuba::SegmentedWidget *>("linuxInAppShortcutTabs");
        auto *shortcutStack=window.findChild<QStackedWidget *>("InAppShortcutStack");
        if(!tabs || !shortcutStack || shortcutStack->count()!=4) return fail("original shortcut tabs missing");
        tabs->setCurrentItem("tools");
        // setCurrentItem 更新选中状态，切页由用户点击触发，测试走实际按钮。
        // Exercise actual tab clicks, not just the selected-state setter.
        for(auto *button:tabs->findChildren<QPushButton *>()) if(button->text()==QStringLiteral("标注工具")) button->click();
        if(shortcutStack->currentIndex()!=1) return fail("tool shortcut tab did not switch");
        auto clear=[](LinuxShortcutEdit *edit) {QTest::mouseClick(edit,Qt::LeftButton,Qt::NoModifier,QPoint(edit->width()-8,edit->height()/2));};
        auto enter=[](LinuxShortcutEdit *edit,int key) {QTest::mouseClick(edit,Qt::LeftButton,Qt::NoModifier,QPoint(8,edit->height()/2)); QTest::keyClick(edit,Qt::Key(key)); QTest::keyClick(edit,Qt::Key_Return);};
        auto *editor=window.findChild<LinuxShortcutEdit *>("localShortcut_pen");
        enter(editor,Qt::Key_F3);
        if(restarted.shortcut("pen")!="F2") return fail("existing binding was overwritten without deletion");
        clear(editor); if(!restarted.shortcut("pen").isEmpty()) return fail("clear affordance did not delete");
        QTest::mouseClick(editor,Qt::LeftButton,Qt::NoModifier,QPoint(8,editor->height()/2)); QTest::keyClick(editor,Qt::Key_F3); QTest::keyClick(editor,Qt::Key_Escape);
        if(!restarted.shortcut("pen").isEmpty() || !editor->text().isEmpty()) return fail("Escape committed instead of cancelling");
        enter(editor,Qt::Key_F3);
        if(restarted.shortcut("pen")!="F3") return fail("empty shortcut editor did not commit");
        auto *conflict=window.findChild<LinuxShortcutEdit *>("localShortcut_rect");
        clear(conflict); enter(conflict,Qt::Key_F3);
        if(!restarted.shortcut("rect").isEmpty() || !conflict->text().isEmpty()) return fail("conflicting binding was committed");
        window.findChild<QPushButton *>("resetLocalShortcuts")->click();
        if(restarted.shortcut("pen")!="P" || editor->text()!="p") return fail("shortcut reset is not wired");
        for(auto *button:tabs->findChildren<QPushButton *>()) if(button->text()==QStringLiteral("截图")) button->click();
        auto *middle=window.findChild<LinuxShortcutEdit *>("localShortcut_pin");
        clear(middle); QTest::mouseClick(middle,Qt::LeftButton,Qt::NoModifier,QPoint(8,middle->height()/2));
        QTest::mouseClick(middle,Qt::MiddleButton); QTest::keyClick(middle,Qt::Key_Return);
        if(restarted.shortcut("pin")!="mousemiddle") return fail("middle-button pin binding was not recorded");
        const auto renderPath=qgetenv("JIETUBA_SHORTCUTS_TEST_RENDER");
        if(!renderPath.isEmpty()) {
            // 截图 QA 与真实入口一样初始化应用调色板，不能只把卡片切暗而保留浅色文字调色板。
            // Visual QA initializes the application palette just like the real entry point.
            jietuba::get_ui_theme()->init(nullptr,&app);
            const auto oldMode=jietuba::ui_theme_mode_value(jietuba::get_ui_theme()->mode());
            const auto oldAppPalette=QApplication::palette();
            window.resize(1050,830); jietuba::get_ui_theme()->set_mode("light",false); QTest::qWait(20); window.grab().save(QString::fromLocal8Bit(renderPath));
            jietuba::get_ui_theme()->set_mode("dark",false); QTest::qWait(20); window.grab().save(QString::fromLocal8Bit(renderPath)+".dark.png");
            window.resize(600,500); QTest::qWait(20); window.grab().save(QString::fromLocal8Bit(renderPath)+".narrow.png");
            QApplication::setPalette(oldAppPalette); jietuba::get_ui_theme()->set_mode(oldMode,false);
        }
        window.hide();
        // 门户抢先截获 Ctrl+Shift+S 时仍能快速保存；普通按键重绑定后旧组合不再复制。
        // A portal-intercepted Ctrl+Shift+S still quick-saves; rebinding disables the old local combination.
        restarted.setSaveDirectory(directory.path()); restarted.sync();
        if (!next.triggerLocalBinding("Ctrl+Shift+S") || !nextView->canvas->selection_model->is_empty()
            || QDir(directory.path()).entryList({"*.png"},QDir::Files).isEmpty()) return fail("portal-to-local quick save failed");
        nextView->setFocus(); QTest::keyClick(nextView,Qt::Key_L);
        if (nextView->canvas->selection_model->rect()!=crop) return fail("restore last region failed");
        if (!restarted.setShortcut("copy","Alt+C")) return fail("copy binding could not change");
        QApplication::clipboard()->setText("unchanged");
        QTest::keyClick(nextView,Qt::Key_C,Qt::ControlModifier);
        if (QApplication::clipboard()->text()!="unchanged") return fail("old copy binding remained active");
        QTest::keyClick(nextView,Qt::Key_C,Qt::AltModifier);
        if (!QApplication::clipboard()->mimeData()->hasFormat("image/png") || !nextView->canvas->selection_model->is_empty()) return fail("custom copy did not export crop");
        next.hide();
        auto *pin=new LinuxPinWindow(source,restarted); pin->showOnScreen(app.primaryScreen()); pin->activateWindow(); pin->setFocus(); QTest::qWait(5);
        const double initialZoom=pin->zoom();
        QTest::keyClick(pin,Qt::Key_Plus,Qt::ControlModifier|Qt::ShiftModifier);
        if (pin->zoom()<=initialZoom) return fail("pin zoom shortcut failed");
        if (!restarted.setShortcut("pin_reset","F6")) return fail("pin reset binding failed");
        QTest::keyClick(pin,Qt::Key_F6); if (pin->zoom()!=1) return fail("pin reset shortcut failed");
        QTest::keyClick(pin,Qt::Key_C,Qt::ControlModifier);
        if (!QApplication::clipboard()->mimeData()->hasFormat("image/png")) return fail("capture copy binding leaked into pin context");
        pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    // 对照原版的固定 Shift/滚轮行为：测试原生窗口事件，不能只向视图直接注入按键。
    // Regress Windows-style Shift/wheel behavior through native window delivery, not only direct view events.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Magnifier input: %s\n",reason); return 1; };
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("magnifier-input.ini"));
        QImage source(1000,700,QImage::Format_RGB32); source.fill(QColor(31,64,128));
        CapturePreview capture(source,&settings);
        capture.setOverlayMode([] {}); capture.prepareOverlayOnScreen(app.primaryScreen()); capture.showPreparedOverlay();
        capture.activateWindow(); QTest::qWait(10);
        auto *view=dynamic_cast<PreviewView *>(capture.findChild<QGraphicsView *>());
        auto *mag=capture.findChild<jietuba::MagnifierOverlay *>();
        auto *window=capture.windowHandle(); view->setFocus();
        QTest::mouseMove(view->viewport(),view->mapFromScene(QPointF(300,200))); QTest::qWait(5);
        const QString rgb="31, 64, 128", hex="#1F4080";
        if (!window || !mag->isVisible() || mag->get_zoom_factor()!=4 || mag->get_color_info_text()!=rgb) return fail("original 4x/RGB defaults missing");
        const QImage rgbView=mag->grab().toImage();
        QTest::keyClick(window,Qt::Key_Shift);
        if (mag->get_color_info_text()!=hex) return fail("native-window Shift did not cycle exactly once");
        QCoreApplication::processEvents();
        if (mag->grab().toImage()==rgbView) return fail("format changed internally but the visible magnifier did not repaint");
        QTest::keyClick(view->viewport(),Qt::Key_Shift);
        if (mag->get_color_info_text()!=rgb) return fail("viewport Shift did not cycle exactly once");
        {
            // 模拟 layer-shell 的键盘表面已切换，但 QWidget 的全局焦点还停在设置输入框。
            // Simulate native capture delivery while QWidget focus still belongs to a settings editor.
            QLineEdit settingsEditor; settingsEditor.show(); settingsEditor.activateWindow(); settingsEditor.setFocus(); QTest::qWait(5);
            QKeyEvent nativeCaptureShift(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier);
            QApplication::sendEvent(window,&nativeCaptureShift);
            if (mag->get_color_info_text()!=hex) return fail("foreign settings focus blocked native capture Shift");
        }
        capture.activateWindow(); view->setFocus(); QTest::qWait(5);
        QTest::keyClick(window,Qt::Key_Shift);
        if (mag->get_color_info_text()!=rgb) return fail("Shift failed after restoring capture focus");
        QKeyEvent reserve(QEvent::ShortcutOverride,Qt::Key_Shift,Qt::ShiftModifier);
        QApplication::sendEvent(view->viewport(),&reserve);
        if (!reserve.isAccepted() || mag->get_color_info_text()!=rgb) return fail("ShortcutOverride changed the format");
        QKeyEvent repeat(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier,QString(),true);
        QApplication::sendEvent(window,&repeat);
        if (mag->get_color_info_text()!=rgb) return fail("held Shift cycled repeatedly");
        QKeyEvent rightPress(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier,62,0xffe2,0,QString(),false);
        QKeyEvent rightRelease(QEvent::KeyRelease,Qt::Key_Shift,Qt::NoModifier,62,0xffe2,0,QString(),false);
        QApplication::sendEvent(window,&rightPress); QApplication::sendEvent(window,&rightRelease);
        if (mag->get_color_info_text()!=hex) return fail("right Shift or release cycled incorrectly");
        QKeyEvent combined(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier|Qt::ControlModifier);
        QApplication::sendEvent(window,&combined);
        if (mag->get_color_info_text()!=hex) return fail("a modified shortcut changed the format");
        // 框选时鼠标被视图抓取，普通工具快捷键被禁用，但原版的颜色格式切换仍必须生效。
        // Crop dragging grabs the mouse and blocks tool shortcuts, but must still permit format cycling.
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(QPointF(20,20)));
        QTest::mouseMove(view->viewport(),view->mapFromScene(QPointF(400,300)));
        QTest::keyClick(window,Qt::Key_Shift);
        if (!view->canvas->selection_model->is_dragging() || mag->get_color_info_text()!=rgb) return fail("Shift failed while selecting a crop");
        QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(QPointF(400,300)));
        QTest::mouseMove(view->viewport(),view->mapFromScene(QPointF(300,200)));
        const QPoint local=view->mapFromScene(QPointF(300,200));
        const auto wheel=[&](int delta) {
            QWheelEvent event(local,view->viewport()->mapToGlobal(local),QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
            QApplication::sendEvent(view->viewport(),&event);
        };
        wheel(120); if (mag->get_zoom_factor()!=4.25 || settings.magnifierZoom()!=4.25) return fail("wheel does not zoom by original 0.25 step");
        wheel(-120); if (mag->get_zoom_factor()!=4) return fail("reverse wheel zoom failed");
        auto options=jietuba::MagnifierOverlay::Options{};
        options.zoom=10; mag->configure(options); wheel(120);
        if (mag->get_zoom_factor()!=10) return fail("maximum zoom was not clamped");
        options.zoom=2; mag->configure(options); wheel(-120);
        if (mag->get_zoom_factor()!=2) return fail("minimum zoom was not clamped");
        for (auto &format:options.formats) format.enabled=format.name=="HEX";
        mag->configure(options); const auto single=mag->get_color_info_text(); QTest::keyClick(window,Qt::Key_Shift);
        if (mag->get_color_info_text()!=single) return fail("single-format mode changed unexpectedly");
        options.formats=jietuba::color_formats::default_formats();
        for (auto &format:options.formats) format.enabled=true;
        mag->configure(options);
        for (int i=1;i<=options.formats.size();++i) {
            QTest::keyClick(window,Qt::Key_Shift);
            if (mag->get_color_info_text()!=options.formats[i%options.formats.size()].render(QColor(31,64,128))) return fail("enabled format order or wraparound differs");
        }
    }
    // 回归“调整后截图假死”：使用全屏截图流程，检查同一表面、无原生模态、Esc 及焦点恢复。
    // Regress the frozen capture: exercise fullscreen presentation, same-surface editing and Escape/focus recovery.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Embedded toolbar editor: %s\n",reason); return 1; };
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("embedded-toolbar.ini"));
        QImage image(800,600,QImage::Format_RGB32); image.fill(Qt::white);
        CapturePreview capture(image,&settings);
        int cancelled=0;
        capture.setOverlayMode([&] { ++cancelled; capture.hide(); });
        capture.prepareOverlayOnScreen(app.primaryScreen()); capture.showPreparedOverlay(); QTest::qWait(20);
        auto *bar=capture.findChild<jietuba::Toolbar *>();
        auto *view=dynamic_cast<PreviewView *>(capture.findChild<QGraphicsView *>());
        const QRectF crop(20,20,500,300);
        view->canvas->selection_model->initialize_confirmed_rect(crop); view->selectionChanged();
        const auto flush=[] { QCoreApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); };
        const auto open=[&] {
            view->setFocus(); bar->more_btn->click(); bar->more_popup()->adjust_btn->click(); QCoreApplication::processEvents();
            return capture.findChild<jietuba::ToolbarLayoutDialog *>("linuxToolbarLayoutDialog");
        };
        auto *dialog=open();
        if (!dialog || !dialog->isVisible() || dialog->isWindow() || dialog->window()!=&capture || dialog->windowHandle()
            || dialog->windowModality()!=Qt::NonModal || QApplication::activeModalWidget()) return fail("editor is a separate/modal window");
        if (!capture.isEnabled() || view->isEnabled() || bar->isEnabled()) return fail("background input is not isolated locally");
        QTest::keyClick(dialog->findChild<jietuba::ComboBox *>(),Qt::Key_Escape); flush();
        if (bar->preview_layout_dialog_open() || cancelled || !view->isEnabled() || !bar->isEnabled()
            || QApplication::focusWidget()!=view || view->canvas->selection_model->rect()!=crop) return fail("Escape lost capture state or focus");
        dialog=open();
        capture.resize(640,480); QTest::qWait(10);
        auto *shield=capture.findChild<QWidget *>("linuxToolbarLayoutOverlay");
        if (!shield || shield->geometry()!=capture.rect() || !shield->rect().contains(dialog->geometry())) return fail("panel escaped resized output");
        // 点击面板外再按 Esc 也要能退出；宿主收到 Esc 时同样先关闭调整，不取消整次截图。
        // Escape must work after a background click and when delivered directly to the capture host.
        QTest::mouseClick(shield,Qt::LeftButton,Qt::NoModifier,QPoint(2,2));
        QTest::keyClick(shield,Qt::Key_Escape); flush();
        if (bar->preview_layout_dialog_open() || cancelled) return fail("background Escape failed");
        dialog=open(); QTest::keyClick(&capture,Qt::Key_Escape); flush();
        if (bar->preview_layout_dialog_open() || cancelled) return fail("host Escape cancelled capture instead of editor");
        dialog=open(); dialog->findChild<QPushButton *>("toolbarLayoutClose")->click(); flush();
        if (bar->preview_layout_dialog_open() || !view->isEnabled() || !bar->isEnabled()) return fail("close button left background disabled");
        // 保存失败也不能新建模态错误窗：保留面板供重试或取消，避免在错误路径重现假死。
        // Failed persistence stays inline and retryable, without reintroducing a native modal error dialog.
        bar->set_preview_layout_storage([&] { return settings.screenshotToolbarLayout(); },[](const QJsonArray &) { return false; });
        dialog=open(); QSignalSpy accepted(dialog,&QDialog::accepted); dialog->accept(); QCoreApplication::processEvents();
        if (!dialog->isVisible() || !accepted.isEmpty() || QApplication::activeModalWidget()) return fail("save failure closed editor or opened a modal");
        QTest::keyClick(dialog,Qt::Key_Escape); flush();
        QTest::keyClick(view,Qt::Key_Escape); flush();
        if (cancelled!=1) return fail("Escape could not cancel capture after dismissing editor");
    }
    // 从真实截图画布点击“更多 → 调整”，验证排序、模式、取消、恢复及跨会话持久化。
    // Enter customization through the real capture canvas; verify editing, cancellation and persistence.
    {
        namespace tl=jietuba::toolbar_layout;
        const auto fail=[](const char *reason) { std::fprintf(stderr,"Toolbar layout: %s\n",reason); return 1; };
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("toolbar.ini"));
        QImage background(1000,700,QImage::Format_RGB32); background.fill(Qt::white);
        CapturePreview capture(background,&settings); capture.resize(1200,850); capture.show(); QTest::qWait(20);
        auto *view=dynamic_cast<PreviewView *>(capture.findChild<QGraphicsView *>());
        auto *bar=capture.findChild<jietuba::Toolbar *>();
        if (!view || !bar) return fail("missing capture components");
        const QRectF crop(20,20,900,600);
        view->canvas->selection_model->initialize_confirmed_rect(crop); view->selectionChanged(); QCoreApplication::processEvents();
        bar->rect_btn->click();
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(QPointF(100,100)));
        QTest::mouseMove(view->viewport(),view->mapFromScene(QPointF(200,180)),10);
        QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->mapFromScene(QPointF(200,180)));
        QTest::qWait(15);
        const int undoCount=view->canvas->undo_stack->count();
        if (undoCount<1) return fail("annotation setup failed");
        const QImage before=jietuba::ExportService(view->canvas).export_image(crop);
        const auto open=[&]() -> jietuba::ToolbarLayoutDialog * {
            bar->more_btn->click();
            if (!bar->more_popup() || !bar->more_popup()->adjust_btn->isEnabled()) return nullptr;
            bar->more_popup()->adjust_btn->click(); QCoreApplication::processEvents();
            return capture.findChild<jietuba::ToolbarLayoutDialog *>("linuxToolbarLayoutDialog");
        };
        const auto flush=[] { QCoreApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); };
        const auto rows=[](jietuba::ToolbarLayoutDialog *dialog) {
            QMap<QString,jietuba::ToolbarLayoutRow *> result;
            for (auto *row:dialog->findChild<jietuba::ReorderableRowList *>()->rows()) {
                auto *entry=static_cast<jietuba::ToolbarLayoutRow *>(row); result[entry->key]=entry;
            }
            return result;
        };
        auto *dialog=open();
        if (!dialog || !bar->preview_layout_dialog_open() || bar->more_popup()->isVisible()) return fail("adjust entry did not open original dialog");
        auto entries=rows(dialog);
        if (entries.contains("confirm") || entries.size()!=tl::default_order().size()-1) return fail("locked confirm is editable");
        auto *row=entries.value("rect");
        auto *list=dialog->findChild<jietuba::ReorderableRowList *>();
        const QPoint target=list->rows().first()->mapToGlobal(QPoint(0,1));
        // 发送携带左键状态的真实拖动事件；只换顺序时必须立即重排，不能命中旧宽度缓存。
        // Drag with a held left button; same-width reorder must invalidate the old layout cache.
        QTest::mousePress(row->grip,Qt::LeftButton,Qt::NoModifier,row->grip->rect().center());
        QMouseEvent moved(QEvent::MouseMove,row->grip->mapFromGlobal(target),target,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(row->grip,&moved);
        QTest::mouseRelease(row->grip,Qt::LeftButton,Qt::NoModifier,row->grip->mapFromGlobal(target));
        dialog->accept(); flush();
        if (bar->preview_layout_dialog_open() || bar->rect_btn->x()>=bar->long_screenshot_btn->x()) return fail("same-width reorder did not apply");
        if (settings.screenshotToolbarLayout().first().toArray().first().toString()!="rect") return fail("order not saved");
        dialog=open(); if (!dialog) return fail("cannot reopen customization");
        entries=rows(dialog); entries["save"]->set_mode(tl::HIDE); entries["pen"]->set_mode(tl::MORE);
        dialog->accept(); flush();
        if (!bar->save_btn->isHidden() || !bar->confirm_btn->isVisible()) return fail("hide mode or mandatory confirm failed");
        const QJsonArray saved=settings.screenshotToolbarLayout();
        bar->more_btn->click();
        if (bar->pen_btn->parentWidget()!=bar->more_popup() || !bar->pen_btn->isVisible()) return fail("folded pen missing from More");
        bar->more_popup()->adjust_btn->click(); QCoreApplication::processEvents();
        dialog=capture.findChild<jietuba::ToolbarLayoutDialog *>("linuxToolbarLayoutDialog");
        if (!dialog) return fail("cannot adjust while a tool is folded");
        entries=rows(dialog); entries["save"]->set_mode(tl::SHOW); dialog->reject(); flush();
        if (settings.screenshotToolbarLayout()!=saved || !bar->save_btn->isHidden()) return fail("cancel persisted changes");
        // 新窗口/设置实例真实读取已写入文件的布局；屏幕变窄再变宽也不能复活隐藏项。
        // Recreate storage and canvas from disk; resize overflow must not resurrect hidden actions.
        LinuxSettings restarted(directory.filePath("toolbar.ini"));
        CapturePreview next(background,&restarted); next.resize(600,500); next.show(); QTest::qWait(20);
        auto *nextView=dynamic_cast<PreviewView *>(next.findChild<QGraphicsView *>());
        auto *nextBar=next.findChild<jietuba::Toolbar *>();
        nextView->canvas->selection_model->initialize_confirmed_rect(crop); nextView->selectionChanged(); QCoreApplication::processEvents();
        if (!nextBar->save_btn->isHidden() || !nextBar->confirm_btn->isVisible() || nextBar->width()>next.width()) return fail("narrow/restarted canvas layout failed");
        next.resize(1200,850); QTest::qWait(20);
        if (!nextBar->save_btn->isHidden() || nextBar->rect_btn->x()>=nextBar->long_screenshot_btn->x()) return fail("expansion lost custom order");
        dialog=open(); if (!dialog) return fail("cannot restore defaults");
        QPushButton *restore=nullptr;
        for (auto *button:dialog->findChildren<QPushButton *>()) if (button->text().contains("Restore defaults")) restore=button;
        if (!restore) return fail("missing original Restore defaults button");
        restore->click(); restore->click();
        if (dialog->entries()!=tl::default_layout()) return fail("restore did not restore original layout");
        dialog->accept(); flush();
        if (settings.screenshotToolbarLayout()!=tl::default_layout() || bar->save_btn->isHidden()) return fail("restored layout not applied or saved");
        if (view->canvas->selection_model->rect()!=crop || view->canvas->undo_stack->count()!=undoCount
            || jietuba::ExportService(view->canvas).export_image(crop)!=before) return fail("customization modified annotations or selection");
        // 配置异常也走原版规范化规则，重复/未知按钮被清理，确认不能隐藏。
        // Normalize malformed, duplicate and unknown entries; Confirm cannot be hidden.
        settings.saveScreenshotToolbarLayout(QJsonArray{QJsonArray{"confirm","hide"},QJsonArray{"pen","invalid"},QJsonArray{"pen","more"},QJsonArray{"unknown","show"}});
        bar->reload_layout();
        if (!bar->confirm_btn->isVisible() || !bar->pen_btn->isVisible()) return fail("invalid settings were not normalized");
    }
    // GIF 控件复用原版视觉：状态更新不会误发开始/暂停信号，帧率和倒计时真正持久化。
    // GIF controls share the original visuals; state refresh is silent and preferences persist.
    {
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("recording.ini"));
        LinuxGifToolbar toolbar(settings);
        toolbar.adjustSize(); toolbar.show(); QTest::qWait(10);
        // 可选的离屏视觉检查，不改变正常测试输出，也不访问真实桌面。
        // Optional offscreen visual snapshot, without changing normal test output or accessing the desktop.
        const QString snapshot = qEnvironmentVariable("JIETUBA_GIF_UI_SNAPSHOT");
        if (!snapshot.isEmpty() && !toolbar.grab().save(snapshot)) return 259;
        auto *start = toolbar.findChild<QPushButton *>("gifStartStop");
        auto *pause = toolbar.findChild<QPushButton *>("gifPause");
        auto *fps = toolbar.findChild<jietuba::ClickMenuButton *>("gifFps");
        auto *delay = toolbar.findChild<QPushButton *>("gifDelay");
        if (!start || !pause || !fps || !delay || toolbar.height()!=40 || start->icon().isNull()) return 240;
        QSignalSpy requests(&toolbar,&LinuxGifToolbar::startStopRequested), pauses(&toolbar,&LinuxGifToolbar::pauseRequested);
        if (!start->isEnabled() || pause->isEnabled()) return 241;
        // 操作真实弹出菜单，验证原版菜单组件和新的时间入口，而非直接改测试状态。
        // Exercise actual shared popup menus rather than assigning test-only state.
        const auto choose = [&](QPushButton *button,int value) {
            button->click(); QCoreApplication::processEvents();
            for (auto *menu:toolbar.findChildren<QMenu *>()) if (menu->isVisible()) {
                for (auto *action:menu->actions()) if (action->data().isValid() && action->data().toInt()==value) {
                    action->trigger(); menu->close(); QCoreApplication::processEvents(); return true;
                }
            }
            return false;
        };
        if (!choose(fps,24) || !choose(delay,3)) return 249;
        LinuxSettings restored(directory.filePath("recording.ini"));
        if (restored.gifFps()!=24 || restored.gifDelay()!=3) return 242;
        using M = jietuba::RecordToolbarMetrics;
        auto *grip = toolbar.findChild<QLabel *>("gifMoveHandle");
        if (!grip || grip->size()!=QSize(M::HANDLE_WIDTH,M::BUTTON_HEIGHT) || grip->pixmap().isNull()) return 250;
        int drawings=0;
        for (const auto &tool:jietuba::RECORDING_DRAW_TOOLS) {
            auto *button=toolbar.findChild<QPushButton *>(QStringLiteral("gifDraw_")+QString::fromLatin1(tool.id));
            if (!button || button->isEnabled() || button->size()!=QSize(M::BUTTON_WIDTH,M::BUTTON_HEIGHT) || button->icon().isNull()) return 251;
            ++drawings;
        }
        if (drawings!=6 || toolbar.findChildren<QWidget *>("gifSeparator").size()!=2 || toolbar.findChildren<QComboBox *>().size()!=0) return 252;
        const QSize output(1500,1000), rowSize=toolbar.sizeHint();
        for (const QRect &selection:{QRect(100,100,1000,500),QRect(200,700,800,300),QRect(0,0,1000,600)}) {
            const QRect placed=LinuxGifToolbar::position(output,selection,rowSize);
            if (placed.isEmpty() || placed.size()!=rowSize || placed.intersects(selection) || !QRect(QPoint(),output).contains(placed)) return 253;
        }
        if (!LinuxGifToolbar::position(output,QRect(QPoint(),output),rowSize).isEmpty()) return 254;
        if (!LinuxGifToolbar::position(QSize(200,100),QRect(10,10,80,60),rowSize).isEmpty()) return 255;
        toolbar.setState("00:02.0",true,false,false);
        if (fps->isEnabled() || delay->isEnabled() || !pause->isEnabled() || start->icon().isNull()) return 243;
        if(!grip->isEnabled()) return 1;
        toolbar.setState("00:02.0",true,true,false);
        if (!requests.isEmpty() || !pauses.isEmpty() || pause->icon().isNull()) return 244;
        start->click(); pause->click(); if (requests.size()!=1 || pauses.size()!=1) return 245;
        toolbar.setState("倒计时 3",false,false,true);
        if (start->isEnabled() || pause->isEnabled() || fps->isEnabled()) return 246;
        // 预览在高 DPI 下使用足够的物理像素，小画面不自动放大；导出原帧不变。
        // HiDPI preview retains physical resolution and never enlarges small source frames.
        QImage source(1200,800,QImage::Format_RGB32); source.fill(Qt::blue);
        const QPixmap retina=LinuxGifPreviewLabel::fittedPixmap(source,QSize(600,400),2.0);
        if (retina.size()!=source.size() || retina.devicePixelRatio()!=2.0) return 256;
        const QPixmap small=LinuxGifPreviewLabel::fittedPixmap(source,QSize(2000,1600),1.5);
        if (small.size()!=source.size() || small.devicePixelRatio()!=1.5) return 257;
        const QPixmap down=LinuxGifPreviewLabel::fittedPixmap(source,QSize(300,200),1.5);
        if (down.size()!=QSize(450,300)) return 258;
    }
    // 全屏录制保留完整工具条与画布，采集开始后仍可操作。
    // Full-screen recording retains its complete toolbar and canvas during capture.
    {
        QTemporaryDir directory;
        LinuxSettings settings(directory.filePath("fullscreen-recording.ini"));
        auto *screen = app.primaryScreen();
        const QRect full(QPoint(), screen->geometry().size());
        QPointer<LinuxGifCaptureWindow> recording;
        QWidget *surface = nullptr;
        bool visibleAtCapture = false;
        recording = new LinuxGifCaptureWindow(screen, full.size(), full, settings, true, nullptr, full, [&] {
            visibleAtCapture = surface->isVisible();
            recording->cancel();
        });
        recording->showControls();
        for (auto *window : app.topLevelWidgets())
            if (window->isVisible() && window->objectName() == "linuxRecordingSurface") surface = window;
        auto *bar = surface ? surface->findChild<LinuxGifToolbar *>() : nullptr;
        auto *hint = bar ? bar->findChild<QLabel *>("recordingTrayHint") : nullptr;
        auto *grip = bar ? bar->findChild<QLabel *>("gifMoveHandle") : nullptr;
        if (!bar || !surface->windowFlags().testFlag(Qt::FramelessWindowHint) || !hint || hint->isVisible()
            || !grip || !grip->isVisible() || bar->geometry().center().y() <= full.center().y()
            || !full.contains(bar->geometry())) {
            const auto geometry = bar ? bar->geometry() : QRect{};
            std::fprintf(stderr, "Full-screen recording: surface=%d, frameless=%d, bar=(%d,%d %dx%d), screen=%dx%d, hint=%d, grip=%d\n",
                         surface != nullptr, surface && surface->windowFlags().testFlag(Qt::FramelessWindowHint),
                         geometry.x(), geometry.y(), geometry.width(), geometry.height(), full.width(), full.height(),
                         hint && hint->isVisible(), grip && grip->isVisible());
            return 1;
        }
        auto *pen = bar->findChild<QPushButton *>("gifDraw_pen");
        if (!pen || !pen->isVisible() || !pen->isEnabled()
            || !surface->findChild<QWidget *>("linuxRecordingCanvas")) return 1;
        if (!LinuxGifFrames::ffmpegPath().isEmpty()) {
            bar->findChild<QPushButton *>("gifStartStop")->click();
            QElapsedTimer wait;
            wait.start();
            while (recording && wait.elapsed() < 2000) QTest::qWait(10);
            if (recording || !visibleAtCapture) {
                std::fprintf(stderr, "Full-screen recording: full toolbar disappeared during full-screen capture\n");
                return 1;
            }
        } else {
            recording->cancel();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
    // 录制标注复用真实画布：六种工具、原版样式面板、撤销重做以及采集区域移动。
    // Exercise the shared recording canvas, all six tools, original panels, undo/redo and crop movement.
    {
        const auto fail=[](const char *reason) {std::fprintf(stderr,"GIF drawing: %s\n",reason); return 1;};
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("gif-drawing.ini")); settings.setSaveDirectory(directory.path());
        auto *screen=app.primaryScreen(); const QSize pixels=screen->geometry().size()*1.25;
        const QRect logical(100,100,200,160),native(125,125,250,200);
        QPointer<LinuxGifCaptureWindow> recording;
        QImage background(native.size(),QImage::Format_RGB32); background.fill(Qt::white);
        recording=new LinuxGifCaptureWindow(screen,pixels,native,settings,true,nullptr,logical,[&] {
            QImage image=background; image.setDevicePixelRatio(1.25);
            QMetaObject::invokeMethod(recording,"acceptFrame",Qt::QueuedConnection,Q_ARG(QImage,image));
        });
        recording->showControls();
        QWidget *surface=nullptr;
        for(auto *widget:app.topLevelWidgets()) if(widget->isVisible() && widget->objectName()=="linuxRecordingSurface") surface=widget;
        if(!surface) return fail("recording surface unavailable");
        auto *canvas=dynamic_cast<LinuxAnnotationCanvas *>(surface->findChild<QWidget *>("linuxRecordingCanvas"));
        auto *view=canvas ? dynamic_cast<PreviewView *>(canvas->findChild<QGraphicsView *>()) : nullptr;
        auto *bar=surface->findChild<LinuxGifToolbar *>();
        if(!canvas || !view || !bar || !canvas->recordingImage().isNull()) return fail("transparent canvas exported UI decorations");
        const auto tool=[&](const char *id) {bar->findChild<QPushButton *>(QStringLiteral("gifDraw_")+id)->click(); QCoreApplication::processEvents();};
        const auto drag=[&](QPoint from,QPoint to) {
            const auto start=view->mapFromScene(from),end=view->mapFromScene(to);
            QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,start);
            QMouseEvent move(QEvent::MouseMove,QPointF(end),QPointF(view->viewport()->mapToGlobal(end)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QCoreApplication::sendEvent(view->viewport(),&move);
            QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,end);
        };
        for(const auto &entry:jietuba::RECORDING_DRAW_TOOLS)
            if(!bar->findChild<QPushButton *>(QStringLiteral("gifDraw_")+entry.id)->isEnabled()) return fail("draw button disabled");
        if(!surface->mask().isEmpty()) return fail("recording still clips widget painting with its input mask");
        if(surface->windowHandle()->mask().contains(canvas->geometry().center())) return fail("cursor mode blocks the recorded application");
        tool("pen");
        auto *panel=surface->findChild<jietuba::PaintSettingsPanel *>("linuxPenPanel");
        if(!panel || !panel->isVisible() || !surface->windowHandle()->mask().contains(canvas->geometry().center())) return fail("pen panel or drawing input missing");
        panel->size_spin->setValue(8);
        QMetaObject::invokeMethod(panel,"color_changed",Q_ARG(QColor,QColor(Qt::red)));
        drag(QPoint(25,30),QPoint(100,30));
        const auto stroke=canvas->recordingImage();
        if(stroke.isNull() || stroke.size()!=native.size() || stroke.pixelColor(60,30).alpha()==0 || stroke.pixelColor(0,0).alpha()!=0)
            return fail("pen missed native pixels or exported crop decorations");
        bar->findChild<QPushButton *>("gifUndo")->click();
        if(!canvas->recordingImage().isNull()) return fail("undo did not remove stroke");
        bar->findChild<QPushButton *>("gifRedo")->click();
        if(canvas->recordingImage()!=stroke) return fail("redo did not restore stroke");
        tool("rect"); drag(QPoint(125,20),QPoint(175,55));
        tool("ellipse"); drag(QPoint(125,90),QPoint(175,125));
        tool("arrow"); drag(QPoint(30,110),QPoint(85,150));
        tool("text"); drag(QPoint(20,170),QPoint(20,170)); QTest::keyClicks(view,"GIF");
        tool("pen"); // 提交文字 / Commit text.
        int shapes=0; for(auto *item:view->canvas->items()) if(jietuba::annotation_owner(item)==item && item!=view->canvas->background && item!=view->canvas->selection_item) ++shapes;
        if(shapes<5) return fail("shape or text tool failed");
        tool("eraser"); drag(QPoint(60,30),QPoint(60,30));
        if(canvas->recordingImage().pixelColor(60,30).alpha()!=0) return fail("eraser did not remove the annotation");
        bar->findChild<QPushButton *>("gifUndo")->click();
        const auto annotations=canvas->recordingImage();
        canvas->activateRecordingTool("cursor");
        if(surface->windowHandle()->mask().contains(canvas->geometry().center())) return fail("cursor did not restore click-through");
        // 通过实际倒数第二个按钮移动选区，原生表面保持原位，标注内容和尺寸不变。
        // Drag the real grip: move the crop while keeping the native surface and annotation document stable.
        const auto surfaceGeometry=surface->geometry(); auto *grip=bar->findChild<QLabel *>("gifMoveHandle");
        const auto point=grip->mapToGlobal(QPoint(5,5));
        QTest::mousePress(grip,Qt::LeftButton,Qt::NoModifier,QPoint(5,5));
        QMouseEvent move(QEvent::MouseMove,QPointF(35,25),QPointF(point+QPoint(30,20)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QCoreApplication::sendEvent(grip,&move); QTest::mouseRelease(grip,Qt::LeftButton,Qt::NoModifier,QPoint(5,5));
        if(recording->logicalCaptureRegion()!=logical.translated(30,20) || canvas->geometry()!=logical.translated(30,20)
            || surface->geometry()!=surfaceGeometry || canvas->recordingImage()!=annotations) return fail("grip moved only controls or lost annotations");
        const QPoint oldBorder=logical.topLeft()-QPoint(4,4);
        if(surface->grab().toImage().pixelColor(oldBorder).alpha()!=0) return fail("moving the recording retained an old border");
        QSignalSpy captured(recording,&LinuxGifCaptureWindow::frameCaptured);
        recording->toggle(); QElapsedTimer wait; wait.start();
        while(captured.size()<2 && wait.elapsed()<3000) QTest::qWait(10);
        if(captured.size()<2) return fail("annotated recording never captured");
        recording->beginRegionMove(); recording->moveRegion(QPoint(20,10)); recording->endRegionMove();
        if(recording->logicalCaptureRegion()!=logical.translated(50,30)) return fail("recording-state crop did not move");
        recording->stop();
        auto *completed=bar->findChild<QWidget *>("videoResultControls"); wait.restart();
        while(!completed->isVisible() && wait.elapsed()<6000) QTest::qWait(10);
        if(!completed->isVisible()) return fail("GIF result toolbar missing");
        auto *preview=surface->findChild<LinuxRecordingResultPreview *>();
        if(!preview || !preview->isVisible() || preview->isWindow()) return fail("unified annotated preview unavailable");
        wait.restart(); while(preview->currentFrame().isNull() && wait.elapsed()<3000) QTest::qWait(10);
        const auto files=QDir(directory.path()).entryList({"*.gif"},QDir::Files);
        if(files.size()!=1) return fail("GIF was not saved automatically");
        const auto decoded=QImageReader(QDir(directory.path()).filePath(files.first())).read();
        const auto hasAnnotations = [](const QImage &frame) {
            const auto stroke = frame.pixelColor(60, 30);
            return stroke.red()>200 && stroke.green()<80 && frame.pixelColor(0,0)==QColor(Qt::white);
        };
        if(decoded.size()!=native.size() || preview->currentFrame().size()!=native.size()
            || !hasAnnotations(decoded) || !hasAnnotations(preview->currentFrame()))
            return fail("saved preview omitted annotations or included editor UI");
        if(preview->isPlaying()) return fail("GIF first frame started playing automatically");
        const auto saved=QDir(directory.path()).filePath(files.first());
        QPointer<QWidget> closedSurface=surface; QPointer<LinuxRecordingResultPreview> closedPreview=preview;
        RecordingFolderProbe folder; auto bus=QDBusConnection::sessionBus();
        if(!bus.registerService("org.freedesktop.FileManager1") || !bus.registerObject("/org/freedesktop/FileManager1",&folder,QDBusConnection::ExportAllSlots))
            return fail("private file manager registration failed");
        bar->findChild<QPushButton *>("videoResult_1")->click();
        if(!recording) return fail("folder action closed before acknowledgement");
        wait.restart(); while(recording && wait.elapsed()<3000) {QTest::qWait(10); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
        bus.unregisterObject("/org/freedesktop/FileManager1"); bus.unregisterService("org.freedesktop.FileManager1");
        if(folder.urls!=QStringList{QUrl::fromLocalFile(saved).toString(QUrl::FullyEncoded)} || recording || closedSurface || closedPreview || !QFileInfo::exists(saved))
            return fail("opening the folder did not close playback or removed the saved file");
    }
    // MP4 的新增按钮、共用设置表单、自动保存、复制文件与重新录制使用真实控件和编码器。
    // Exercise MP4 controls, shared settings, real encoding, automatic save, clipboard and re-record.
    {
        const auto fail=[](const char *reason) {std::fprintf(stderr,"MP4 recording: %s\n",reason); return 1;};
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("mp4.ini")); settings.setSaveDirectory(directory.path()); settings.setGifFps(10);
        auto options=settings.videoOptions(); options.system_audio=false; options.hardware=false; settings.setVideoOptions(options); settings.sync();
        auto *screen=app.primaryScreen(); const QSize pixels=screen->geometry().size()*1.25;
        const QRect logical(100,100,200,160),native(125,125,250,200);
        QPointer<LinuxGifCaptureWindow> recording;
        QImage white(native.size(),QImage::Format_RGB32); white.fill(Qt::white);
        int sourceFrame=0;
        recording=new LinuxGifCaptureWindow(screen,pixels,native,settings,true,nullptr,logical,[&] {
            // 各帧颜色不同，确保默认显示的确实是首帧，且暂停后没有悄悄推进。
            // Distinct frame colors verify that the initial still is frame zero and does not advance.
            QImage frame=white; const int phase=sourceFrame++%3;
            if(phase) frame.fill(phase==1 ? Qt::red : Qt::blue);
            frame.setDevicePixelRatio(1.25); QMetaObject::invokeMethod(recording,"acceptFrame",Qt::QueuedConnection,Q_ARG(QImage,frame));
        });
        recording->showControls(); QWidget *surface=nullptr;
        for(auto *widget:app.topLevelWidgets()) if(widget->isVisible() && widget->objectName()=="linuxRecordingSurface") surface=widget;
        if(!surface) return fail("surface missing");
        auto *bar=surface->findChild<LinuxGifToolbar *>(); auto *mode=bar->findChild<jietuba::ClickMenuButton *>("gifFormat");
        mode->click(); bool selected=false;
        for(auto *menu:mode->findChildren<QMenu *>()) if(menu->isVisible()) for(auto *action:menu->actions()) if(action->data().toString()=="mp4") {action->trigger(); menu->close(); selected=true; break;}
        QCoreApplication::processEvents();
        if(!selected || bar->recordingMode()!="mp4" || !bar->findChild<QPushButton *>("videoAudioMenu")->isVisible()
            || !bar->findChild<QPushButton *>("videoCursorToggle")->isVisible() || !bar->findChild<QPushButton *>("videoSettings")->isVisible()) return fail("MP4-specific controls missing");
        bar->findChild<QPushButton *>("videoCursorToggle")->click();
        if(settings.videoOptions().cursor) return fail("cursor preference did not change");
        bar->findChild<QPushButton *>("videoSettings")->click(); QCoreApplication::processEvents();
        auto *dialog=surface->findChild<QDialog *>("linuxVideoSettings");
        if(!dialog || dialog->isWindow() || app.activeModalWidget() || !surface->windowHandle()->mask().contains(dialog->geometry().center())) return fail("video settings are inaccessible on the layer surface");
        dialog->findChild<QSpinBox *>("videoFps")->setValue(24);
        dialog->findChild<QDoubleSpinBox *>("videoBitrate")->setValue(8);
        dialog->accept(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(bar->fps()!=24 || settings.videoOptions().bitrate!=8000000 || settings.gifFps()!=10) return fail("video settings changed GIF defaults or failed to save");
        QSignalSpy captured(recording,&LinuxGifCaptureWindow::frameCaptured);
        QSignalSpy trayActions(recording, &LinuxGifCaptureWindow::controlsChanged);
        recording->toggle(); QElapsedTimer wait; wait.start();
        while(captured.size()<3 && wait.elapsed()<3000) QTest::qWait(10);
        if(captured.size()<3) return fail("MP4 never captured");
        const auto hasTrayActions = [&trayActions](const QString &toggle, const QString &stop) {
            return !trayActions.isEmpty() && trayActions.last() == QVariantList{toggle, stop};
        };
        if (!hasTrayActions(QStringLiteral("暂停 MP4 录制"), QStringLiteral("停止 MP4 录制")))
            return fail("incorrect tray actions while recording MP4");
        recording->toggle();
        if (!hasTrayActions(QStringLiteral("继续 MP4 录制"), QStringLiteral("停止 MP4 录制")))
            return fail("incorrect tray actions while MP4 is paused");
        recording->stop(); wait.restart();
        const auto *progress = surface->findChild<QWidget *>("recordingFinishingProgress");
        const auto available = screen->availableGeometry().translated(-screen->geometry().topLeft());
        if (!progress || !progress->isVisible() || !available.contains(progress->geometry())
            || qAbs(progress->geometry().center().x() - available.center().x()) > 1
            || progress->geometry().bottom() < available.bottom() - 32
            || !progress->findChild<QProgressBar *>() || !hasTrayActions({}, {}))
            return fail("finishing progress missing, misplaced or left stale tray actions");
        const auto cleared = surface->grab().toImage();
        if (bar->isVisible() || cleared.pixelColor(logical.topLeft() - QPoint(4,4)).alpha() != 0)
            return fail("encoding progress retained recording controls or crop outline");
        auto *result=bar->findChild<QWidget *>("videoResultControls");
        while(!result->isVisible() && wait.elapsed()<6000) QTest::qWait(10);
        if(!result->isVisible() || surface->findChild<QWidget *>("recordingFinishingProgress"))
            return fail("MP4 preview did not replace finishing progress");
        for(int i=0;i<4;++i) if(!bar->findChild<QPushButton *>(QStringLiteral("videoResult_%1").arg(i))->isEnabled()) return fail("result action disabled");
        for(int i=0;i<4;++i) {
            auto *button=bar->findChild<QPushButton *>(QStringLiteral("videoResult_%1").arg(i));
            if(button->icon().isNull() || !button->text().isEmpty() || button->toolTip().isEmpty() || button->accessibleName().isEmpty()) return fail("result icon or accessibility hint missing");
        }
        auto *preview=surface->findChild<LinuxRecordingResultPreview *>();
        if(!preview || !preview->isVisible() || preview->isWindow() || app.activeModalWidget()) return fail("MP4 embedded preview missing");
        wait.restart(); while(preview->currentFrame().isNull() && wait.elapsed()<4000) QTest::qWait(10);
        const auto firstColor=preview->currentFrame().pixelColor(80,60);
        if(preview->currentFrame().size()!=white.size() || firstColor.red()<240 || firstColor.green()<240 || firstColor.blue()<240) return fail("MP4 saved-file first frame differs");
        auto *play=bar->findChild<QPushButton *>("videoResult_0");
        if(preview->isPlaying()) return fail("MP4 preview did not remain paused on the first frame");
        const auto initial=preview->currentFrame(); QTest::qWait(150);
        if(preview->isPlaying() || preview->currentFrame()!=initial) return fail("MP4 default still frame advanced");
        if(!available.contains(preview->geometry()) || preview->geometry().intersects(bar->geometry())
            || qAbs(preview->geometry().center().x()-available.center().x())>1 || preview->geometry().center().y()<=available.center().y()
            || bar->geometry().top()<=preview->geometry().bottom()) return fail("result is not centered slightly below with the toolbar underneath");
        play->click(); if(!preview->isPlaying()) return fail("MP4 playback action failed");
        play->click(); if(preview->isPlaying()) return fail("MP4 pause action failed");
        if(!surface->windowHandle()->mask().contains(preview->geometry().center())) return fail("result preview has no input region");
        const auto savedFiles=QDir(directory.path()).entryList({"*.mp4"},QDir::Files);
        if(savedFiles.size()!=1) return fail("MP4 result file missing");
        const auto firstPath=QDir(directory.path()).filePath(savedFiles.first());
        const auto unrelated=directory.filePath("unrelated.mp4"); QFile preserved(unrelated);
        if(!preserved.open(QIODevice::WriteOnly)) return fail("cannot create unrelated recording"); preserved.write("keep"); preserved.close();
        bar->findChild<QPushButton *>("videoResult_3")->click();
        auto *canvas=dynamic_cast<LinuxAnnotationCanvas *>(surface->findChild<QWidget *>("linuxRecordingCanvas"));
        if(surface->findChild<LinuxRecordingResultPreview *>() || result->isVisible() || !bar->findChild<QPushButton *>("gifStartStop")->isEnabled() || !canvas->recordingImage().isNull() || QFileInfo::exists(firstPath) || !QFileInfo::exists(unrelated)) return fail("re-record did not delete only the current saved result");
        // 再录一次后直接复制应关闭整个会话，仅保留本次视频和其他无关文件。
        // Copy the second recording and close, retaining the new video and unrelated files.
        captured.clear(); recording->toggle(); wait.restart();
        while(captured.size()<3 && wait.elapsed()<3000) QTest::qWait(10);
        if(captured.size()<3) return fail("MP4 re-record did not capture");
        recording->stop(); wait.restart();
        while(!result->isVisible() && wait.elapsed()<6000) QTest::qWait(10);
        if(!result->isVisible() || !surface->findChild<LinuxRecordingResultPreview *>() || surface->findChild<LinuxRecordingResultPreview *>()->isPlaying()) return fail("second MP4 result did not show a paused preview");
        QPointer<QWidget> closedSurface=surface;
        bar->findChild<QPushButton *>("videoResult_2")->click();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        const auto *mime=app.clipboard()->mimeData();
        if(recording || closedSurface || !mime->hasFormat("video/mp4") || !mime->hasUrls()) return fail("MP4 copy did not close or lost clipboard data");
        const auto path=mime->urls().first().toLocalFile(); QFile file(path);
        if(path==firstPath || QFileInfo::exists(firstPath) || !QFileInfo::exists(unrelated) || !file.open(QIODevice::ReadOnly) || file.readAll()!=mime->data("video/mp4")) return fail("MP4 copy removed a saved file or changed bytes");
        app.clipboard()->clear();
    }
    // 工具栏必须在采集区域之外；分数缩放时选区要向外取整，防止污染导出像素。
    // Controls must stay outside the crop, including at fractional output scales.
    {
        const QSize output(1200, 800), pixels(1500, 1000);
        for (const QRect &native : {QRect(100, 100, 1000, 500), QRect(0, 600, 1200, 400),
                                     QRect(500, 0, 1000, 1000), QRect(0, 0, 1000, 1000)}) {
            const QRect area = LinuxScrollCaptureOverlay::selectionInLogicalPixels(output, pixels, native);
            const QRect toolbar = LinuxScrollCaptureOverlay::toolbarPosition(output, area);
            if (area.isEmpty() || toolbar.isEmpty() || toolbar.intersects(area) || !QRect(QPoint(), output).contains(toolbar)) return 207;
        }
        const QRect area(100, 100, 700, 500);
        const QRect toolbar = LinuxScrollCaptureOverlay::toolbarPosition(output, area);
        const QRect preview = LinuxScrollCaptureOverlay::previewPosition(output, area, toolbar);
        if (preview.width() < 110 || preview.height() < 100 || preview.intersects(area) || preview.intersects(toolbar)) return 216;
        const QRect fractional = LinuxScrollCaptureOverlay::selectionInLogicalPixels(output, pixels, QRect(1, 1, 101, 101));
        if (fractional != QRect(0, 0, 82, 82)) return 208;
        if (!LinuxScrollCaptureOverlay::toolbarPosition(output, QRect(QPoint(), output)).isEmpty()) return 209;
    }
    // Linux 使用原版 FloatingToolbar 本体：验证按钮规格、背景组件和嵌入窗口角色。
    // Verify the shared toolbar's original button metrics and its embedded surface role.
    {
        QWidget host;
        jietuba::FloatingToolbar toolbar(&host, true);
        toolbar.set_result_size(639, 4448); toolbar.adjustSize();
        host.resize(toolbar.size()); host.show(); toolbar.show(); QTest::qWait(20);
        if (toolbar.isWindow() || toolbar.height() != 40 || toolbar.save_btn->size() != QSize(42, 40)) return 219;
        if (toolbar.size_label->text() != QStringLiteral("639 × 4448") || toolbar.save_btn->icon().isNull()) return 220;
        QSignalSpy direction(&toolbar, &jietuba::FloatingToolbar::direction_changed);
        QSignalSpy automatic(&toolbar, &jietuba::FloatingToolbar::auto_scroll_clicked);
        QSignalSpy cropped(&toolbar, &jietuba::FloatingToolbar::crop_requested);
        QSignalSpy interaction(&toolbar, &jietuba::FloatingToolbar::interaction_active);
        toolbar.direction_btn->click(); toolbar.auto_scroll_btn->click();
        if (direction.size()!=1 || automatic.size()!=1 || !toolbar.auto_scroll_btn->isChecked()) return 226;
        toolbar.set_auto_scrolling(false);
        QTimer::singleShot(0,&toolbar,[&] {
            for(auto *menu:toolbar.findChildren<QMenu *>()) if(menu->isVisible()) { menu->actions().first()->trigger(); menu->close(); }
        });
        toolbar.crop_btn->click();
        if(cropped.size()!=1 || cropped.first()[0].toString()!="top" || interaction.size()!=2 ||
           !interaction.first()[0].toBool() || interaction.last()[0].toBool()) return 227;
        const auto renderPath = qgetenv("JIETUBA_SCROLL_TOOLBAR_TEST_RENDER");
        if (!renderPath.isEmpty()) toolbar.grab().save(QString::fromLocal8Bit(renderPath));
    }
    // 真实录制状态机：分数缩放区域帧必须写盘并打开预览，不能误按整屏尺寸拒收。
    // Exercise the recording coordinator, not only the encoder: cropped fractional-DPI frames reach preview.
    {
        const auto fail=[](const char *reason) { std::fprintf(stderr,"GIF area recording: %s\n",reason); return 1; };
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("gif-area.ini")); settings.setSaveDirectory(directory.path());
        auto *screen=app.primaryScreen();
        const QSize pixels(qRound(screen->geometry().width()*1.25),qRound(screen->geometry().height()*1.25));
        const QRect logical(121,121,97,81),native(151,151,121,101);
        QImage first(121,101,QImage::Format_RGB32);
        for(int y=0;y<first.height();++y) for(int x=0;x<first.width();++x) first.setPixel(x,y,qRgb(x*2,y*2,(x+y)%2*255));
        QPointer<LinuxGifCaptureWindow> recording;
        int frameIndex = 0;
        recording=new LinuxGifCaptureWindow(screen,pixels,native,settings,true,nullptr,logical,[&] {
            QImage image=first; image.setDevicePixelRatio(1.25);
            if (frameIndex) image.fill(frameIndex % 2 ? Qt::red : Qt::blue);
            ++frameIndex;
            QMetaObject::invokeMethod(recording,"acceptFrame",Qt::QueuedConnection,Q_ARG(QImage,image));
        });
        QSignalSpy captured(recording,&LinuxGifCaptureWindow::frameCaptured);
        recording->showControls(); recording->toggle();
        QElapsedTimer wait; wait.start();
        while(captured.size()<4 && wait.elapsed()<3000) QTest::qWait(10);
        if(captured.size()<4 || captured.first().first().toSize()!=first.size()) return fail("cropped frames were rejected");
        recording->stop();
        QWidget *surface=nullptr;
        // 编码期间表面暂时隐藏，等待同一实例重新显示结果，而不是误判其不存在。
        // The surface hides while encoding; wait for results on the same instance.
        for(auto *top:app.topLevelWidgets()) if(top->objectName()=="linuxRecordingSurface") surface=top;
        if(!surface) return fail("recording surface missing");
        auto *bar=surface->findChild<LinuxGifToolbar *>();
        auto *result=bar->findChild<QWidget *>("videoResultControls");
        wait.restart(); while(!result->isVisible() && wait.elapsed()<6000) QTest::qWait(10);
        auto *preview=surface->findChild<LinuxRecordingResultPreview *>();
        if(!result->isVisible() || !preview || !preview->isVisible() || app.activeModalWidget())
            return fail("GIF did not reach the same embedded result as MP4");
        wait.restart(); while(preview->currentFrame().isNull() && wait.elapsed()<3000) QTest::qWait(10);
        const auto files=QDir(directory.path()).entryList({"*.gif"},QDir::Files);
        if(files.size()!=1) return fail("GIF requires an extra save confirmation");
        const QString saved=QDir(directory.path()).filePath(files.first());
        QImageReader animated(saved);
        if(animated.imageCount()!=captured.size() || animated.size()!=first.size()) return fail("accepted frames were not exported at native size");
        if(preview->currentFrame()!=first || animated.read().isNull()) return fail("preview does not show the recorded first frame");
        for(auto *top:app.topLevelWidgets()) if(top->isVisible() && top->objectName()=="linuxGifPreview") return fail("legacy GIF confirmation still shown");
        auto *play=bar->findChild<QPushButton *>("videoResult_0");
        if(preview->isPlaying()) return fail("GIF did not open paused on the first frame");
        play->click(); if(!preview->isPlaying()) return fail("GIF playback action failed");
        play->click(); if(preview->isPlaying() || play->toolTip()!=QStringLiteral("播放预览")) return fail("GIF pause action failed");
        auto *timeline = preview->findChild<jietuba::RangeSlider *>("recordingResultTrim");
        auto *saveTrim = bar->findChild<QPushButton *>("gifSaveTrim");
        auto *startLabel = preview->findChild<QLabel *>("recordingTrimStart");
        auto *endLabel = preview->findChild<QLabel *>("recordingTrimEnd");
        if (!timeline || !saveTrim || !saveTrim->isVisible()
            || !startLabel || !startLabel->isVisible() || !endLabel || !endLabel->isVisible()
            || !preview->rect().contains(endLabel->geometry()) || startLabel->geometry().intersects(endLabel->geometry())
            || preview->trimRange() != std::pair(0, int(captured.size()) - 1)) return fail("GIF trim controls are missing");
        // 读取失败必须保留界面；成功后自动关闭，已复制动画仍可粘贴。
        // A failed read keeps controls open; successful copying closes playback and retains the animation.
        QFile file(saved); if(!file.open(QIODevice::ReadOnly)) return fail("saved GIF unreadable");
        const auto bytes=file.readAll(); file.close();
        if(!QFile::remove(saved)) return fail("cannot simulate missing saved file");
        bar->findChild<QPushButton *>("videoResult_2")->click(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(!recording || !result->isVisible()) return fail("failed GIF copy closed the result");
        if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size()) return fail("cannot restore GIF result"); file.close();

        // 真实拖动两个手柄，并解码剪贴板数据验证只保留对应两帧。
        // Drag both handles and decode the clipboard GIF to verify the two retained frames.
        const auto pointAt = [&](int index) {
            const int margin = jietuba::RangeSlider::BASE_HANDLE_W;
            return QPoint(margin + index * (timeline->width() - 2 * margin) / (captured.size() - 1), timeline->height() / 2);
        };
        const auto moveHandle = [&](int from, int to) {
            QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, pointAt(from));
            QTest::mouseMove(timeline, pointAt(to));
            QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, pointAt(to));
        };
        moveHandle(0, 1);
        moveHandle(int(captured.size()) - 1, 2);
        if (preview->trimRange() != std::pair(1, 2) || !startLabel->text().endsWith('2') || !endLabel->text().endsWith('3')
            || preview->currentFrame().pixelColor(40, 40) != Qt::red)
            return fail("dragging the GIF handles did not seek to the selected start");
        QSignalSpy played(preview, &LinuxRecordingResultPreview::framePresented);
        play->click();
        wait.restart();
        while (preview->isPlaying() && wait.elapsed() < 3000) QTest::qWait(10);
        if (preview->isPlaying() || played.size() != 2 || preview->currentFrame().pixelColor(40, 40) != Qt::red)
            return fail("GIF playback did not stop at the selected range");
        QPointer<LinuxRecordingResultPreview> closedPreview=preview;
        QPointer<QWidget> closedSurface=surface;
        bar->findChild<QPushButton *>("videoResult_2")->click();
        wait.restart();
        while (recording && wait.elapsed() < 6000) { QTest::qWait(10); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
        const auto *mime=app.clipboard()->mimeData();
        if(recording || closedSurface || closedPreview || !QFileInfo::exists(saved) || !mime || mime->hasUrls() || !mime->hasFormat("image/gif") || app.activeModalWidget())
            return fail("GIF copy did not close playback or invalidated clipboard data");
        QBuffer trimmed;
        trimmed.setData(mime->data("image/gif"));
        trimmed.open(QIODevice::ReadOnly);
        QImageReader decoded(&trimmed, "gif");
        if (decoded.imageCount() != 2 || decoded.read().pixelColor(40, 40) != Qt::red || decoded.read().pixelColor(40, 40) != Qt::blue)
            return fail("GIF clipboard contains frames outside the selected range");
        if (!file.open(QIODevice::ReadOnly) || file.readAll() != bytes) return fail("trimming changed the original recording");
        file.close();
        app.clipboard()->clear();

        // 首帧尺寸错误必须只报告实际原因一次，不得被空录制提示覆盖。
        // A bad first frame must report its actual mismatch exactly once, not a generic empty-recording error.
        int warnings=0; QString warning;
        QTimer dismiss;
        QObject::connect(&dismiss,&QTimer::timeout,&app,[&] {
            if(auto *box=qobject_cast<QMessageBox *>(app.activeModalWidget())) { ++warnings; warning=box->text(); box->accept(); }
        });
        dismiss.start(10);
        recording=new LinuxGifCaptureWindow(screen,pixels,native,settings,true,nullptr,logical,[&] {
            const QImage wrong(20,20,QImage::Format_RGB32);
            QMetaObject::invokeMethod(recording,"acceptFrame",Qt::QueuedConnection,Q_ARG(QImage,wrong));
        });
        recording->showControls(); recording->toggle();
        wait.restart(); while(recording && wait.elapsed()<3000) { QTest::qWait(10); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); }
        dismiss.stop();
        if(recording || warnings!=1 || !warning.contains(QStringLiteral("预期 121 × 101")) || warning.contains(QStringLiteral("没有录到")))
            return fail("actual first-frame error was hidden or duplicated");
    }
    // 四个象限分别固定对应窗口角；连续放大、缩小后该角不能累计漂移。
    // Each pointer quadrant fixes its own rectangle corner through repeated resizes.
    using Corner = PinZoomAnchor::Corner;
    struct CornerCase { QPointF pointer; Corner expected; };
    for (const CornerCase sample : {CornerCase{QPointF(20, 20), Corner::TopLeft},
                                    CornerCase{QPointF(100, 20), Corner::TopRight},
                                    CornerCase{QPointF(20, 60), Corner::BottomLeft},
                                    CornerCase{QPointF(100, 60), Corner::BottomRight}}) {
        const QPointF initial(70, 90);
        const QSize original(120, 80);
        const auto anchor = PinZoomAnchor::fromPointer(initial, original, sample.pointer);
        if (anchor.corner != sample.expected || anchor.topLeftFor(original) != initial) return 168;
        const bool right = sample.expected == Corner::TopRight || sample.expected == Corner::BottomRight;
        const bool bottom = sample.expected == Corner::BottomLeft || sample.expected == Corner::BottomRight;
        for (const QSize size : {QSize(240, 160), QSize(75, 50), original}) {
            const QPointF fixed = anchor.topLeftFor(size) + QPointF(right ? size.width() : 0, bottom ? size.height() : 0);
            if (fixed != anchor.outputPoint) return 169;
        }
    }
    // 从真实按钮、键盘和右键菜单验证锁定边界以及穿透模式的退出通道。
    // Exercise actual close inputs and the escape hatch on a pass-through pin.
    {
        const auto fail=[](const char *why){std::fprintf(stderr,"Pin settings: %s\n",why);return 1;};
        QTemporaryDir dir; LinuxSettings prefs(dir.filePath("pin.ini")); prefs.setPinFlag("auto_ocr",false);
        QImage src(200,100,QImage::Format_RGB32);src.fill(Qt::red);
        QPointer<LinuxPinWindow> pin=new LinuxPinWindow(src,prefs); pin->showOnScreen(app.primaryScreen(),QRectF(100,100,200,100));
        QTest::keyClick(pin,Qt::Key_L); const auto base=pin->geometry();
        if(pin->beginDrag(pin->mapToGlobal(pin->rect().center()))) return fail("locked drag accepted");
        pin->triggerShortcut("pin_zoom_in"); if(pin->geometry()!=base) return fail("locked zoom accepted");
        pin->findChild<QPushButton *>("pinClose")->click(); QTest::keyClick(pin,Qt::Key_Escape);
        if(!pin->isVisible()) return fail("locked close accepted");
        prefs.setPinFlag("lock_block_move",false);
        QWheelEvent fine(QPointF(100,50),QPointF(pin->mapToGlobal(QPoint(100,50))),QPoint(),QPoint(0,120),Qt::NoButton,Qt::AltModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(pin,&fine); if(qAbs(pin->zoom()-1.01)>.0001) return fail("fine zoom step ignored");
        const QPoint edge=pin->mapToGlobal(QPoint(pin->width()-1,pin->height()-1));
        if(!pin->beginEdgeResize({qreal(pin->width()-1),qreal(pin->height()-1)},edge)) return fail("corner resize not started");
        pin->dragTo(edge+QPoint(50,25));pin->endDrag(); if(pin->width()<=202 || qAbs(pin->width()-2*pin->height())>1) return fail("resize changed aspect ratio");
        const auto menuAction=[&](const QString &prefix){
            QTimer::singleShot(0,pin,[prefix]{if(auto *m=qobject_cast<QMenu *>(QApplication::activePopupWidget())) {for(auto *a:m->actions()) if(a->text().startsWith(prefix)){m->setActiveAction(a); QTest::keyClick(m,Qt::Key_Return);return;} m->close();}});
            QContextMenuEvent evt(QContextMenuEvent::Mouse,{10,10},pin->mapToGlobal(QPoint(10,10))); QApplication::sendEvent(pin,&evt);
        };
        menuAction("鼠标穿透"); auto *host=pin->window(); QWidget *bar=nullptr;
        for(auto *top:QApplication::topLevelWidgets()) if(top->objectName()=="pinThroughBar" && top->isVisible()) {bar=top;break;}
        if(!bar || !bar->isVisible() || host->windowHandle()->mask().contains(pin->geometry().center()) || (!bar->isWindow() || bar->windowHandle()==host->windowHandle())) return fail("incorrect native pass-through region");
        bar->findChild<QPushButton *>()->click(); if(!host->windowHandle()->mask().contains(pin->geometry().center())) return fail("exit left image click-through");
        prefs.setPinFlag("rounded_corners",true);LinuxPinWindow::refreshOpenAppearance(prefs);
        if(pin->mask().contains(QPoint(0,0)) || pin->image()!=src) return fail("incorrect rounded pin/export");
        // 圆角边框必须沿曲线连续绘制，不能只是把矩形边框的四角裁掉。
        // Check rounded strokes in physical pixels at 1x and 2x; exports remain undecorated.
        for(int scale:{1,2}) {
            QImage outline(pin->size()*scale,QImage::Format_ARGB32_Premultiplied); outline.setDevicePixelRatio(scale);outline.fill(Qt::transparent);
            {QPainter painter(&outline);pin->paintBorder(painter);}
            bool curve=false;
            for(int y=1;y<7*scale;++y) for(int x=1;x<7*scale;++x) curve|=outline.pixelColor(x,y).alpha()>0;
            if(!curve || outline.pixelColor(0,0).alpha()!=0 || outline.pixelColor(outline.width()/2,outline.height()/2).alpha()!=0) return fail("rounded border clipped or filled the image");
        }
        menuAction("关闭贴图"); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); if(pin) return fail("explicit menu close blocked");
    }
    // 中央区域固定光标下的相对位置，避免缩放跳动。
    // The central quarter preserves the image point under the pointer without snapping the image center.
    for(const QPointF pointer:{QPointF(35,25),QPointF(60,40),QPointF(85,55)}) {
        const auto anchor=PinZoomAnchor::fromPointer(QPointF(70,90),QSize(120,80),pointer);
        if(anchor.corner!=Corner::Pointer) return 1;
        for(const QSize size:{QSize(240,160),QSize(75,50),QSize(120,80)}) {
            const auto fixed=anchor.topLeftFor(size)+QPointF(size.width()*pointer.x()/120,size.height()*pointer.y()/80);
            if((fixed-anchor.outputPoint).manhattanLength()>1e-8) return 1;
        }
    }
    // 验证真正的贴图子控件及可见像素位置，而非只验证工具栏的请求坐标。
    // Exercise actual child geometry and input masks, not merely requested toolbar coordinates.
    {
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("pin-geometry.ini"));
        QImage source(240,160,QImage::Format_RGB32); source.fill(Qt::red);
        for(const QPoint pointer:{QPoint(20,20),QPoint(220,20),QPoint(20,140),QPoint(220,140),QPoint(100,70)}) {
            QPointer<LinuxPinWindow> pin=new LinuxPinWindow(source,settings);
            pin->showOnScreen(app.primaryScreen(),QRectF(200,160,240,160));
            QWidget *surface=pin->window();
            if(surface==pin || surface->objectName()!="linuxPinSurface") return 1;
            const QRect surfaceGeometry=surface->geometry();
            const auto anchor=PinZoomAnchor::fromPointer(pin->pos(),pin->size(),pointer);
            const auto wheel=[&](int delta) {
                QWheelEvent event(QPointF(pointer),QPointF(pin->mapToGlobal(pointer)),QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
                QCoreApplication::sendEvent(pin,&event);
            };
            for(int delta:{120,120,-120,-120,120,-120}) {
                wheel(delta);
                if(pin->pos()!=anchor.topLeftFor(pin->size()).toPoint() || surface->geometry()!=surfaceGeometry
                    || surface->windowHandle()->mask()!=QRegion(pin->geometry()).intersected(surface->rect())) {
                    std::fprintf(stderr,"Pin zoom moved only metadata or changed its native surface\n"); return 1;
                }
            }
            if(pin->size()!=source.size() || pin->pos()!=QPoint(200,160)) return 1;
            // 在高 DPI 绘制下边框仍只有一列物理像素。
            // The cosmetic border remains one physical pixel even on a high-DPI paint device.
            QImage borderImage(pin->size()*2,QImage::Format_RGB32); borderImage.setDevicePixelRatio(2); borderImage.fill(Qt::red);
            {QPainter painter(&borderImage); pin->paintBorder(painter);}
            if(borderImage.pixelColor(0,20)!=QColor("#8cc8ed") || borderImage.pixelColor(1,20)==QColor("#8cc8ed")) return 1;
            // 拖动到不同的小数物理像素相位时，四边均须留在 clip 内，且仍只有一物理像素。
            // Fractional placement changes subpixel phase; all four one-pixel edges must remain inside the clip.
            for(double phase:{0.,.2,.4,.6,.8}) {
                QImage image(500,400,QImage::Format_RGB32); image.setDevicePixelRatio(1.25); image.fill(Qt::red);
                QPainter painter(&image); painter.translate(10+phase,10+phase); painter.setClipRect(pin->rect());
                const auto bounds=painter.deviceTransform().mapRect(QRectF(pin->rect()));
                pin->paintBorder(painter); painter.end();
                const int left=int(std::ceil(bounds.left()-1e-7)),top=int(std::ceil(bounds.top()-1e-7));
                const int right=int(std::floor(bounds.right()+1e-7))-1,bottom=int(std::floor(bounds.bottom()+1e-7))-1;
                for(const QPoint point:{QPoint(left,(top+bottom)/2),QPoint(right,(top+bottom)/2),QPoint((left+right)/2,top),QPoint((left+right)/2,bottom)})
                    if(image.pixelColor(point)!=QColor("#8cc8ed")) return 1;
                if(image.pixelColor(left+1,(top+bottom)/2)==QColor("#8cc8ed")) return 1;
            }
            pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        }
    }
    // 多次从下拉框切换时间格式时应只保留一个占位符，预览也只能出现一次时间。
    // Repeated format choices replace a token and collapse adjacent old duplicates.
    jietuba::WatermarkInputDialog timeDialog(QStringLiteral("王瑶$yyyy-MM-dd HH:mm:ss$$yyyy-MM-dd HH:mm:ss$"));
    timeDialog.edit->moveCursor(QTextCursor::End);
    timeDialog.time_menu->popup(QPoint(5, 5));
    QCoreApplication::processEvents();
    const auto choices = timeDialog.time_menu->actions();
    auto chosenFormat = std::find_if(choices.begin(), choices.end(), [](QAction *action) { return action->text().startsWith("US 12h"); });
    if (chosenFormat == choices.end()) return 165;
    (*chosenFormat)->trigger();
    timeDialog.time_menu->hide();
    if (timeDialog.template_text() != QStringLiteral("王瑶$MM/dd/yyyy hh:mm:ss AP$")
        || timeDialog.preview->text().contains('$')) return 166;
    QImage image(800, 600, QImage::Format_RGB32);
    for (int y=0; y<600; ++y) for (int x=0; x<800; ++x)
        image.setPixelColor(x,y,QColor(x*255/799, y*255/599, ((x+y)%2)*255));
    // 离屏平台验证普通窗口回退：准备阶段不可提前显示，首帧尺寸对应输出，Esc 能结束。
    // Check the non-layer fallback offscreen: hidden preparation, correct output size and Escape completion.
    {
        CapturePreview overlay(image);
        bool finished = false;
        overlay.setOverlayMode([&] { finished = true; overlay.hide(); });
        overlay.prepareOverlayOnScreen(app.primaryScreen());
        if (overlay.isVisible() || overlay.size() != app.primaryScreen()->geometry().size()) return 201;
        overlay.showPreparedOverlay();
        QTest::qWait(30);
        auto *overlayView = dynamic_cast<PreviewView *>(overlay.findChild<QGraphicsView *>());
        if (!overlay.isFullScreen() || !overlayView || !overlayView->fullScreenCapture) return 202;
        const QPoint mapped = overlayView->mapFromScene(QPointF(400, 300));
        if ((mapped - overlayView->viewport()->rect().center()).manhattanLength() > 3) return 203;
        // 保存窗口显示期间必须撤下覆盖层；取消恢复选区，成功保存结束截图。
        // The modal dialog must be interactive with the overlay unmapped; cancel restores, save finishes.
        overlayView->canvas->preset_selection(QRectF(100,100,240,160));
        auto *saveBar=overlay.findChild<jietuba::Toolbar *>();
        const auto selectionBefore=overlayView->canvas->selection_model->rect();
        bool dialogUnblocked=false;
        QTimer::singleShot(0,[&] {
            if(auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialogUnblocked=!overlay.isVisible() && !dialog->parentWidget();
                dialog->reject();
            }
        });
        saveBar->save_btn->click();
        if(!dialogUnblocked || !overlay.isVisible() || !saveBar->isVisible() || finished
            || overlayView->canvas->selection_model->rect()!=selectionBefore) return 253;
        QTemporaryDir savedCapture;
        const auto savedPath=savedCapture.filePath("capture.png");
        dialogUnblocked=false;
        QTimer::singleShot(0,[&] {
            if(auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialogUnblocked=!overlay.isVisible() && !dialog->parentWidget();
                dialog->selectFile(savedPath);
                QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);
            }
        });
        saveBar->save_btn->click();
        if(!dialogUnblocked || !finished || overlay.isVisible() || QImage(savedPath).isNull()) return 254;
        finished=false; overlay.showPreparedOverlay();
        QTest::keyClick(&overlay, Qt::Key_Escape);
        if (!finished || overlay.isVisible()) return 204;
    }
    // 对真实长截图控制器注入帧，验证不匹配不会停止采集或弹出“继续截图”窗口。
    // Feed the real controller without desktop capture: a mismatch must stay hidden and recover.
    {
        const QImage page(QStringLiteral(JIETUBA_BARCODE_FIXTURE_DIR "/stitch_page.png"));
        const QSize pixels(360, 300);
        LinuxScrollCaptureWindow capture(app.primaryScreen(), pixels, QRect(QPoint(), pixels),
                                         page.copy(0, 0, 360, 300), [] {});
        capture.setAttribute(Qt::WA_DeleteOnClose, false); // stack-owned test instance
        QSignalSpy processed(&capture, &LinuxScrollCaptureWindow::frameProcessed);
        QSignalSpy states(&capture, &LinuxScrollCaptureWindow::captureStateChanged);
        capture.startAutomaticCapture();
        QImage unrelated(pixels, QImage::Format_RGB32); unrelated.fill(Qt::red);
        QMetaObject::invokeMethod(&capture, "acceptFrame", Qt::DirectConnection, Q_ARG(QImage, unrelated));
        if (processed.isEmpty() && !processed.wait(3000)) return 210;
        if (capture.isVisible() || !capture.isCapturing() || processed.last()[1].toBool()) return 211;
        processed.clear();
        // 一次送入多个相邻帧，确认不会因后台匹配忙碌而丢帧；确认操作必须等它们拼完。
        // Burst frames must survive an in-flight match, and completion must drain their ordered queue.
        for (int top : {120, 260, 400}) {
            const QImage next = page.copy(0, top, 360, 300);
            QMetaObject::invokeMethod(&capture, "acceptFrame", Qt::DirectConnection, Q_ARG(QImage, next));
        }
        capture.stopAutomaticCapture();
        QElapsedTimer deadline; deadline.start();
        while (processed.size() < 3 && deadline.elapsed() < 3000) processed.wait(100);
        if (processed.size() != 3 || processed.last()[0].toInt() != 4 || !processed.last()[1].toBool()) return 212;
        QCoreApplication::processEvents();
        const auto png = QApplication::clipboard()->mimeData()->data(QStringLiteral("image/png"));
        const QImage copied = QImage::fromData(png).convertToFormat(QImage::Format_RGB32);
        if (capture.isVisible() || capture.isCapturing() || copied != page.copy(0, 0, 360, 700).convertToFormat(QImage::Format_RGB32)) return 214;
        if (states.last()[0].toBool()) return 215;

    }
    // 末页不裁边；停滚后导航条淡出时以当前完整选区更新，确认操作必须等待队列处理结束。
    // Preserve the full viewport, refresh a fading idle footer, and drain updates before clipboard export.
    {
        QImage page(120,600,QImage::Format_RGB32); QRandomGenerator random(74);
        for(int y=0;y<600;++y) for(int x=0;x<120;++x) page.setPixel(x,y,qRgb(random.bounded(256),random.bounded(256),random.bounded(256)));
        const auto frame = [&](int top) {
            QImage view=page.copy(0,top,120,200);
            QPainter painter(&view); painter.fillRect(QRect(0,176,120,24),QColor(30,44,60,140)); return view;
        };
        const QSize pixels(120,200);
        LinuxScrollCaptureWindow capture(app.primaryScreen(),pixels,QRect(QPoint(),pixels),frame(0),[] {});
        capture.setAttribute(Qt::WA_DeleteOnClose,false);
        QSignalSpy processed(&capture,&LinuxScrollCaptureWindow::frameProcessed);
        capture.startAutomaticCapture();
        for(int top:{48,96,144}) { const QImage next=frame(top); QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,next)); }
        QElapsedTimer wait; wait.start();
        while(processed.size()<3 && wait.elapsed()<3000) processed.wait(100);
        if(processed.size()!=3) return 228;
        QImage current=page.copy(0,0,120,344);
        { QPainter painter(&current); painter.drawImage(0,144,frame(144)); }
        if(capture.resultImage()!=current) return 229;
        // 同一位置的末帧发生变化，不能作为无重叠画面丢弃，也不能追加一页。
        // A changed stationary viewport is an update, not a lost-overlap failure or another appended page.
        const QImage idle=page.copy(0,144,120,200);
        QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,idle));
        capture.stopAutomaticCapture();
        wait.restart();
        while(processed.size()<4 && wait.elapsed()<3000) processed.wait(100);
        QCoreApplication::processEvents();
        const QImage copied=QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32);
        if(processed.size()!=4 || processed.last()[0].toInt()!=4 || !processed.last()[1].toBool() || copied!=page.copy(0,0,120,344)) {
            std::fprintf(stderr,"stop did not export full current viewport: %dx%d\n",copied.width(),copied.height()); return 230;
        }
    }
    // 快速连续送帧仍保留短重叠，最终 PNG 必须包含所有中间行。
    // A burst of short-overlap frames must survive the real queue and export every document row.
    {
        QImage page(120,1400,QImage::Format_RGB32); QRandomGenerator random(86);
        for(int y=0;y<page.height();++y) for(int x=0;x<120;++x) page.setPixel(x,y,qRgb(random.bounded(256),random.bounded(256),random.bounded(256)));
        const QSize pixels(120,200);
        LinuxScrollCaptureWindow capture(app.primaryScreen(),pixels,QRect(QPoint(),pixels),page.copy(0,0,120,200),[] {});
        capture.setAttribute(Qt::WA_DeleteOnClose,false);
        QSignalSpy processed(&capture,&LinuxScrollCaptureWindow::frameProcessed);
        capture.startAutomaticCapture();
        for(int top:{182,364,546,728,910}) {
            const QImage frame=page.copy(0,top,120,200);
            QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,frame));
        }
        capture.stopAutomaticCapture();
        QElapsedTimer wait; wait.start();
        while(processed.size()<5 && wait.elapsed()<3000) processed.wait(100);
        QCoreApplication::processEvents();
        const QImage copied=QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32);
        if(processed.size()!=5 || processed.last()[0].toInt()!=6 || copied!=page.copy(0,0,120,1110)) return 231;
    }
    // 上划回看并等待停滚，导航条、末页与帧计数都不能被复制到历史停留位置。
    // Backward navigation and idle refresh must leave captured pixels, tail and frame count unchanged.
    {
        QImage page(120,600,QImage::Format_RGB32); QRandomGenerator random(91);
        for(int y=0;y<page.height();++y) for(int x=0;x<120;++x) page.setPixel(x,y,qRgb(random.bounded(256),random.bounded(256),random.bounded(256)));
        const auto view=[&](int top,bool bar=true) {
            QImage frame=page.copy(0,top,120,200);
            if(bar) { QPainter painter(&frame); painter.fillRect(0,176,120,24,QColor(30,44,60,140)); }
            return frame;
        };
        const QSize pixels(120,200);
        LinuxScrollCaptureWindow capture(app.primaryScreen(),pixels,QRect(QPoint(),pixels),view(0),[] {});
        capture.setAttribute(Qt::WA_DeleteOnClose,false);
        QSignalSpy processed(&capture,&LinuxScrollCaptureWindow::frameProcessed);
        capture.startAutomaticCapture();
        const auto feed=[&](const QImage &frame) {
            const int before=processed.size();
            QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,frame));
            QElapsedTimer wait; wait.start();
            while(processed.size()==before && wait.elapsed()<3000) processed.wait(100);
            return processed.size()>before && processed.last()[1].toBool();
        };
        for(int top:{48,96,144}) if(!feed(view(top))) return 232;
        const QImage saved=capture.resultImage();
        for(int top:{96,48}) if(!feed(view(top)) || capture.resultImage()!=saved || processed.last()[0].toInt()!=4) return 233;
        if(!feed(view(48,false)) || capture.resultImage()!=saved) return 234;
        // 等待停滚预览计时器执行，确保它不会偷偷覆盖历史或切换末页快照。
        // Let the settle timer run as well; it must not reintroduce the historical overlay.
        QTest::qWait(230);
        if(capture.resultImage()!=saved) return 235;
        // 注入当前画面作为最后在途帧，再完成并检查实际剪贴板 PNG。
        // Complete the final in-flight request and verify the actual clipboard PNG.
        const QImage idle=view(48,false);
        QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,idle));
        capture.stopAutomaticCapture(); QCoreApplication::processEvents();
        const QImage copied=QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32);
        if(copied!=saved) return 236;
    }
    // 区域接口连续帧无需整屏尺寸：在分数缩放、非零选区原点下仍正确拼接和导出。
    // Native area replies stay cropped: exercise fractional DPI and a nonzero output-local origin.
    {
        QImage page(120,600,QImage::Format_RGB32); QRandomGenerator random(187);
        for (int y=0;y<page.height();++y) for (int x=0;x<page.width();++x)
            page.setPixel(x,y,qRgb(random.bounded(256),random.bounded(256),random.bounded(256)));
        auto *screen=app.primaryScreen();
        const QSize pixels(qRound(screen->geometry().width()*1.25),qRound(screen->geometry().height()*1.25));
        const QRect logical(80,100,96,160), native(100,125,120,200);
        LinuxScrollCaptureWindow capture(screen,pixels,native,page.copy(0,0,120,200),[] {},nullptr,logical);
        capture.setAttribute(Qt::WA_DeleteOnClose,false);
        QSignalSpy processed(&capture,&LinuxScrollCaptureWindow::frameProcessed);
        capture.startAutomaticCapture();
        for (int top:{48,96,144}) {
            QImage frame=page.copy(0,top,120,200); frame.setDevicePixelRatio(1.25);
            QMetaObject::invokeMethod(&capture,"acceptRegionFrame",Qt::DirectConnection,Q_ARG(QImage,frame));
        }
        capture.stopAutomaticCapture();
        QElapsedTimer wait; wait.start();
        while (processed.size()<3 && wait.elapsed()<3000) processed.wait(100);
        QCoreApplication::processEvents();
        const auto copied=QImage::fromData(app.clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32);
        if (processed.size()!=3 || !processed.last()[1].toBool() || copied!=page.copy(0,0,120,344)) {
            std::fprintf(stderr,"Native-area scrolling failed to stitch cropped frames\n"); return 1;
        }
        // 长图最终也走共用贴图窗口；展开工具栏后仍须能从画布拖动，工具栏跟随新位置。
        // Pin the stitched output through the shared window and drag from its expanded annotation canvas.
        QTemporaryDir directory; LinuxSettings settings(directory.filePath("scroll-pin.ini"));
        QPointer<LinuxPinWindow> pin=new LinuxPinWindow(copied,settings);
        pin->showOnScreen(screen); pin->toggleToolbar(); QCoreApplication::processEvents();
        auto *view=dynamic_cast<PreviewView *>(pin->findChild<QGraphicsView *>());
        auto *host=pin->window()->findChild<LinuxPinToolbar *>();
        if (!view || !host) return 1;
        const QPoint before=pin->pos();
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(15,15));
        QMouseEvent move(QEvent::MouseMove,QPointF(35,30),QPointF(view->viewport()->mapToGlobal(QPoint(35,30))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(),&move);
        QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(35,30));
        const QRectF pinRect(pin->pos()-screen->geometry().topLeft(),pin->size());
        if (pin->pos()!=before+QPoint(20,15) || host->toolbar()->geometry()!=LinuxPinToolbar::toolbarGeometry(host->size(),pinRect,host->toolbar()->size())) {
            std::fprintf(stderr,"Stitched pin drag/toolbar position failed\n"); return 1;
        }
        pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    // 横向使用真实控制器，包含向右扩展、向左回看、裁剪两端及 PNG 导出。
    // Exercise horizontal matching, history navigation, viewport cropping and PNG export end-to-end.
    {
        const QImage page = QImage(QStringLiteral(JIETUBA_BARCODE_FIXTURE_DIR "/stitch_page.png")).convertToFormat(QImage::Format_RGB32);
        const auto raw = [&](int left) { return page.copy(0,left,360,300).transformed(QTransform().rotate(-90)); };
        const QSize pixels(300,360);
        QImage latest = raw(0);
        LinuxScrollCaptureWindow *session = nullptr;
        LinuxScrollCaptureWindow capture(app.primaryScreen(), pixels, QRect(QPoint(),pixels), latest, [&] {
            // 真实后端总会回调；测试也返回当前画面，避免人为制造永不完成的在途请求。
            // Return the current frame just as a real backend would, including unchanged frames.
            QMetaObject::invokeMethod(session, "acceptFrame", Qt::QueuedConnection, Q_ARG(QImage, latest));
        });
        session = &capture; capture.setAttribute(Qt::WA_DeleteOnClose, false);
        QSignalSpy processed(&capture, &LinuxScrollCaptureWindow::frameProcessed);
        capture.startAutomaticCapture(); capture.toggleDirection();
        if (!capture.isHorizontal() || !capture.isCapturing()) return 221;
        for (int left : {100,220,100}) {
            latest = raw(left); const int count = processed.size();
            QMetaObject::invokeMethod(&capture,"acceptFrame",Qt::DirectConnection,Q_ARG(QImage,latest));
            QElapsedTimer wait; wait.start();
            while (processed.size() == count && wait.elapsed() < 3000) processed.wait(100);
            if (processed.size() == count || !processed.last()[1].toBool()) return 222;
        }
        capture.cropCurrentView("top");
        if (!QTest::qWaitFor([&] { return capture.isCapturing(); }, 3000)) return 223;
        if (capture.resultImage() != page.copy(0,100,360,420).transformed(QTransform().rotate(-90))) return 223;
        capture.cropCurrentView("bottom");
        if (!QTest::qWaitFor([&] { return capture.isCapturing(); }, 3000)) return 224;
        if (capture.resultImage() != raw(100)) return 224;
        capture.stopAutomaticCapture(); QCoreApplication::processEvents();
        const QImage copied = QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_RGB32);
        if (copied != raw(100)) return 225;
    }
    // 悬停预览需真正改变绘制透明度，离开恢复；不会触发额外结果窗口。
    // Hover fades the live preview and leaving restores it.
    {
        LinuxScrollPreview preview;
        preview.resize(180, 240); preview.setImage(image, 300); preview.show();
        QEnterEvent entered(QPointF(30, 30), QPointF(30, 30), QPointF(30, 30));
        QApplication::sendEvent(&preview, &entered); QTest::qWait(190);
        if (preview.previewOpacity() > 0.2) return 217;
        QEvent left(QEvent::Leave); QApplication::sendEvent(&preview, &left); QTest::qWait(190);
        if (preview.previewOpacity() < 0.95) return 218;
    }
    CapturePreview window(image); window.resize(800,600); window.show(); QTest::qWait(50);
    auto *view = dynamic_cast<PreviewView *>(window.findChild<QGraphicsView *>());
    auto *bar = window.findChild<jietuba::Toolbar *>();
    auto *panel = window.findChild<jietuba::MosaicSettingsPanel *>();
    if (!view || !bar || !panel || !bar->mosaic_btn->isEnabled() || !panel->kind_buttons.value("smart_erase")->isEnabled()) return 1;
    auto point = [&](int x, int y) { return view->mapFromScene(QPointF(x,y)); };
    auto drag = [&](QPoint from, QPoint to) { auto *v=view->viewport(); QTest::mousePress(v,Qt::LeftButton,Qt::NoModifier,from); QTest::mouseMove(v,to,10); QTest::mouseRelease(v,Qt::LeftButton,Qt::NoModifier,to); QTest::qWait(15); };
    drag(point(20,20),point(780,580));
    if (!bar->isVisible()) return 2;
    // 工具栏入口传递原图像素选区和首帧，不把标注或工具栏截入长图。
    // The toolbar supplies the original native-pixel crop and screen to the long-capture controller.
    bool longStarted = false;
    window.setLongCaptureHandler([&](QScreen *screen, const QSize &pixels, const QRect &region, const QImage &first, const QRect &logicalRegion) {
        longStarted = screen && pixels == image.size() && first == image.copy(region) && logicalRegion.isEmpty();
    });
    if (!bar->long_screenshot_btn->isEnabled()) return 205;
    bar->long_screenshot_btn->click();
    if (!longStarted) return 206;
    // GIF 入口同样传原生选区，框选后可直接进入实时录制，不依赖画布标注导出。
    // The GIF entry passes the native crop to a live recorder rather than exporting annotations.
    bool gifStarted = false;
    window.setGifCaptureHandler([&](QScreen *screen,const QSize &pixels,const QRect &region,const QRect &logicalRegion) {
        gifStarted = screen && pixels == image.size() && region == view->canvas->selection_model->rect().normalized().toAlignedRect().intersected(image.rect()) && logicalRegion.isEmpty();
    });
    if (!bar->gif_btn->isEnabled()) return 247;
    bar->gif_btn->click(); if (!gifStarted) return 248;
    QTest::mouseClick(bar->mosaic_btn,Qt::LeftButton); QTest::qWait(15);
    QTest::mouseClick(panel->rect_btn,Qt::LeftButton);
    drag(point(200,150),point(400,350));
    jietuba::MosaicItem *item=nullptr;
    for (auto *candidate : view->canvas->items()) if ((item=dynamic_cast<jietuba::MosaicItem *>(candidate))) break;
    if (!item || !item->fill_mode() || !view->editor->active_layer()) return 3;
    auto exportImage = [&] { return jietuba::ExportService(view->canvas).export_image(QRectF(0,0,800,600)); };
    auto exported=exportImage();
    if (exported.pixelColor(250,200) == image.pixelColor(250,200) || exported.pixelColor(100,100) != image.pixelColor(100,100)) return 4;
    QTest::mouseClick(panel->kind_buttons.value("blur"),Qt::LeftButton);
    if (item->kind()!=jietuba::MosaicKind::Blur) return 5;
    if (exportImage().copy(200,150,200,200) == exported.copy(200,150,200,200)) return 21;
    view->canvas->undo_stack->undo(); if(item->kind()!=jietuba::MosaicKind::Pixelate) return 6;
    view->canvas->undo_stack->redo(); if(item->kind()!=jietuba::MosaicKind::Blur) return 7;
    QTest::mouseClick(panel->block_size_buttons.value(32),Qt::LeftButton);
    if (item->block_size()!=32) return 8;
    view->canvas->undo_stack->undo(); if(item->block_size()!=10) return 9;
    view->canvas->undo_stack->redo(); if(item->block_size()!=32) return 10;
    drag(point(260,250),point(310,270));
    if (qAbs(item->pos().x()-50)>2 || qAbs(item->pos().y()-20)>2) return 11;
    view->canvas->undo_stack->undo(); if (qAbs(item->pos().x())>1) return 12;
    view->canvas->undo_stack->redo(); if (qAbs(item->pos().x()-50)>2) return 13;
    if (exportImage().pixelColor(300,220)==image.pixelColor(300,220)) return 14;
    auto before = item->rect();
    // 用真实鼠标事件拖动控制点，覆盖视口坐标转换和自动生成的撤销命令。
    // Drag the real resize handle to exercise view mapping and the generated undo command.
    const auto handle = std::find_if(view->editor->handles.begin(), view->editor->handles.end(), [](const auto &h) { return h.handle_type == jietuba::HandleType::CORNER_BR; });
    if (handle == view->editor->handles.end()) return 15;
    drag(view->mapFromScene(handle->position), view->mapFromScene(handle->position + QPointF(50, 30)));
    if (item->rect().width() < before.width()+48) return 15;
    view->canvas->undo_stack->undo(); if (item->rect()!=before) return 16;
    view->canvas->undo_stack->redo(); if (item->rect().width() < before.width()+49) return 17;
    QTest::mouseClick(panel->freehand_btn,Qt::LeftButton);
    drag(point(550,200),point(650,200));
    int count=0; for(auto *candidate:view->canvas->items()) if(dynamic_cast<jietuba::MosaicItem *>(candidate)) ++count;
    if(count!=2 || exportImage().pixelColor(600,200)==image.pixelColor(600,200)) return 18;
    view->canvas->undo_stack->undo();
    if(exportImage().pixelColor(600,200)!=image.pixelColor(600,200)) return 19;
    view->canvas->undo_stack->redo();
    if(exportImage().pixelColor(600,200)==image.pixelColor(600,200)) return 20;
    // 选中已有手绘图形，调整笔宽和位置也必须进入撤销栈。
    // Editing an existing freehand stroke also records width and movement changes.
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point(600,200));
    auto *stroke = dynamic_cast<jietuba::MosaicItem *>(view->editor->active_layer());
    if (!stroke || stroke->fill_mode()) return 24;
    const auto originalWidth = stroke->brush_width();
    panel->size_spin->setValue(50);
    if (stroke->brush_width() != 50) return 25;
    view->canvas->undo_stack->undo(); if (stroke->brush_width() != originalWidth) return 26;
    view->canvas->undo_stack->redo(); if (stroke->brush_width() != 50) return 27;
    drag(point(600,200),point(600,260));
    if (qAbs(stroke->pos().y()-60)>2) return 28;
    view->canvas->undo_stack->undo(); if (qAbs(stroke->pos().y())>2) return 29;
    view->canvas->undo_stack->redo(); if (qAbs(stroke->pos().y()-60)>2) return 30;
    // 真正走确认按钮并读取 PNG 剪贴板数据，不只测试中间的场景渲染。
    // Exercise the actual confirm action and PNG clipboard payload, not just scene rendering.
    const auto selection = view->canvas->selection_model->rect();
    const auto expected = jietuba::ExportService(view->canvas).export_image(selection);
    QTest::mouseClick(bar->confirm_btn, Qt::LeftButton);
    const auto *mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat("image/png") || QImage::fromData(mime->data("image/png")).convertToFormat(QImage::Format_ARGB32) != expected.convertToFormat(QImage::Format_ARGB32)) return 22;
    // 重新绘制并通过真实保存对话框验证磁盘图片也包含处理后的像素。
    // Draw again and verify the actual saved file contains obfuscated pixels.
    drag(point(20,20),point(780,580));
    QTest::mouseClick(bar->mosaic_btn,Qt::LeftButton);
    QTest::mouseClick(panel->rect_btn,Qt::LeftButton);
    drag(point(200,150),point(400,350));
    const auto savedRect = view->canvas->selection_model->rect();
    QTemporaryDir dir;
    const auto path = dir.filePath("mosaic.png");
    QTimer::singleShot(0, [path] { if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); } });
    QTest::mouseClick(bar->save_btn, Qt::LeftButton);
    const QImage saved(path);
    const QPoint sample(qRound(250-savedRect.left()),qRound(200-savedRect.top()));
    if (saved.isNull() || !saved.rect().contains(sample) || saved.pixelColor(sample) == image.pixelColor(250,200)) return 23;
    // 单独用平色图片验证序号、批量橡皮擦和真实智能擦除算法。
    // Exercise numbering, batch erasure and the real smart-erase solver on a flat image.
    QImage source(800, 600, QImage::Format_RGB32); source.fill(Qt::white);
    { QPainter painter(&source); painter.fillRect(QRect(340,180,40,40), Qt::black); }
    CapturePreview editWindow(source); editWindow.resize(800,600); editWindow.show(); QTest::qWait(30);
    auto *editView = dynamic_cast<PreviewView *>(editWindow.findChild<QGraphicsView *>());
    auto *editBar = editWindow.findChild<jietuba::Toolbar *>();
    auto *numberPanel = editWindow.findChild<jietuba::NumberSettingsPanel *>();
    auto *editMosaicPanel = editWindow.findChild<jietuba::MosaicSettingsPanel *>();
    if (!editView || !editBar || !numberPanel || !editMosaicPanel || !editBar->number_btn->isEnabled() || !editBar->eraser_btn->isEnabled()) return 31;
    auto ep = [&](int x,int y) { return editView->mapFromScene(QPointF(x,y)); };
    auto click = [&](int x,int y) { QTest::mouseClick(editView->viewport(), Qt::LeftButton, Qt::NoModifier, ep(x,y)); QTest::qWait(10); };
    auto edrag = [&](QPoint a,QPoint b) { QTest::mousePress(editView->viewport(),Qt::LeftButton,Qt::NoModifier,a); QTest::mouseMove(editView->viewport(),b,10); QTest::mouseRelease(editView->viewport(),Qt::LeftButton,Qt::NoModifier,b); QTest::qWait(10); };
    edrag(ep(20,20),ep(780,580));
    QTest::mouseClick(editBar->rect_btn,Qt::LeftButton);
    edrag(ep(450,100),ep(530,160));
    auto rectangles = [&] { int count=0; for (auto *candidate : editView->canvas->items()) if (dynamic_cast<jietuba::RectItem *>(candidate)) ++count; return count; };
    if (rectangles()!=1) return 57;
    QTest::mouseClick(editBar->number_btn,Qt::LeftButton);
    click(150,100); click(260,100);
    auto numbers = [&] { QList<jietuba::NumberItem *> result; for (auto *candidate : editView->canvas->items()) if (auto *number=dynamic_cast<jietuba::NumberItem *>(candidate)) result.append(number); return result; };
    if (numbers().size()!=2 || jietuba::NumberTool::get_next_number(editView->canvas)!=3 || numberPanel->next_number()!=3) return 32;
    auto *second=dynamic_cast<jietuba::NumberItem *>(editView->editor->active_layer());
    if (!second || second->number!=2) return 33;
    const auto increment=std::find_if(editView->editor->handles.begin(), editView->editor->handles.end(), [](const auto &h) { return h.handle_type==jietuba::HandleType::NUMBER_INCREMENT; });
    if (increment==editView->editor->handles.end()) return 34;
    const auto incrementPos=editView->mapFromScene(increment->position)+increment->offset.toPoint();
    QTest::mouseClick(editView->viewport(),Qt::LeftButton,Qt::NoModifier,incrementPos);
    if (second->number!=3 || numberPanel->next_number()!=4) return 35;
    // 连续操作可能被合并成一条撤销命令；每一次都必须立即刷新面板。
    // Consecutive edits can merge into one undo command; every step must refresh the panel.
    QTest::mouseClick(editView->viewport(),Qt::LeftButton,Qt::NoModifier,incrementPos);
    if (second->number!=4 || numberPanel->next_number()!=5) return 68;
    const auto decrement=std::find_if(editView->editor->handles.begin(), editView->editor->handles.end(), [](const auto &h) { return h.handle_type==jietuba::HandleType::NUMBER_DECREMENT; });
    if (decrement==editView->editor->handles.end()) return 69;
    QTest::mouseClick(editView->viewport(),Qt::LeftButton,Qt::NoModifier,editView->mapFromScene(decrement->position)+decrement->offset.toPoint());
    if (second->number!=3 || numberPanel->next_number()!=4) return 70;
    editView->canvas->undo_stack->undo(); if (second->number!=2 || numberPanel->next_number()!=3) return 36;
    editView->canvas->undo_stack->redo(); if (second->number!=3 || numberPanel->next_number()!=4) return 37;
    click(370,100);
    auto *third=dynamic_cast<jietuba::NumberItem *>(editView->editor->active_layer());
    if (!third || third->number!=4 || numberPanel->next_number()!=5) return 71;
    editView->canvas->undo_stack->undo();
    if (numbers().size()!=2 || numberPanel->next_number()!=4) return 72;
    click(260,100);
    if (editView->editor->active_layer()!=second) return 73;
    QMetaObject::invokeMethod(numberPanel, "style_changed", Q_ARG(QString, QString("hollow_bg")));
    if (second->style!="hollow_bg") return 38;
    editView->canvas->undo_stack->undo(); if (second->style!="solid") return 39;
    editView->canvas->undo_stack->redo(); if (second->style!="hollow_bg") return 40;
    const auto initialSize=second->get_stroke_width().value_or(0);
    numberPanel->size_spin->setValue(24);
    if (second->get_stroke_width()!=24) return 62;
    editView->canvas->undo_stack->undo(); if (second->get_stroke_width()!=initialSize) return 63;
    editView->canvas->undo_stack->redo(); if (second->get_stroke_width()!=24) return 64;
    QMetaObject::invokeMethod(numberPanel, "color_changed", Q_ARG(QColor, QColor(Qt::blue)));
    if (second->color!=QColor(Qt::blue)) return 65;
    editView->canvas->undo_stack->undo(); if (second->color!=QColor(Qt::red)) return 66;
    editView->canvas->undo_stack->redo(); if (second->color!=QColor(Qt::blue)) return 67;
    auto clickNumberHandle = [&](jietuba::HandleType type) {
        const auto h = std::find_if(editView->editor->handles.begin(), editView->editor->handles.end(), [type](const auto &v) { return v.handle_type==type; });
        if (h==editView->editor->handles.end()) return false;
        QTest::mouseClick(editView->viewport(),Qt::LeftButton,Qt::NoModifier,editView->mapFromScene(h->position)+h->offset.toPoint());
        return true;
    };
    if (!clickNumberHandle(jietuba::HandleType::NUMBER_ARROW) || !second->arrow_tip()) return 47;
    editView->canvas->undo_stack->undo(); if (second->arrow_tip()) return 48;
    editView->canvas->undo_stack->redo(); if (!second->arrow_tip()) return 49;
    if (!clickNumberHandle(jietuba::HandleType::NUMBER_LABEL) || !second->label() || !second->label()->is_editing()) return 50;
    QTest::keyClicks(editView->viewport(), "hello");
    second->label()->clearFocus(); QTest::qWait(10);
    if (second->label_text()!="hello") return 51;
    editView->canvas->undo_stack->undo(); if (!second->label_text().isEmpty()) return 52;
    editView->canvas->undo_stack->redo(); if (second->label_text()!="hello") return 53;
    QTest::mouseClick(editBar->eraser_btn,Qt::LeftButton);
    edrag(ep(150,100),ep(260,100));
    if (!numbers().isEmpty()) return 41;
    editView->canvas->undo_stack->undo(); if (numbers().size()!=2) return 42;
    editView->canvas->undo_stack->redo(); if (!numbers().isEmpty()) return 43;
    click(490,100);
    if (rectangles()!=0) return 58;
    editView->canvas->undo_stack->undo(); if (rectangles()!=1) return 59;
    editView->canvas->undo_stack->redo(); if (rectangles()!=0) return 60;
    QTest::mouseClick(editBar->mosaic_btn,Qt::LeftButton);
    QTest::mouseClick(editMosaicPanel->rect_btn,Qt::LeftButton);
    QTest::mouseClick(editMosaicPanel->kind_buttons.value("smart_erase"),Qt::LeftButton);
    edrag(ep(325,165),ep(395,235));
    jietuba::MosaicItem *smart = nullptr;
    for (auto *candidate : editView->canvas->items()) if (auto *m=dynamic_cast<jietuba::MosaicItem *>(candidate); m && m->kind()==jietuba::MosaicKind::SmartErase) smart=m;
    if (!smart || !smart->ensure_smart_erase_result()) return 44;
    edrag(ep(360,200),ep(370,205));
    if (qAbs(smart->pos().x()-10)>2 || !smart->ensure_smart_erase_result()) return 54;
    editView->canvas->undo_stack->undo();
    if (qAbs(smart->pos().x())>2 || !smart->ensure_smart_erase_result()) return 55;
    const auto result=jietuba::ExportService(editView->canvas).export_image(QRectF(0,0,800,600));
    if (result.pixelColor(360,200)!=QColor(Qt::white)) return 45;
    const auto selection2=editView->canvas->selection_model->rect();
    QTest::mouseClick(editBar->confirm_btn,Qt::LeftButton);
    const QImage smartCopy=QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png"));
    if (smartCopy.isNull() || smartCopy.pixelColor(qRound(360-selection2.left()),qRound(200-selection2.top()))!=QColor(Qt::white)) return 46;
    // 全图遮罩没有周围像素可采样：必须报错并保留剪贴板和当前选区。
    // A full-image mask has no donors: refuse export and keep clipboard and selection intact.
    edrag(ep(5,5),ep(795,595));
    const auto fullRect=editView->canvas->sceneRect();
    QPainterPath fullPath; fullPath.addRect(fullRect);
    auto *failed=new jietuba::MosaicItem(fullPath, 30, 10, editView->canvas->background->reduced_image(10), fullRect, true, jietuba::MosaicKind::SmartErase);
    editView->canvas->addItem(failed);
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    bool warned=false;
    QTimer::singleShot(0, [&] { if (auto *message=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { warned=true; message->accept(); } });
    QTest::mouseClick(editBar->confirm_btn, Qt::LeftButton);
    if (!warned || QApplication::clipboard()->text()!=QStringLiteral("sentinel") || editView->canvas->selection_model->is_empty()) return 56;
    editView->canvas->removeItem(failed); delete failed;
    QTest::mouseClick(editBar->mosaic_btn,Qt::LeftButton);
    QTest::mouseClick(editMosaicPanel->rect_btn,Qt::LeftButton);
    QTest::mouseClick(editMosaicPanel->kind_buttons.value("smart_erase"),Qt::LeftButton);
    edrag(ep(325,165),ep(395,235));
    const auto selection3=editView->canvas->selection_model->rect();
    QTemporaryDir smartDir;
    const auto smartPath=smartDir.filePath("smart.png");
    QTimer::singleShot(0, [smartPath] { if (auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { dialog->selectFile(smartPath); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection); } });
    QTest::mouseClick(editBar->save_btn,Qt::LeftButton);
    const QImage smartSaved(smartPath);
    if (smartSaved.isNull() || smartSaved.pixelColor(qRound(360-selection3.left()),qRound(200-selection3.top()))!=QColor(Qt::white)) return 61;
    // 模拟用户在真实画布上操作后等待后台计算，不主动触发同步导出。
    // Verify that the on-screen freehand result arrives asynchronously without forcing export.
    CapturePreview liveWindow(source); liveWindow.resize(800,600); liveWindow.show(); QTest::qWait(30);
    auto *liveView=dynamic_cast<PreviewView *>(liveWindow.findChild<QGraphicsView *>());
    auto *liveBar=liveWindow.findChild<jietuba::Toolbar *>();
    auto *livePanel=liveWindow.findChild<jietuba::MosaicSettingsPanel *>();
    if (!liveView || !liveBar || !livePanel) return 74;
    auto lp = [&](int x,int y) { return liveView->mapFromScene(QPointF(x,y)); };
    auto ldrag = [&](QPoint a,QPoint b) { QTest::mousePress(liveView->viewport(),Qt::LeftButton,Qt::NoModifier,a); QTest::mouseMove(liveView->viewport(),b,10); QTest::mouseRelease(liveView->viewport(),Qt::LeftButton,Qt::NoModifier,b); QTest::qWait(10); };
    ldrag(lp(20,20),lp(780,580));
    QTest::mouseClick(liveBar->mosaic_btn,Qt::LeftButton);
    QTest::mouseClick(livePanel->kind_buttons.value("smart_erase"),Qt::LeftButton);
    if (!livePanel->kind_buttons.value("smart_erase")->isChecked()) return 84;
    livePanel->size_spin->setValue(60);
    ldrag(lp(325,200),lp(395,200));
    jietuba::MosaicItem *liveSmart=nullptr;
    for (auto *candidate:liveView->canvas->items()) if (auto *m=dynamic_cast<jietuba::MosaicItem *>(candidate); m && m->kind()==jietuba::MosaicKind::SmartErase) liveSmart=m;
    if (!liveSmart || liveSmart->fill_mode()) return 75;
    QElapsedTimer wait; wait.start();
    while (liveSmart->smart_erase_fill() && !liveSmart->smart_erase_fill()->ready() && wait.elapsed()<5000) QTest::qWait(10);
    if (!liveSmart->smart_erase_fill() || !liveSmart->smart_erase_fill()->ready() || !liveSmart->smart_erase_fill()->patch()) return 76;
    const auto liveImage=liveView->viewport()->grab().toImage();
    const auto livePoint=lp(360,200);
    if (!liveImage.rect().contains(livePoint) || liveImage.pixelColor(livePoint)!=QColor(Qt::white)) return 77;
    // 单击手绘智能擦除应产生圆形笔刷点，不必拖动鼠标。
    // A freehand click should erase a circular stamp even without mouse movement.
    QImage dotSource(800,600,QImage::Format_RGB32); dotSource.fill(Qt::white);
    { QPainter painter(&dotSource); painter.fillRect(QRect(356,196,8,8),Qt::black); }
    CapturePreview dotWindow(dotSource); dotWindow.resize(800,600); dotWindow.show(); QTest::qWait(30);
    auto *dotView=dynamic_cast<PreviewView *>(dotWindow.findChild<QGraphicsView *>());
    auto *dotBar=dotWindow.findChild<jietuba::Toolbar *>();
    auto *dotPanel=dotWindow.findChild<jietuba::MosaicSettingsPanel *>();
    if (!dotView || !dotBar || !dotPanel) return 78;
    auto dp = [&](int x,int y) { return dotView->mapFromScene(QPointF(x,y)); };
    QTest::mousePress(dotView->viewport(),Qt::LeftButton,Qt::NoModifier,dp(20,20));
    QTest::mouseMove(dotView->viewport(),dp(780,580),10);
    QTest::mouseRelease(dotView->viewport(),Qt::LeftButton,Qt::NoModifier,dp(780,580)); QTest::qWait(10);
    QTest::mouseClick(dotBar->mosaic_btn,Qt::LeftButton);
    QTest::mouseClick(dotPanel->kind_buttons.value("smart_erase"),Qt::LeftButton);
    dotPanel->size_spin->setValue(40);
    QTest::mouseClick(dotView->viewport(),Qt::LeftButton,Qt::NoModifier,dp(360,200));
    jietuba::MosaicItem *dotSmart=nullptr;
    for (auto *candidate:dotView->canvas->items()) if (auto *m=dynamic_cast<jietuba::MosaicItem *>(candidate); m && m->kind()==jietuba::MosaicKind::SmartErase) dotSmart=m;
    if (!dotSmart) return 79;
    wait.restart();
    while (dotSmart->smart_erase_fill() && !dotSmart->smart_erase_fill()->ready() && wait.elapsed()<5000) QTest::qWait(10);
    if (!dotSmart->smart_erase_fill() || !dotSmart->smart_erase_fill()->ready() || !dotSmart->smart_erase_fill()->patch()) return 80;
    if (dotView->viewport()->grab().toImage().pixelColor(dp(360,200))!=QColor(Qt::white)) return 81;
    // 用设置页修改临时 INI，再从新设置实例构建画布，检查实际路径而非仅断言控件值。
    // Persist options via the real settings page, then exercise a new canvas and inspect the stroke geometry.
    {
        const auto fail = [](const char *why) { std::fprintf(stderr,"Capture quick actions: %s\n",why); return 1; };
        QTemporaryDir dir; LinuxSettings prefs(dir.filePath("actions.ini"));
        LinuxSettingsWindow page(prefs);
        auto *angles = page.findChild<QComboBox *>("shiftLineAngle");
        auto *cross = page.findChild<jietuba::SwitchSettingCard *>("crossToolSelection");
        auto *top = page.findChild<jietuba::SwitchSettingCard *>("textAlwaysOnTop");
        if (!angles || !cross || !top || angles->currentData()!=45 || !cross->isChecked() || !top->isChecked()) return fail("incorrect defaults");
        angles->setCurrentIndex(angles->findData(90)); cross->setChecked(false); top->setChecked(false);
        LinuxSettings stored(prefs.fileName());
        if (stored.crossToolSelection() || stored.textAlwaysOnTop() || stored.shiftLineAngle()!=90) return fail("settings did not persist");
        CapturePreview window(source,&stored); window.resize(800,600); window.show(); QTest::qWait(20);
        auto *v = dynamic_cast<PreviewView *>(window.findChild<QGraphicsView *>());
        auto *bar = window.findChild<jietuba::Toolbar *>();
        v->canvas->preset_selection(QRectF(20,20,740,550));
        for (auto *button : {bar->pen_btn, bar->highlighter_btn}) {
            button->click();
            const int y = button==bar->pen_btn ? 100 : 200;
            QTest::mousePress(v->viewport(),Qt::LeftButton,Qt::ShiftModifier,v->mapFromScene(100,y));
            QTest::mouseRelease(v->viewport(),Qt::LeftButton,Qt::ShiftModifier,v->mapFromScene(240,y+36));
            bool horizontal=false;
            for (auto *item:v->canvas->items()) if (auto *stroke=dynamic_cast<jietuba::StrokeItem *>(item)) {
                auto path=stroke->path(); const auto first=path.elementAt(0);
                if (qAbs(first.y-y)<2 && path.currentPosition().x()>200 && qAbs(path.currentPosition().y()-first.y)<1) horizontal=true;
            }
            if (!horizontal) return fail("Shift ignored horizontal snap");
        }
        bar->rect_btn->click();
        auto *strokePos=v->viewport(); const auto hit=v->mapFromScene(155,100);
        QTest::mouseClick(strokePos,Qt::LeftButton,Qt::ControlModifier,hit);
        if (v->tool!="rect") return fail("disabled Ctrl switched tools");
        stored.setCrossToolSelection(true);
        QTest::mouseClick(strokePos,Qt::LeftButton,Qt::ControlModifier,hit);
        if (v->tool!="pen" || !dynamic_cast<jietuba::StrokeItem *>(v->editor->active_layer())) return fail("enabled Ctrl did not select pen stroke");
        bar->text_btn->click(); QTest::mouseClick(v->viewport(),Qt::LeftButton,Qt::NoModifier,v->mapFromScene(360,210));
        auto *text=dynamic_cast<jietuba::TextItem *>(v->canvas->focusItem());
        if (!text || text->zValue()!=jietuba::TextItem::NORMAL_ANNOTATION_Z_VALUE) return fail("new text ignored stacking setting");
    }
    // 荧光笔使用原版相乘混合：背景着色，黑色文字仍然可读。
    // Highlighter uses the original multiply blending, preserving black text underneath.
    QImage highlightSource(800,600,QImage::Format_RGB32); highlightSource.fill(QColor(180,200,240));
    { QPainter painter(&highlightSource); painter.fillRect(QRect(200,195,12,10),Qt::black); }
    CapturePreview highlightWindow(highlightSource); highlightWindow.resize(800,600); highlightWindow.show(); QTest::qWait(30);
    auto *highlightView=dynamic_cast<PreviewView *>(highlightWindow.findChild<QGraphicsView *>());
    auto *highlightBar=highlightWindow.findChild<jietuba::Toolbar *>();
    auto *highlightPanel=highlightWindow.findChild<jietuba::PaintSettingsPanel *>("linuxHighlighterPanel");
    if (!highlightView || !highlightBar || !highlightPanel || !highlightBar->highlighter_btn->isEnabled()) return 85;
    auto hp = [&](int x,int y) { return highlightView->mapFromScene(QPointF(x,y)); };
    auto hdrag = [&](QPoint a,QPoint b) { QTest::mousePress(highlightView->viewport(),Qt::LeftButton,Qt::NoModifier,a); QTest::mouseMove(highlightView->viewport(),b,10); QTest::mouseRelease(highlightView->viewport(),Qt::LeftButton,Qt::NoModifier,b); QTest::qWait(10); };
    hdrag(hp(20,20),hp(780,580));
    QTest::mouseClick(highlightBar->highlighter_btn,Qt::LeftButton); QTest::qWait(10);
    if (!highlightPanel->mode_widget->isVisible() || !highlightPanel->freehand_btn->isChecked() || !highlightPanel->line_style_combo->isHidden() || highlightPanel->current_size!=15) return 86;
    // 更多按钮与弹窗之间有间隔：鼠标从按钮移入菜单后，计时器不得关闭菜单。
    // Crossing from More to the popup must cancel the close timer and keep the menu visible.
    QEnterEvent enter(QPointF(1,1),QPointF(1,1),QPointF(1,1));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(highlightBar->more_btn,&enter);
    auto *more=highlightBar->more_popup();
    if (!more || !more->isVisible() || highlightPanel->isVisible()) return 113;
    QApplication::sendEvent(highlightBar->more_btn,&leave);
    QTest::qWait(350);
    if (!more->isVisible()) return 114;
    QApplication::sendEvent(more,&enter);
    QTest::qWait(750);
    if (!more->isVisible()) return 115;
    // 悬停打开后第一次点击不应关闭；再次点击可以显式关闭并恢复工具面板。
    // First click after hover keeps it open; a second closes it and restores the panel.
    highlightBar->more_btn->click(); if (!more->isVisible()) return 116;
    highlightBar->more_btn->click(); if (more->isVisible() || !highlightPanel->isVisible()) return 117;
    highlightBar->more_btn->click(); if (!more->isVisible()) return 118;
    QApplication::sendEvent(more,&leave);
    QTest::qWait(750);
    if (more->isVisible() || !highlightPanel->isVisible()) return 119;
    // 窄窗口中的可用按钮折叠进菜单后仍能点击，执行后菜单正常关闭。
    // An enabled tool folded into More must remain clickable and close the popup afterwards.
    highlightBar->set_preview_available_width(300);
    highlightBar->more_btn->click();
    if (!more->isVisible() || highlightBar->highlighter_btn->parentWidget()!=more || !highlightBar->highlighter_btn->isEnabled()) return 120;
    highlightBar->highlighter_btn->click();
    if (more->isVisible()) return 121;
    highlightBar->set_preview_available_width(highlightWindow.width());
    QTest::mouseClick(highlightBar->highlighter_btn,Qt::LeftButton);
    if (!highlightPanel->isVisible()) return 122;
    QTest::mouseClick(highlightBar->pen_btn,Qt::LeftButton);
    if (highlightView->canvas->tool_controller->ctx.color!=QColor(Qt::red) || highlightView->canvas->tool_controller->ctx.stroke_width!=3) return 110;
    QTest::mouseClick(highlightBar->highlighter_btn,Qt::LeftButton);
    if (highlightView->canvas->tool_controller->ctx.color!=QColor(Qt::yellow) || highlightView->canvas->tool_controller->ctx.stroke_width!=15) return 111;
    hdrag(hp(150,200),hp(300,200));
    jietuba::StrokeItem *highlightStroke=nullptr;
    for (auto *candidate:highlightView->canvas->items()) if (auto *stroke=dynamic_cast<jietuba::StrokeItem *>(candidate); stroke && stroke->is_highlighter) highlightStroke=stroke;
    if (!highlightStroke || qAbs(highlightStroke->pen().widthF()-45)>0.01) return 87;
    auto highlightExport=[&] { return jietuba::ExportService(highlightView->canvas).export_image(QRectF(0,0,800,600)); };
    auto firstHighlight=highlightExport();
    if (firstHighlight.pixelColor(170,200)!=QColor(180,200,0) || firstHighlight.pixelColor(205,200)!=QColor(Qt::black)) return 88;
    const auto visibleHighlight=highlightView->viewport()->grab().toImage();
    if (visibleHighlight.pixelColor(hp(170,200))!=QColor(180,200,0) || visibleHighlight.pixelColor(hp(205,200))!=QColor(Qt::black)) return 112;
    // Ctrl 选择已有笔迹后，宽度和颜色更改都必须正确撤销。
    // Ctrl selects an existing stroke; width and color edits must remain undoable.
    QTest::mouseClick(highlightView->viewport(),Qt::LeftButton,Qt::ControlModifier,hp(170,200));
    if (highlightView->editor->active_layer()!=highlightStroke) return 89;
    highlightPanel->size_spin->setValue(20);
    if (qAbs(highlightStroke->pen().widthF()-60)>0.01) return 90;
    highlightView->canvas->undo_stack->undo(); if (qAbs(highlightStroke->pen().widthF()-45)>0.01) return 91;
    highlightView->canvas->undo_stack->redo(); if (qAbs(highlightStroke->pen().widthF()-60)>0.01) return 92;
    QMetaObject::invokeMethod(highlightPanel,"color_changed",Q_ARG(QColor,QColor(Qt::blue)));
    if (highlightStroke->pen().color()!=QColor(Qt::blue)) return 93;
    highlightView->canvas->undo_stack->undo(); if (highlightStroke->pen().color()!=QColor(Qt::yellow)) return 94;
    highlightView->canvas->undo_stack->redo(); if (highlightStroke->pen().color()!=QColor(Qt::blue)) return 95;
    QTest::mouseClick(highlightPanel->rect_btn,Qt::LeftButton);
    hdrag(hp(350,300),hp(500,400));
    jietuba::RectItem *highlightRect=nullptr;
    for (auto *candidate:highlightView->canvas->items()) if (auto *rect=dynamic_cast<jietuba::RectItem *>(candidate); rect && rect->is_highlighter_rect) highlightRect=rect;
    if (!highlightRect || highlightView->editor->active_layer()!=highlightRect || highlightExport().pixelColor(420,350)==highlightSource.pixelColor(420,350)) return 96;
    QMetaObject::invokeMethod(highlightPanel,"color_changed",Q_ARG(QColor,QColor(Qt::red)));
    if (highlightRect->pen().color()!=QColor(Qt::red) || highlightRect->brush().color()!=QColor(Qt::red)) return 97;
    highlightView->canvas->undo_stack->undo();
    // 旧笔迹改为蓝色不应覆盖默认黄色；新矩形撤销自身修改后仍恢复默认黄色。
    // Editing an old stroke blue must not replace the yellow default used by the new rectangle.
    if (highlightRect->pen().color()!=QColor(Qt::yellow) || highlightRect->brush().color()!=QColor(Qt::yellow)) return 98;
    highlightView->canvas->undo_stack->redo();
    if (highlightRect->pen().color()!=QColor(Qt::red) || highlightRect->brush().color()!=QColor(Qt::red)) return 99;
    highlightPanel->opacity_spin->setValue(50);
    if (highlightRect->pen().color().alpha()<120 || highlightRect->pen().color().alpha()>135 || highlightRect->brush().color().alpha()!=highlightRect->pen().color().alpha()) return 102;
    highlightView->canvas->undo_stack->undo(); if (highlightRect->pen().color().alpha()!=255 || highlightRect->brush().color().alpha()!=255) return 103;
    highlightView->canvas->undo_stack->redo(); if (highlightRect->brush().color().alpha()!=highlightRect->pen().color().alpha()) return 104;
    hdrag(hp(420,350),hp(450,370));
    if (qAbs(highlightRect->pos().x()-30)>2 || qAbs(highlightRect->pos().y()-20)>2) return 105;
    highlightView->canvas->undo_stack->undo(); if (qAbs(highlightRect->pos().x())>2) return 106;
    highlightView->canvas->undo_stack->redo(); if (qAbs(highlightRect->pos().x()-30)>2) return 107;
    const auto highlightSelection=highlightView->canvas->selection_model->rect();
    const auto highlightExpected=jietuba::ExportService(highlightView->canvas).export_image(highlightSelection);
    QTest::mouseClick(highlightBar->confirm_btn,Qt::LeftButton);
    const QImage highlightCopy=QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png"));
    if (highlightCopy.convertToFormat(QImage::Format_ARGB32)!=highlightExpected.convertToFormat(QImage::Format_ARGB32)) return 100;
    hdrag(hp(20,20),hp(780,580));
    QTest::mouseClick(highlightBar->highlighter_btn,Qt::LeftButton);
    QTest::mouseClick(highlightPanel->rect_btn,Qt::LeftButton);
    hdrag(hp(350,300),hp(500,400));
    const auto highlightSelection2=highlightView->canvas->selection_model->rect();
    QTemporaryDir highlightDir; const auto highlightPath=highlightDir.filePath("highlight.png");
    QTimer::singleShot(0,[highlightPath] { if (auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { dialog->selectFile(highlightPath); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection); } });
    QTest::mouseClick(highlightBar->save_btn,Qt::LeftButton);
    const QImage highlightSaved(highlightPath);
    if (highlightSaved.isNull() || highlightSaved.pixelColor(qRound(420-highlightSelection2.left()),qRound(350-highlightSelection2.top()))==highlightSource.pixelColor(420,350)) return 101;
    // 按住 Shift 的多次移动应保持同一条直线，而非产生自由曲线。
    // Multiple movements while Shift is held must form a single straight segment.
    CapturePreview shiftWindow(highlightSource); shiftWindow.resize(800,600); shiftWindow.show(); QTest::qWait(30);
    auto *shiftView=dynamic_cast<PreviewView *>(shiftWindow.findChild<QGraphicsView *>());
    auto *shiftBar=shiftWindow.findChild<jietuba::Toolbar *>();
    if (!shiftView || !shiftBar) return 108;
    auto sp=[&](int x,int y) { return shiftView->mapFromScene(QPointF(x,y)); };
    QTest::mousePress(shiftView->viewport(),Qt::LeftButton,Qt::NoModifier,sp(20,20)); QTest::mouseMove(shiftView->viewport(),sp(780,580),10); QTest::mouseRelease(shiftView->viewport(),Qt::LeftButton,Qt::NoModifier,sp(780,580)); QTest::qWait(10);
    QTest::mouseClick(shiftBar->highlighter_btn,Qt::LeftButton);
    QTest::keyPress(shiftView->viewport(),Qt::Key_Shift);
    QTest::mousePress(shiftView->viewport(),Qt::LeftButton,Qt::ShiftModifier,sp(100,200));
    QMouseEvent shiftMove1(QEvent::MouseMove,QPointF(sp(200,220)),QPointF(shiftView->viewport()->mapToGlobal(sp(200,220))),Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    QMouseEvent shiftMove2(QEvent::MouseMove,QPointF(sp(280,260)),QPointF(shiftView->viewport()->mapToGlobal(sp(280,260))),Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    QCoreApplication::sendEvent(shiftView->viewport(), &shiftMove1); QCoreApplication::sendEvent(shiftView->viewport(), &shiftMove2);
    QTest::mouseRelease(shiftView->viewport(),Qt::LeftButton,Qt::ShiftModifier,sp(280,260));
    QTest::keyRelease(shiftView->viewport(),Qt::Key_Shift);
    jietuba::StrokeItem *straight=nullptr;
    for (auto *candidate:shiftView->canvas->items()) if (auto *stroke=dynamic_cast<jietuba::StrokeItem *>(candidate); stroke && stroke->is_highlighter) straight=stroke;
    if (!straight || straight->path().elementCount()!=2) return 109;
    // 端到端验证聚光灯共享遮罩、水印编辑和合成贴图。
    // Exercise spotlight, watermark and flattened pin through the real toolbar.
    QImage featureSource(800,600,QImage::Format_RGB32); featureSource.fill(Qt::white);
    QTemporaryDir featureDir; LinuxSettings featureSettings(featureDir.filePath("settings.ini"));
    CapturePreview featureWindow(featureSource, &featureSettings); featureWindow.resize(800,600); featureWindow.show(); QTest::qWait(30);
    auto *featureView=dynamic_cast<PreviewView *>(featureWindow.findChild<QGraphicsView *>());
    auto *featureBar=featureWindow.findChild<jietuba::Toolbar *>();
    auto *spotPanel=featureWindow.findChild<jietuba::PaintSettingsPanel *>("linuxSpotlightPanel");
    auto *wmPanel=featureWindow.findChild<jietuba::WatermarkSettingsPanel *>("linuxWatermarkPanel");
    if (!featureView || !featureBar || !spotPanel || !wmPanel || !featureBar->spotlight_btn->isEnabled() || !featureBar->watermark_btn->isEnabled() || !featureBar->pin_btn->isEnabled()) return 123;
    auto fp=[&](int x,int y) { return featureView->mapFromScene(QPointF(x,y)); };
    auto fdrag=[&](QPoint a,QPoint b) { QTest::mousePress(featureView->viewport(),Qt::LeftButton,Qt::NoModifier,a); QTest::mouseMove(featureView->viewport(),b,10); QTest::mouseRelease(featureView->viewport(),Qt::LeftButton,Qt::NoModifier,b); QTest::qWait(10); };
    fdrag(fp(20,20),fp(780,580));
    featureBar->more_btn->click(); featureBar->spotlight_btn->click();
    if (!spotPanel->isVisible()) return 124;
    fdrag(fp(200,150),fp(400,300));
    jietuba::SpotlightItem *spot=nullptr;
    for (auto *candidate:featureView->canvas->items()) if (auto *item=dynamic_cast<jietuba::SpotlightItem *>(candidate)) spot=item;
    auto *curtain=jietuba::SpotlightCurtain::find(featureView->canvas);
    if (!spot || !curtain) return 125;
    auto featureExport=[&] { return jietuba::ExportService(featureView->canvas).export_image(QRectF(0,0,800,600)); };
    auto lit=featureExport();
    if (lit.pixelColor(250,200)!=QColor(Qt::white) || lit.pixelColor(100,100).red()>100) return 126;
    spotPanel->opacity_spin->setValue(50);
    if (qAbs(curtain->opacity()-.5)>.02) return 127;
    featureView->canvas->undo_stack->undo(); if (qAbs(curtain->opacity()-.7)>.02) return 128;
    featureView->canvas->undo_stack->redo(); if (qAbs(curtain->opacity()-.5)>.02) return 129;
    fdrag(fp(250,200),fp(280,220));
    if (qAbs(spot->pos().x()-30)>2 || featureExport().pixelColor(210,160).red()>150) return 130;
    featureView->canvas->undo_stack->undo(); if (qAbs(spot->pos().x())>2 || featureExport().pixelColor(210,160)!=QColor(Qt::white)) return 131;
    featureView->canvas->undo_stack->redo(); if (qAbs(spot->pos().x()-30)>2) return 132;
    const auto spotlightRect=spot->rect();
    const auto spotHandle=std::find_if(featureView->editor->handles.begin(),featureView->editor->handles.end(),[](const auto &h) { return h.handle_type==jietuba::HandleType::CORNER_BR; });
    if (spotHandle==featureView->editor->handles.end()) return 154;
    fdrag(featureView->mapFromScene(spotHandle->position),featureView->mapFromScene(spotHandle->position+QPointF(30,20)));
    if (spot->rect().width()<spotlightRect.width()+28) return 155;
    featureView->canvas->undo_stack->undo(); if (spot->rect()!=spotlightRect) return 156;
    featureView->canvas->undo_stack->redo(); if (spot->rect().width()<spotlightRect.width()+28) return 157;
    fdrag(fp(500,150),fp(600,260));
    if (featureExport().pixelColor(550,200)!=QColor(Qt::white) || featureExport().pixelColor(280,220)!=QColor(Qt::white)) return 147;
    featureView->canvas->undo_stack->undo(); if (featureExport().pixelColor(550,200).red()>150) return 148;
    featureView->canvas->undo_stack->redo(); if (featureExport().pixelColor(550,200)!=QColor(Qt::white)) return 149;
    fdrag(fp(650,300),fp(550,200));
    if (featureExport().pixelColor(575,225)!=QColor(Qt::white) || featureExport().pixelColor(625,250)!=QColor(Qt::white)) return 158;
    featureView->canvas->undo_stack->undo(); if (featureExport().pixelColor(625,250).red()>150) return 159;
    featureView->canvas->undo_stack->redo(); if (featureExport().pixelColor(625,250)!=QColor(Qt::white)) return 160;
    const auto beforeWatermark=featureExport();
    // 原版水印输入框被接受后应创建图层，编辑和删除都能撤销。
    // The original watermark dialog creates a layer with undoable edits and removal.
    QTimer::singleShot(0, [] { if (auto *dialog=qobject_cast<jietuba::WatermarkInputDialog *>(QApplication::activeModalWidget())) { dialog->edit->setPlainText(QStringLiteral("TEST")); dialog->accept(); } });
    featureBar->more_btn->click(); featureBar->watermark_btn->click();
    auto *wm=jietuba::WatermarkItem::find(featureView->canvas);
    if (!wm || wm->template_text()!="TEST" || !wmPanel->isVisible()) return 133;
    const auto withWatermark=featureExport();
    if (withWatermark==beforeWatermark) return 134;
    const int centerIndex=wmPanel->layout_combo->findData(QStringLiteral("center"));
    if (centerIndex<0) return 135;
    wmPanel->layout_combo->setCurrentIndex(centerIndex);
    if (wm->style().layout!="center") return 136;
    featureView->canvas->undo_stack->undo(); if (wm->style().layout!="tile") return 137;
    featureView->canvas->undo_stack->redo(); if (wm->style().layout!="center") return 138;
    wmPanel->show_style_popup(); QTest::qWait(10);
    auto *wmPopup=featureWindow.findChild<jietuba::WatermarkStylePopup *>();
    if (!wmPopup || !wmPopup->isVisible() || wmPopup->findChildren<QSlider *>().isEmpty()) return 150;
    auto *opacitySlider=wmPopup->findChildren<QSlider *>().first();
    opacitySlider->setSliderDown(true); opacitySlider->setValue(60);
    QEvent popupLeave(QEvent::Leave); QApplication::sendEvent(wmPopup,&popupLeave); QTest::qWait(400);
    if (!wmPopup->isVisible() || qAbs(wm->style().opacity-.6)>.01) return 151;
    opacitySlider->setSliderDown(false); wmPopup->hide();
    featureView->canvas->undo_stack->undo(); if (qAbs(wm->style().opacity-.35)>.01) return 152;
    featureView->canvas->undo_stack->redo(); if (qAbs(wm->style().opacity-.6)>.01) return 153;
    LinuxSettings savedWatermarkSettings(featureDir.filePath("settings.ini"));
    if (savedWatermarkSettings.watermarkText()!="TEST" || savedWatermarkSettings.watermarkStyle().value("layout").toString()!="center" || qAbs(savedWatermarkSettings.watermarkStyle().value("opacity").toDouble()-.6)>.01) return 161;
    QMetaObject::invokeMethod(wmPanel,"text_cleared");
    if (jietuba::WatermarkItem::find(featureView->canvas)) return 139;
    featureView->canvas->undo_stack->undo();
    wm=jietuba::WatermarkItem::find(featureView->canvas);
    if (!wm || wm->template_text()!="TEST") return 140;
    // 图钉应接收完整导出，包括聚光灯、水印和原始像素。
    // Pin receives exactly the composited selection, including spotlight and watermark.
    const auto featureRect=featureView->canvas->selection_model->rect();
    const auto expectedPin=jietuba::ExportService(featureView->canvas).export_image(featureRect);
    QImage pinned; QRectF pinnedRect;
    featureWindow.setPinHandler([&](const QImage &image,QScreen *,const QRectF &rect) { pinned=image; pinnedRect=rect; });
    if (!featureBar->pin_btn->isVisible()) featureBar->more_btn->click();
    featureBar->pin_btn->click();
    if (pinned!=expectedPin || pinned.isNull() || !pinnedRect.isValid()) return 141;
    QPointer<LinuxPinWindow> pin=new LinuxPinWindow(pinned,featureSettings);
    pin->showOnScreen(QGuiApplication::primaryScreen(), pinnedRect); QTest::qWait(20);
    if (!pin || !pin->isVisible() || !(pin->windowFlags() & Qt::WindowStaysOnTopHint) || pin->image()!=expectedPin) return 142;
    if (pin->size() != pinnedRect.size().toSize()) return 163;
    const auto renderedPin = pin->grab().toImage();
    if (renderedPin.pixelColor(0, 0) != QColor("#8cc8ed") || renderedPin.pixelColor(1,1)==QColor("#8cc8ed")) return 164;
    // 右键菜单至少应能切换边框和恢复原大小，且关闭菜单不会销毁贴图。
    // A dark desktop palette must not turn menu labels white against its light background.
    const QPalette oldPalette = QApplication::palette();
    QPalette darkPalette = oldPalette;
    darkPalette.setColor(QPalette::Window, QColor("#202020"));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    QApplication::setPalette(darkPalette);
    bool menuSeen = false, menuReadable = false;
    QTimer::singleShot(0, pin, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        menuSeen = true;
        auto actions = menu->actions();
        const QImage menuImage = menu->grab().toImage();
        const QRect firstItem = menu->actionGeometry(actions.first()).adjusted(12, 2, -12, -2);
        int darkPixels = 0;
        for (int y = firstItem.top(); y <= firstItem.bottom(); ++y)
            for (int x = firstItem.left(); x <= firstItem.right(); ++x)
                if (menuImage.rect().contains(x, y) && qGray(menuImage.pixel(x, y)) < 100) ++darkPixels;
        menuReadable = darkPixels > 5;
        auto border = std::find_if(actions.begin(), actions.end(), [](QAction *action) { return action->text() == QStringLiteral("显示边框"); });
        if (border != actions.end()) { menu->setActiveAction(*border); QTest::keyClick(menu, Qt::Key_Return); }
        else menu->close();
    });
    QTimer::singleShot(500, pin, [] { if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close(); });
    QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(10, 10), pin->mapToGlobal(QPoint(10, 10)));
    QCoreApplication::sendEvent(pin, &context);
    QApplication::setPalette(oldPalette);
    if (!menuSeen || !menuReadable || pin->grab().toImage().pixelColor(0, 0) == QColor("#8cc8ed")) return 167;
    const auto zoomBefore=pin->zoom();
    QWheelEvent wheel(QPointF(10,10),QPointF(pin->mapToGlobal(QPoint(10,10))),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(pin,&wheel);
    if (pin->zoom()<=zoomBefore) return 143;
    QTest::keyClick(pin,Qt::Key_C,Qt::ControlModifier);
    if (QImage::fromData(QApplication::clipboard()->mimeData()->data("image/png")).convertToFormat(QImage::Format_ARGB32)!=pinned.convertToFormat(QImage::Format_ARGB32)) return 144;
    const auto pinPath=featureDir.filePath("pin.png");
    QTimer::singleShot(0,[pinPath] { if (auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { dialog->selectFile(pinPath); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection); } });
    QTest::keyClick(pin,Qt::Key_S,Qt::ControlModifier);
    if (QImage(pinPath).convertToFormat(QImage::Format_ARGB32)!=pinned.convertToFormat(QImage::Format_ARGB32)) return 145;
    QTest::keyClick(pin,Qt::Key_Escape); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    if (pin) return 146;
    QImage thin(400,10,QImage::Format_RGB32); thin.fill(Qt::white);
    QPointer<LinuxPinWindow> thinPin=new LinuxPinWindow(thin,featureSettings);
    thinPin->showOnScreen(QGuiApplication::primaryScreen());
    if (!thinPin || qAbs(double(thinPin->width()) / thinPin->height() - 40.0)>.1) return 162;
    thinPin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    // 异步 OCR 窗口与识别器解耦；验证结果可编辑、可复制，并能独立于截图窗口存活。
    // Inject a deterministic worker result to exercise the real asynchronous result UI and clipboard.
    QPointer<LinuxOcrResultWindow> ocrWindow = new LinuxOcrResultWindow(featureSource, nullptr,
        [](const QImage &) { return LinuxOcrResult{QStringLiteral("TEST OCR"), QStringLiteral("test"), {}}; });
    ocrWindow->show();
    QElapsedTimer ocrWait; ocrWait.start();
    while (ocrWindow && ocrWindow->isWorking() && ocrWait.elapsed() < 2000) QTest::qWait(10);
    QCoreApplication::processEvents();
    if (!ocrWindow || ocrWindow->isWorking() || ocrWindow->text() != QStringLiteral("TEST OCR")) return 173;
    for (auto *button : ocrWindow->findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("复制文字")) button->click();
    if (QApplication::clipboard()->text() != QStringLiteral("TEST OCR")) return 174;
    ocrWindow->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (ocrWindow) return 175;
    // 快速保存走真实工具栏和原版导出，文件写到隔离目录且不覆盖先前截图。
    // Quick save uses the real toolbar/export path and writes into an isolated directory.
    featureSettings.setSaveDirectory(featureDir.path());
    if (!featureBar->quick_save_btn->isEnabled()) return 176;
    featureBar->quick_save_btn->click();
    const QStringList quickSaved = QDir(featureDir.path()).entryList({QStringLiteral("截图_*.png")}, QDir::Files);
    if (quickSaved.size() != 1 || QImage(featureDir.filePath(quickSaved.first())).convertToFormat(QImage::Format_ARGB32)
                            != expectedPin.convertToFormat(QImage::Format_ARGB32)) return 177;
    // 真正点击 OCR 工具栏按钮：结果窗口独立打开，原选区按截图完成流程清理。
    // Click the real OCR action and confirm the independent result window replaces the capture selection.
    CapturePreview ocrPreview(featureSource, &featureSettings); ocrPreview.resize(800, 600); ocrPreview.show(); QTest::qWait(20);
    auto *ocrView = dynamic_cast<PreviewView *>(ocrPreview.findChild<QGraphicsView *>());
    auto *ocrBar = ocrPreview.findChild<jietuba::Toolbar *>();
    if (!ocrView || !ocrBar || !ocrBar->text_recognize_btn->isEnabled()) return 178;
    const QPoint ocrStart = ocrView->mapFromScene(QPointF(40, 40));
    const QPoint ocrEnd = ocrView->mapFromScene(QPointF(400, 220));
    QTest::mousePress(ocrView->viewport(), Qt::LeftButton, Qt::NoModifier, ocrStart);
    QTest::mouseMove(ocrView->viewport(), ocrEnd, 10);
    QTest::mouseRelease(ocrView->viewport(), Qt::LeftButton, Qt::NoModifier, ocrEnd);
    ocrBar->text_recognize_btn->click();
    LinuxOcrResultWindow *openedResult = nullptr;
    for (auto *top : QApplication::topLevelWidgets()) {
        openedResult = qobject_cast<LinuxOcrResultWindow *>(top);
        if (openedResult) break;
    }
    if (!openedResult || !openedResult->isVisible() || !ocrView->canvas->selection_model->is_empty()) return 179;
    openedResult->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#ifdef JIETUBA_HAS_ZXING
    // 使用原版多码夹具走真正的 Linux 扫码按钮、后台解码和复制按钮。
    // Exercise the scan action, asynchronous decoder and copy control with original fixtures.
    const QImage barcodeImage(QStringLiteral(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.png"));
    if (barcodeImage.isNull()) return 180;
    CapturePreview barcodePreview(barcodeImage, &featureSettings); barcodePreview.resize(900, 560); barcodePreview.show(); QTest::qWait(20);
    auto *barcodeView = dynamic_cast<PreviewView *>(barcodePreview.findChild<QGraphicsView *>());
    auto *barcodeBar = barcodePreview.findChild<jietuba::Toolbar *>();
    if (!barcodeView || !barcodeBar || !barcodeBar->scan_code_btn->isEnabled()) return 181;
    const QPoint barcodeStart = barcodeView->mapFromScene(QPointF(10, 10));
    const QPoint barcodeEnd = barcodeView->mapFromScene(QPointF(890, 550));
    QTest::mousePress(barcodeView->viewport(), Qt::LeftButton, Qt::NoModifier, barcodeStart);
    QTest::mouseMove(barcodeView->viewport(), barcodeEnd, 10);
    QTest::mouseRelease(barcodeView->viewport(), Qt::LeftButton, Qt::NoModifier, barcodeEnd);
    barcodeBar->scan_code_btn->click();
    LinuxBarcodeResultWindow *barcodeResult = nullptr;
    for (auto *top : QApplication::topLevelWidgets()) {
        barcodeResult = qobject_cast<LinuxBarcodeResultWindow *>(top);
        if (barcodeResult) break;
    }
    if (!barcodeResult || !barcodeResult->isVisible() || !barcodeView->canvas->selection_model->is_empty()) return 182;
    QElapsedTimer barcodeWait; barcodeWait.start();
    while (barcodeResult->isWorking() && barcodeWait.elapsed() < 2000) QTest::qWait(10);
    QCoreApplication::processEvents();
    if (barcodeResult->decodedTexts().isEmpty() || !barcodeResult->decodedTexts().contains(QStringLiteral("Second code"))) return 183;
    for (auto *button : barcodeResult->findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("复制内容")) button->click();
    if (QApplication::clipboard()->text() != barcodeResult->decodedTexts().first()) return 184;
    barcodeResult->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#endif
    // 没配密钥时翻译窗口只显示本地 OCR 原文和设置提示，不能悄悄发网络请求。
    // With no credentials, translation shows OCR text but never sends a network request.
    QPointer<LinuxTranslationResultWindow> translationResult = new LinuxTranslationResultWindow(featureSource, featureSettings, nullptr,
        [](const QImage &) { return LinuxOcrResult{QStringLiteral("Hello"), QStringLiteral("test"), {}}; });
    translationResult->show();
    QElapsedTimer translationWait; translationWait.start();
    while (translationResult->isRecognizing() && translationWait.elapsed() < 2000) QTest::qWait(10);
    QCoreApplication::processEvents();
    if (translationResult->sourceText() != QStringLiteral("Hello") || !translationResult->translatedText().isEmpty()) return 185;
    translationResult->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // 截图翻译按钮必须将选区底图交给独立结果窗口，并结束当前选区。
    // The screenshot-translate action opens its own result window and ends the capture selection.
    CapturePreview translationPreview(featureSource, &featureSettings); translationPreview.resize(800, 600); translationPreview.show(); QTest::qWait(20);
    auto *translationView = dynamic_cast<PreviewView *>(translationPreview.findChild<QGraphicsView *>());
    auto *translationBar = translationPreview.findChild<jietuba::Toolbar *>();
    if (!translationView || !translationBar || !translationBar->screenshot_translate_btn->isEnabled()) return 186;
    translationPreview.runOnSelection("translate");
    const QPoint translationStart = translationView->mapFromScene(QPointF(40, 40));
    const QPoint translationEnd = translationView->mapFromScene(QPointF(400, 220));
    QTest::mousePress(translationView->viewport(), Qt::LeftButton, Qt::NoModifier, translationStart);
    QTest::mouseMove(translationView->viewport(), translationEnd, 10);
    QTest::mouseRelease(translationView->viewport(), Qt::LeftButton, Qt::NoModifier, translationEnd);
    LinuxTranslationResultWindow *openedTranslation = nullptr;
    for (auto *top : QApplication::topLevelWidgets()) {
        openedTranslation = qobject_cast<LinuxTranslationResultWindow *>(top);
        if (openedTranslation) break;
    }
    if (!openedTranslation || !openedTranslation->isVisible() || !translationView->canvas->selection_model->is_empty()) return 187;
    openedTranslation->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // 翻译设置沿用原版卡片列宽；密钥框不能超过 200px 控制区而被裁切。
    // Translation credential edits must fit the original card's 200px control column.
    LinuxSettingsWindow translationSettings(featureSettings); translationSettings.showTranslationPage(); QTest::qWait(20);
    auto *providerChoice = translationSettings.findChild<jietuba::ComboBox *>(QStringLiteral("translationProvider"));
    auto *deepLKey = translationSettings.findChild<jietuba::LineEdit *>(QStringLiteral("translationApiKey"));
    auto *googleKey = translationSettings.findChild<jietuba::LineEdit *>(QStringLiteral("googleApiKey"));
    auto *youdaoId = translationSettings.findChild<jietuba::LineEdit *>(QStringLiteral("youdaoAppId"));
    auto *youdaoSecret = translationSettings.findChild<jietuba::LineEdit *>(QStringLiteral("youdaoAppSecret"));
    if (!providerChoice || !deepLKey || !googleKey || !youdaoId || !youdaoSecret) return 188;
    for (auto *field : {deepLKey, googleKey, youdaoId, youdaoSecret})
        if (field->width() > jietuba::SettingCard::CONTROL_COLUMN_MAX
            || field->geometry().right() > field->parentWidget()->rect().right()) return 189;
    providerChoice->setCurrentIndex(providerChoice->findData(QStringLiteral("google")));
    if (!googleKey->isVisibleTo(&translationSettings) || deepLKey->isVisibleTo(&translationSettings)) return 190;
    providerChoice->setCurrentIndex(providerChoice->findData(QStringLiteral("youdao")));
    if (!youdaoId->isVisibleTo(&translationSettings) || !youdaoSecret->isVisibleTo(&translationSettings)
        || googleKey->isVisibleTo(&translationSettings)) return 191;
    // 原版的导航页应有对应的多组内容；未接入项只显示状态，不能成为可操作的假开关。
    // Each original navigation page now has its own groups; pending features are status-only rows.
    auto *settingsPages = translationSettings.findChild<QStackedWidget *>();
    if (!settingsPages || settingsPages->count() != 12) return 192;
    const int minimumGroups[] = {0, 3, 9, 8, 2, 3, 3, 5, 2, 3, 1, 4};
    for (int index = 0; index < settingsPages->count(); ++index)
        if (settingsPages->widget(index)->findChildren<jietuba::SettingCardGroup *>().size() < minimumGroups[index]) return 193;
    bool openedDeveloper = false;
    for (auto *action : translationSettings.findChildren<QAction *>()) {
        if (action->objectName() != "openDeveloperOptions") continue;
        action->trigger();
        openedDeveloper = settingsPages->currentWidget()->findChildren<QLabel *>("pendingSettingStatus").size() == 9;
        break;
    }
    if (!openedDeveloper) return 201;
    // 标题不应被错误的初始 sizeHint 拉成大片空白；缩小窗口后也需保持可滚动。
    // Group headings stay compact after layout and after a narrow-window resize.
    settingsPages->setCurrentIndex(3); QTest::qWait(15);
    for (auto *group : settingsPages->widget(3)->findChildren<jietuba::SettingCardGroup *>())
        if (group->isVisibleTo(&translationSettings) && group->titleLabel->height() > group->titleLabel->sizeHint().height() + 2) return 199;
    translationSettings.resize(600, 400); QTest::qWait(20);
    for (auto *group : settingsPages->widget(3)->findChildren<jietuba::SettingCardGroup *>())
        if (group->isVisibleTo(&translationSettings) && group->titleLabel->height() > group->titleLabel->sizeHint().height() + 2) return 200;
    if (!translationSettings.findChild<jietuba::SwitchSettingCard *>("pin_lock_block_move") ||
        !translationSettings.findChild<jietuba::ComboBox *>("pin_fine_zoom_step")) return 194;
    auto *pinZoom = translationSettings.findChild<jietuba::ComboBox *>(QStringLiteral("pinZoomStep"));
    auto *pinBorder = translationSettings.findChild<jietuba::SwitchSettingCard *>(QStringLiteral("pinAutoBorder"));
    if (!pinZoom || !pinBorder) return 195;
    pinZoom->setCurrentIndex(pinZoom->findData(25)); pinBorder->setChecked(false);
    if (featureSettings.pinZoomStep() != 25 || featureSettings.pinBorderEnabled()) return 196;
    QImage pinSource(80, 60, QImage::Format_RGB32); pinSource.fill(Qt::white);
    QPointer<LinuxPinWindow> configuredPin = new LinuxPinWindow(pinSource, featureSettings);
    configuredPin->showOnScreen(QGuiApplication::primaryScreen()); QTest::qWait(10);
    if (configuredPin->grab().toImage().pixelColor(0, 0) == QColor("#8cc8ed")) return 197;
    QWheelEvent configuredWheel(QPointF(20, 15), QPointF(configuredPin->mapToGlobal(QPoint(20, 15))),
                               QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(configuredPin, &configuredWheel);
    if (qAbs(configuredPin->zoom() - 1.25) > .001) return 198;
    configuredPin->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::fprintf(stderr,"mosaic, numbering, highlighter, spotlight, watermark, pin, OCR and quick save passed\n"); return 0;
}
#include "mosaic_ui_test.moc"
