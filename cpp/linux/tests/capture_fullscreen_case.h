#pragma once
#include <QTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <cstdio>

/**
 * 在私有 KWin 中验证真实整屏采集、分数缩放和多屏独立的像素坐标。
 * 由 mosaic_ui_test 引入，使用生产 CaptureSession；不连接用户桌面或写入用户配置。
 * Exercises the production capture-first path on an isolated compositor, including
 * mixed output scales and the pointer-selected output, without touching the real desktop.
 */
inline int runFullscreenCaptureCase(QApplication &app) {
    const auto fail=[](const char *text){std::fprintf(stderr,"Fullscreen capture: %s\n",text);return 1;};
    if(app.platformName()!="wayland" || qgetenv("WAYLAND_DISPLAY")!="wayland-jietuba-test") return fail("private compositor required");
    app.setQuitOnLastWindowClosed(false); app.setDesktopFileName("jietuba-capture-reentry-test");
    jietuba::get_ui_theme()->init(nullptr,&app);
    QTemporaryDir files; LinuxSettings settings(files.filePath("capture.ini"));
    settings.setSaveDirectory(files.path()); settings.setCaptureFlag(LinuxSettings::CaptureFlag::AutoSave,false);
    const auto outputs=app.screens();
    std::vector<std::unique_ptr<QWidget>> desktops;
    QHash<QString,QSize> nativeSizes;
    KWinCapture backend(CallerWindowPolicy::Include); std::atomic_bool cancelled{false};
    for(auto *output:outputs) {
        auto desktop=std::make_unique<QWidget>();desktop->setScreen(output);
        desktop->setStyleSheet("background:#27648a");desktop->showFullScreen();
        if(!QTest::qWaitForWindowExposed(desktop.get(),2000)) return fail("desktop not exposed");
        const auto frame=backend.capture(output->name(),cancelled);
        if(frame.image.isNull()) return fail("native fullscreen request failed");
        nativeSizes.insert(output->name(),frame.image.size());
        desktops.push_back(std::move(desktop));
    }
    QString dialogError; QTimer dialogs;
    QObject::connect(&dialogs,&QTimer::timeout,&app,[&]{
        if(auto *box=qobject_cast<QMessageBox *>(app.activeModalWidget())) {dialogError=box->text();box->accept();}
    }); dialogs.start(20);
    const auto verify=[&](bool all,QScreen *single){
        settings.setCaptureAllScreens(all);
        CaptureSession session(desktops.front().get(),settings,false);
        // 同一个后台会话连续执行取消、复制、保存和再次取消，不能每次销毁会话掩盖 busy 残留。
        // Reuse one session across cancel/copy/save; reconstructing it would hide stale acquisition state.
        for(int attempt=0;attempt<4;++attempt) {
        session.start();
        QList<CapturePreview *> editors;
        const int count=all ? outputs.size() : 1;
        const bool ready=QTest::qWaitFor([&]{
            editors.clear();
            for(auto *w:app.topLevelWidgets()) if(auto *e=dynamic_cast<CapturePreview *>(w); e && e->isVisible()) editors.append(e);
            return editors.size()==count || !dialogError.isEmpty();
        },6000);
        if(!ready || !dialogError.isEmpty() || editors.size()!=count) return false;
        for(auto *editor:editors) {
            auto *screen=editor->screen(); if(!all && screen!=single) return false;
            if(!QTest::qWaitForWindowExposed(editor,2000)) return false;
            auto *view=dynamic_cast<PreviewView *>(editor->findChild<QGraphicsView *>());
            if(!view || !QTest::qWaitFor([&]{return view->viewport()->size()==screen->size();},2000)) return false;
            if(!view->canvas || view->canvas->background->boundingRect().size()!=QSizeF(nativeSizes.value(screen->name()))) return false;
            const QRect logical(60,60,160,100);
            QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,logical.topLeft());
            QTest::mouseMove(view->viewport(),logical.topLeft()+QPoint(logical.width(),logical.height()));
            QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,logical.topLeft()+QPoint(logical.width(),logical.height()));
            const double sx=double(nativeSizes.value(screen->name()).width())/screen->size().width();
            const double sy=double(nativeSizes.value(screen->name()).height())/screen->size().height();
            const auto crop=static_cast<LinuxAnnotationCanvas *>(editor)->result();
            if(crop.isNull() || std::abs(crop.width()-logical.width()*sx)>2 || std::abs(crop.height()-logical.height()*sy)>2) return false;
            std::fprintf(stderr,"Fullscreen output %s: logical %dx%d, native %dx%d, crop %dx%d\n",qPrintable(screen->name()),screen->size().width(),screen->size().height(),nativeSizes.value(screen->name()).width(),nativeSizes.value(screen->name()).height(),crop.width(),crop.height());
        }
        auto *editor=editors.first(); auto *view=editor->findChild<QGraphicsView *>();
        auto *toolbar=editor->findChild<jietuba::Toolbar *>();
        if(attempt==1) {
            app.clipboard()->clear(); toolbar->confirm_btn->click();
            if(!app.clipboard()->mimeData()->hasFormat("image/png")) return false;
        } else if(attempt==2) {
            const QString path=files.filePath(QStringLiteral("repeat-%1.png").arg(all));
            QTimer::singleShot(0,&app,[path]{
                if(auto *dialog=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    dialog->selectFile(path); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);
                }
            });
            toolbar->save_btn->click();
            if(QImage(path).isNull()) return false;
        } else QTest::keyClick(view,Qt::Key_Escape);
        if(!QTest::qWaitFor([&]{
            for(auto *w:app.topLevelWidgets()) if(auto *e=dynamic_cast<CapturePreview *>(w);e && e->isVisible()) return false;
            return true;
        },2000)) return false;
        std::fprintf(stderr,"Fullscreen repeat: %s cycle %d completed\n",all?"all outputs":"single output",attempt+1);
        }
        return true;
    };
    if(!verify(true,nullptr)) {std::fprintf(stderr,"%s\n",qPrintable(dialogError));return fail("mixed-scale frozen capture/crop failed");}
    // 通过私有输入设备把光标放到最后一个输出，验证未开启多屏时不会固定截主屏。
    // Move the compositor pointer to another output, then exercise single-output capture.
    auto *target=outputs.last();
    const QRect g=target->geometry(); const double scale=target->devicePixelRatio();
    const QPoint point=g.topLeft()+QPoint(qRound(g.width()*scale/2),qRound(g.height()*scale/2));
    QProcess pointer; pointer.start(qEnvironmentVariable("JIETUBA_TEST_CLICK"),{QString::number(point.x()),QString::number(point.y())});
    if(!QTest::qWaitFor([&]{return pointer.state()==QProcess::NotRunning;},4000) || pointer.exitCode()!=0) return fail("test pointer injection failed");
    const auto active=QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall("org.kde.KWin","/KWin","org.kde.KWin","activeOutputName"),QDBus::Block,500);
    if(active.arguments().value(0).toString()!=target->name()) {
        std::fprintf(stderr,"Pointer output: wanted %s at %d,%d %dx%d, injected %d,%d, compositor %s\n",qPrintable(target->name()),g.x(),g.y(),g.width(),g.height(),point.x(),point.y(),qPrintable(active.arguments().value(0).toString()));
        return fail("pointer did not activate target output");
    }
    if(!verify(false,target)) return fail("single capture did not use pointer output");
    std::fprintf(stderr,"Fullscreen capture: native pixels, mixed scaling, pointer output and repeated cancel/copy/save verified\n");
    return 0;
}
