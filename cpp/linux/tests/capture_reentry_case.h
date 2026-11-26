#pragma once
#include "../capture/kwin_capture.h"
#include "../kwin_pointer_tracker.h"
#include <QDBusConnectionInterface>
#include <QScopeGuard>
#include <QTest>
#include <cstdio>

// 此用例由 mosaic_ui_test 引入，共用生产版 CaptureSession、真实编码器和 D-Bus 区域采集。
// Included by mosaic_ui_test to exercise the production session, encoder and D-Bus capture path together.
namespace capture_reentry_test {
template<class T> T *visibleWindow() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *window = dynamic_cast<T *>(widget); window && window->isVisible() && !window->windowHandle()->mask().contains(QPoint(-2,-2))) return window;
    return nullptr;
}

inline int run(QApplication &app, bool virtualKWin) {
    const auto fail = [](const char *reason) { std::fprintf(stderr, "Capture reentry: %s\n", reason); return 1; };
    if (LinuxGifFrames::ffmpegPath().isEmpty()) return 77;
    QProcess fixture;
    if (virtualKWin) {
        // 虚拟合成器必须由隔离的启动器创建；防止误在用户当前的桌面上运行录制测试。
        // Only use the private compositor launched for this test, never the user's desktop.
        if (app.platformName() != "wayland" || qgetenv("WAYLAND_DISPLAY") != "wayland-jietuba-test")
            return fail("expected the isolated KWin test display");
    } else {
        if (app.platformName() != "offscreen") return fail("expected an offscreen fixture");
        auto *bus = QDBusConnection::sessionBus().interface();
        const QString service = QStringLiteral("org.kde.KWin.ScreenShot2");
        if (!bus || bus->isServiceRegistered(service).value()) return fail("expected an empty private bus");
        qputenv("XDG_CURRENT_DESKTOP", "KDE");
        qputenv("WAYLAND_DISPLAY", "jietuba-test-no-compositor");
        fixture.start(QCoreApplication::applicationDirPath() + "/kwin_capture_test", {"--serve-areas"});
        if (!fixture.waitForStarted(2000) || !QTest::qWaitFor([&] { return bus->isServiceRegistered(service).value(); }, 2000))
            return fail("private ScreenShot2 fixture did not start");
    }
    const auto cleanup = qScopeGuard([&] {
        if (fixture.state() != QProcess::NotRunning) { fixture.terminate(); fixture.waitForFinished(2000); }
    });
    app.setQuitOnLastWindowClosed(false);
    app.setDesktopFileName("jietuba-capture-reentry-test");
    jietuba::get_ui_theme()->init(nullptr, &app);
    QTemporaryDir files;
    if (!files.isValid()) return fail("could not allocate a private recording directory");
    LinuxSettings settings(files.filePath("settings.ini"));
    settings.setCaptureAllScreens(false); settings.setGlobalShortcutSlots("capture", {"Alt+1"});
    settings.setGifDelay(0); settings.setSaveDirectory(files.path()); settings.setRecordingMode("mp4");
    auto video = settings.videoOptions(); video.hardware = video.system_audio = false; video.fps = 12;
    settings.setVideoOptions(video);
    QWidget desktop;
    desktop.setStyleSheet("background: #27648a"); desktop.showFullScreen();
    if (virtualKWin) {
        // 直接向测试合成器读取已知桌面色，证明没有替换为模拟服务或误用真实用户屏幕。
        // Read a known desktop pixel from the private KWin itself, not from a mock or the user's display.
        if (!QTest::qWaitForWindowExposed(&desktop, 2500)) return fail("virtual desktop not exposed");
        const auto *output = app.primaryScreen();
        const QRect sample(output->geometry().center(), QSize(24, 16));
        KWinCapture backend(CallerWindowPolicy::Include);
        std::atomic_bool cancelled{false};
        const bool correct = QTest::qWaitFor([&] {
            const auto probe = backend.captureRegion(output->name(), sample, output->geometry(), cancelled);
            return !probe.image.isNull() && probe.image.pixelColor(8,8) == QColor("#27648a");
        }, 2000);
        if (!correct) return fail("KWin did not return the known virtual desktop pixels");
        // 用合成后的像素确认穿透时图片仍在，点击退出能恢复原透明度和输入区。
        // Probe the composited image: input pass-through must not erase its visible pixels.
        settings.setPinFlag("auto_ocr",false);
        QImage red(180,90,QImage::Format_RGB32); red.fill(Qt::red);
        const QPoint origin=sample.topLeft()-QPoint(70,35);
        QPushButton beneath(&desktop); beneath.setObjectName("underlyingButton"); beneath.setStyleSheet("background:#27648a; border:none");
        beneath.setGeometry(QRect(desktop.mapFromGlobal(sample.center())-QPoint(25,15),QSize(50,30))); beneath.show();
        QSignalSpy clicked(&beneath,&QAbstractButton::clicked);
        const auto nativeClick=[](QPoint pt,const QString &gesture=QString()){
            QProcess driver;
            driver.setProcessChannelMode(QProcess::ForwardedChannels);
            QStringList args{QString::number(pt.x()),QString::number(pt.y())};
            if(!gesture.isEmpty()) args.append(gesture);
            driver.start(qEnvironmentVariable("JIETUBA_TEST_CLICK"),args);
            return QTest::qWaitFor([&]{return driver.state()==QProcess::NotRunning;},4000) && driver.exitCode()==0;
        };
        if(!nativeClick(sample.center()) || !QTest::qWaitFor([&]{return clicked.count()==1;},1500)) return fail("test input driver could not click desktop button");
        clicked.clear();
        QPointer<LinuxPinWindow> pin=new LinuxPinWindow(red,settings);
        pin->showOnScreen(app.primaryScreen(),QRectF(origin,red.size()));
        const auto sampleColor=[&] {
            const auto frame=backend.captureRegion(output->name(),sample,output->geometry(),cancelled);
            return frame.image.isNull() ? QColor() : frame.image.pixelColor(8,8);
        };
        if(!QTest::qWaitFor([&]{return sampleColor()==Qt::red;},2500)) return fail("pin pixels not visible");
        if(!nativeClick(sample.center()) || QTest::qWaitFor([&]{return !clicked.isEmpty();},100)) return fail("ordinary pin did not intercept click");
        // 标注工具栏可能已使部分控件原生化，穿透必须覆盖这一条实际使用路径。
        // Also cover native children created by opening an annotation toolbar.
        settings.setPinPercent("fine_zoom_step",3);
        if(!nativeClick(sample.center(),"alt-wheel") || !QTest::qWaitFor([&]{return qAbs(pin->zoom()-1.03)<1e-6;},1000)) return fail("native Alt wheel did not fine-zoom the pin");
        pin->triggerShortcut("pin_reset");
        pin->toggleToolbar(); QTest::qWait(80);
        if(!nativeClick(sample.center(),"alt-wheel") || !QTest::qWaitFor([&]{return qAbs(pin->zoom()-1.03)<1e-6;},1000)) return fail("native Alt wheel did not fine-zoom the canvas");
        pin->triggerShortcut("pin_reset");
        if(!QTest::qWaitFor([&]{return sampleColor()==Qt::red;},1000)) return fail("opening toolbar erased the pin");
        pin->setClickThrough(true);
        auto *host=pin->window(); QWidget *bar=nullptr;
        for(auto *top:QApplication::topLevelWidgets()) if(top->objectName()=="pinThroughBar" && top->isVisible()) {bar=top;break;}
        if(!bar || !bar->isVisible() || host->windowHandle()->mask().contains(origin+QPoint(70,35)) ||
           (!bar->isWindow() || bar->windowHandle()==host->windowHandle())) return fail("pin input mask did not isolate the exit bar");
        if(!QTest::qWaitFor([&]{auto c=sampleColor();return c.red()>100 && c.red()<210 && c.blue()>20;},2500)) return fail("pass-through hid the pin or lost its opacity");
        if(!nativeClick(sample.center()) || !QTest::qWaitFor([&]{return clicked.count()==1;},1500)) return fail("underlying button did not receive composited click");
        auto *exit=bar->findChild<QPushButton *>();
        QPoint exitPosition=exit->mapToGlobal(exit->rect().center());
#ifdef JIETUBA_HAS_LAYER_SHELL
        // Layer-shell 的 Qt 顶层 pos() 可能恒为零，使用提交给合成器的输出边距定位点击。
        // Layer-shell has output-relative placement; Qt toplevel pos() is not the compositor's position.
        if(LinuxWayland::supportsLayerShell()) {
            const auto margins=LayerShellQt::Window::get(bar->windowHandle())->margins();
            exitPosition=output->geometry().topLeft()+QPoint(margins.left(),margins.top())+exit->mapTo(bar,exit->rect().center());
        }
#endif
        if(!nativeClick(exitPosition)) return fail("could not click exit");
        if(!QTest::qWaitFor([&]{return sampleColor()==Qt::red;},2500)) return fail("exiting click-through did not restore the pin");
        clicked.clear();
        if(!nativeClick(sample.center()) || QTest::qWaitFor([&]{return !clicked.isEmpty();},100)) return fail("exiting click-through left the image input disabled");
        if(pin->image().convertToFormat(QImage::Format_RGB32)!=red) return fail("opacity changed exported pixels");
        auto *pinBar=host->findChild<LinuxPinToolbar *>();
        pin->toggleToolbar(); QTest::qWait(80);
        pinBar->toolbar()->pen_btn->click();
        const QRect beforeResize=pin->geometry();
        const QPoint edge=beforeResize.topLeft()+QPoint(beforeResize.width()-3,beforeResize.height()/2);
        if(!nativeClick(edge,"drag:40:20") || !QTest::qWaitFor([&]{return pin->width()>beforeResize.width()+20;},1000)) return fail("native border drag did not resize an active canvas");
        if(qAbs(pin->width()-2*pin->height())>1 || pin->image().convertToFormat(QImage::Format_RGB32)!=red) return fail("border resize changed pixels or aspect ratio");
        pin->toggleToolbar(); pin->toggleToolbar(); QTest::qWait(80);
        const QPoint oldBarPoint=pinBar->toolbar()->geometry().topLeft()+QPoint(18,pinBar->toolbar()->height()/2);
        const auto barPixel=[&]{auto f=backend.captureRegion(output->name(),QRect(oldBarPoint,QSize(8,8)),output->geometry(),cancelled); return f.image.isNull()?QColor():f.image.pixelColor(0,0);};
        if(!QTest::qWaitFor([&]{return barPixel().isValid() && barPixel()!=QColor("#27648a");},1500)) return fail("toolbar not visible before move");
        if(!nativeClick(pin->geometry().center(),"drag:0:-160") || !QTest::qWaitFor([&]{return barPixel()==QColor("#27648a");},1500)) return fail("moving pin left a second toolbar image behind");
        if(host->findChildren<LinuxPinToolbar *>().size()!=1) return fail("duplicate toolbar host created");
        pin->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        std::fprintf(stderr,"Virtual KWin pin: native Alt wheel, edge resize, toolbar cleanup, pass-through and export verified\n");
    }
    QSystemTrayIcon tray;
    QAction record, toggle, stop;
    CaptureSession session(&desktop, settings, true);
    session.bindRecordingTray(&tray, &record, &toggle, &stop);
    // 测 Portal 已分发的动作以及 Qt 直接送到独占层的组合键，两条入口不可误停录屏。
    // Test both the dispatched portal action and a Qt chord delivered to an exclusive surface.
    GlobalShortcut shortcuts;
    QObject::connect(&shortcuts, &GlobalShortcut::activated, &session, &CaptureSession::globalShortcutActivated);
    const auto activate = [&] { emit shortcuts.activated("capture", "Alt+1"); };
    const auto selector = [] { return visibleWindow<LinuxRegionSelector>(); };
    const auto editor = [] { return visibleWindow<CapturePreview>(); };
    const auto wait = [](auto predicate) { return QTest::qWaitFor(predicate, 4500); };
    QString error;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &app, [&] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            error = dialog->text(); std::fprintf(stderr, "Capture dialog: %s\n", qPrintable(error)); dialog->accept();
        }
    });
    dialogs.start(30);
    const QRect recorded(64,70,500,350), firstArea(120,120,360,230), secondArea(150,150,160,90);
    const auto drag = [&](const QRect &r) {
        auto *live = selector();
        QTest::mousePress(live, Qt::LeftButton, Qt::NoModifier, r.topLeft());
        QTest::mouseRelease(live, Qt::LeftButton, Qt::NoModifier, r.topLeft()+QPoint(r.width(),r.height()));
    };
    if (virtualKWin) {
        activate();
        if (!wait(selector)) return fail("missing smart selector");
        auto *live=selector(); auto *tracker=live->findChild<KWinPointerTracker *>();
        if (!tracker) return fail("window detection disabled");
        QSignalSpy hits(tracker,&KWinPointerTracker::windowHit);
        if (!wait([&]{return !hits.isEmpty();})) return fail("KWin did not return a window hit");
        const auto sample=hits.last(); const QRect area=sample[1].toRect();
        if (area!=desktop.screen()->geometry()) return fail("smart target is not the test desktop window");
        QTest::mouseClick(live,Qt::LeftButton,Qt::NoModifier,sample[0].toPoint()-area.topLeft());
        if (!wait(editor) || editor()->captureBounds().size()!=area.size()) return fail("single click did not capture the detected window");
        QTest::keyClick(editor(),Qt::Key_Escape);
    }
    record.trigger();
    if (!wait(selector)) return fail("cannot select tray recording");
    drag(recorded);
    QPointer<LinuxGifCaptureWindow> outer;
    if (!wait([&] { outer = session.findChild<LinuxGifCaptureWindow *>(); return outer && outer->controlWindow(); }))
        return fail("recording controls did not open");
    QSignalSpy outerFrames(outer, &LinuxGifCaptureWindow::frameCaptured);
    toggle.trigger();
    if (!wait([&] { return outerFrames.count() >= 2; }) || record.isVisible() || !stop.isVisible())
        return fail("MP4 not capturing frames or incorrect running tray menu");
    toggle.trigger();
    if (record.isVisible() || !toggle.text().contains(QStringLiteral("继续"))) return fail("incorrect paused tray menu");
    toggle.trigger();
    if (outer->controlWindow()->isVisible()) QTest::keyClick(outer->controlWindow(), Qt::Key_1, Qt::AltModifier);
    else activate(); // 无 layer-shell 时从托盘/Portal 进入 / Tray-only desktops deliver the portal action.
    if (!wait(selector) || !outer->controlWindow()->isVisible()) return fail("recorder toolbar vanished during nested selection");
    const auto before = outerFrames.count();
    if (!wait([&] { return outerFrames.count() >= before + 2; })) return fail("outer stopped in live selection");
    // 在同一段 MP4 内建立 A、B，再用全屏 C 包住它们；验证交互权按 C→B→A 恢复。
    // Exercise both nesting directions while MP4 frames keep arriving; only the newest toolbar accepts input.
    drag(firstArea);
    if (!wait(editor) || editor()->captureBounds()!=firstArea) return fail("screenshot inside recorder rejected");
    QPointer<CapturePreview> parent = editor();
    auto *aBar = parent->findChild<jietuba::Toolbar *>();
    activate();
    if (!wait(selector)) return fail("missing second selector");
    drag(secondArea);
    if (!wait([&] { return editor() && editor()!=parent; })) return fail("nested crop rejected");
    QPointer<CapturePreview> child = editor();
    auto *bBar = child->findChild<jietuba::Toolbar *>();
    const auto clickBar = [](CapturePreview *front, CapturePreview *owner, QAbstractButton *button) {
        QTest::mouseClick(front->windowHandle(), Qt::LeftButton, Qt::NoModifier, button->mapTo(owner, button->rect().center()));
    };
    QSignalSpy staleClicks(aBar->cancel_btn, &QAbstractButton::clicked);
    const auto oldCancel = aBar->cancel_btn->mapTo(parent, aBar->cancel_btn->rect().center());
    if (!aBar->isVisible() || aBar->isEnabled() || !bBar->isEnabled() || bBar->geometry().contains(oldCancel))
        return fail("fixture did not expose parked toolbar separately");
    clickBar(child, parent, aBar->cancel_btn);
    if (staleClicks.count() || editor()!=child || !parent->isVisible()) return fail("old cancel stole input");
    activate();
    if (!wait(selector)) return fail("missing third selector");
    QTest::keyClick(selector(), Qt::Key_Return);
    if (!wait([&] { return editor() && editor()!=child; }) || bBar->isEnabled() || aBar->isEnabled())
        return fail("enclosing full-output crop did not lock both older toolbars");
    QTest::keyClick(editor(), Qt::Key_Escape);
    if (!wait([&] { return editor()==child; }) || !bBar->isEnabled() || aBar->isEnabled()) return fail("wrong toolbar resumed after third crop");
    clickBar(child, child, bBar->confirm_btn);
    if (!wait([&] { return !child && editor()==parent; }) || !aBar->isEnabled()) return fail("confirm did not restore first toolbar");
    activate();
    if (!wait(selector)) return fail("missing selector for second recording");
    drag(secondArea);
    if (!wait([&] { return editor() && editor()!=parent; })) return fail("overlapping recorder crop rejected");
    settings.setRecordingMode("gif");
    auto *tools = editor()->findChild<jietuba::Toolbar *>();
    if (!tools) return fail("capture toolbar missing");
    tools->gif_btn->click();
    QPointer<LinuxGifCaptureWindow> inner;
    if (!wait([&] {
        for (auto *r : session.findChildren<LinuxGifCaptureWindow *>()) if (r != outer && r->controlWindow()) inner = r;
        return !inner.isNull();
    }) || !parent->isVisible() || !toggle.text().contains("MP4")) return fail("ready child stole parent recording or tray");
    QSignalSpy innerFrames(inner, &LinuxGifCaptureWindow::frameCaptured);
    inner->toggle();
    const auto outerBefore = outerFrames.count();
    if (!wait([&] { return innerFrames.count() >= 2 && outerFrames.count() >= outerBefore + 2; })
        || !toggle.text().contains("GIF")) return fail("MP4 and GIF did not capture concurrently");
    activate();
    if (!wait(selector)) return fail("two recorders blocked screenshot selector");
    drag(secondArea.adjusted(5,5,-5,-5));
    if (!wait(editor)) return fail("two recorders blocked screenshot in their shared area");
    const auto innerBefore = innerFrames.count();
    outer->stop(); // 显式停止外层时，内层仍应持续采集，当前编辑器保持可操作。
                   // Explicitly finish the outer recorder while the inner recorder and editor remain live.
    if (!wait([&] { return outer->controlWindow()->findChild<LinuxRecordingResultPreview *>() && innerFrames.count() >= innerBefore + 2; })
        || outer->controlWindow()->isVisible() || !editor() || record.isVisible()) return fail("finishing MP4 interrupted GIF or editor");
    QTest::keyClick(editor(), Qt::Key_Escape);
    if (editor() == parent || !parent->isVisible()) return fail("parked parent must remain visible without taking input");
    stop.trigger();
    if (!wait([&] { return inner->controlWindow()->findChild<LinuxRecordingResultPreview *>() != nullptr; })
        || !record.isVisible() || stop.isVisible()) return fail("stopping last recorder did not restore idle tray");
    const QDir dir(files.path());
    const auto mp4 = dir.entryList({"*.mp4"}), gif = dir.entryList({"*.gif"});
    if (mp4.size() != 1 || gif.size() != 1) return fail("missing saved independent recordings");
    QFile movie(dir.filePath(mp4.first()));
    if (!movie.open(QIODevice::ReadOnly) || !movie.read(16).contains("ftyp") || QImage(dir.filePath(gif.first())).isNull())
        return fail("invalid MP4/GIF results");
    inner->cancel();
    if (!wait([&] { return !inner && editor() == parent; }) || !error.isEmpty()) return fail("child close did not restore parked editor");
    std::fprintf(stderr, "%s: %lld MP4 frames, %lld GIF frames; overlapping crops, latest-only toolbar, editor restore, tray and both files verified\n",
        virtualKWin ? "Virtual KWin ScreenShot2" : "Private ScreenShot2 fixture", static_cast<long long>(outerFrames.count()), static_cast<long long>(innerFrames.count()));
    return 0;
}
} // namespace capture_reentry_test
