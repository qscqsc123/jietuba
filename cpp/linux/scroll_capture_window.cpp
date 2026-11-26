#include "scroll_capture_window.h"
#include "scroll_capture_overlay.h"
#include "linux_settings.h"
#include "ocr_result_window.h"
#include "ppocr_wrapper.h"
#include "pin_window.h"
#include "capture_delivery.h"
#include "result_placement.h"
#include "core/save.h"
#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QTransform>
#include <QtConcurrentRun>
#include <utility>

LinuxScrollCaptureWindow::LinuxScrollCaptureWindow(QScreen *screen, const QSize &screenPixels, const QRect &pixelRegion,
                                                 const QImage &first, CaptureRequest request, LinuxSettings *settings,
                                                 const QRect &logicalRegion)
    : screen_(screen), output_(screen ? screen->name() : QString()), screenGeometry_(screen ? screen->geometry() : QRect()),
      screenPixels_(screenPixels), region_(pixelRegion), logicalRegion_(logicalRegion), stitch_(first), captureRequest_(std::move(request)), settings_(settings) {
    // QWidget 仅作为异步会话所有者和文件对话框的父对象，本身永不显示。
    // The hidden widget owns asynchronous work and export dialogs; it is never shown itself.
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("截图吧 — 长截图"));
    overlay_ = LinuxScrollCaptureOverlay::create(screen, screenPixels, pixelRegion,logicalRegion_);
    if (overlay_) {
        connect(overlay_, &LinuxScrollCaptureOverlay::finishRequested, this, &LinuxScrollCaptureWindow::stopAutomaticCapture);
        connect(overlay_, &LinuxScrollCaptureOverlay::cancelRequested, this, [this] { capturing_ = false; stopAutoScroll(); emit captureStateChanged(false); close(); });
        connect(overlay_, &LinuxScrollCaptureOverlay::directionRequested, this, &LinuxScrollCaptureWindow::toggleDirection);
        connect(overlay_, &LinuxScrollCaptureOverlay::cropRequested, this, &LinuxScrollCaptureWindow::cropCurrentView);
        connect(overlay_, &LinuxScrollCaptureOverlay::autoScrollRequested, this, &LinuxScrollCaptureWindow::toggleAutoScroll);
        connect(overlay_, &LinuxScrollCaptureOverlay::interactionActive, this, &LinuxScrollCaptureWindow::pauseInteraction);
        connect(overlay_, &LinuxScrollCaptureOverlay::saveRequested, this, [this] { finishWith(Action::Save); });
        connect(overlay_, &LinuxScrollCaptureOverlay::ocrRequested, this, [this] { finishWith(Action::Ocr); });
        connect(overlay_, &LinuxScrollCaptureOverlay::pinRequested, this, [this] { finishWith(Action::Pin); });
        if (!settings_) if (auto *pin = overlay_->findChild<QPushButton *>(QStringLiteral("scrollPin"))) pin->setEnabled(false);
        overlay_->setPreview(first, 1, region_.height());
    }
    router_.setLogSuccessfulCaptures(false);
    submitted_ = accepted_ = first.convertToFormat(QImage::Format_RGB32);
    // 只有可信的滚动位移才重置停滚计时；导航条动画不会无限推迟当前页刷新。
    // Only matched scroll movement restarts settling; UI animation must not postpone idle refresh forever.
    settleTimer_.setSingleShot(true);
    connect(&settleTimer_,&QTimer::timeout,this,&LinuxScrollCaptureWindow::settlePreview);
    autoTimer_.setSingleShot(true);
    connect(&autoTimer_, &QTimer::timeout, this, &LinuxScrollCaptureWindow::nextAutoStep);
    connect(&autoInput_, &LinuxScrollAutoInput::authorized, this, [this] {
        uiBlocked_ = false;
        if (overlay_) overlay_->show();
        setStatus(QStringLiteral("自动滚动中；再次点击自动滚动按钮停止。"));
        autoTimer_.start(350);
    });
    connect(&autoInput_, &LinuxScrollAutoInput::stepped, this, [this] {
        if (autoMode_ && capturing_) captureTimer_.start(220);
    });
    connect(&autoInput_, &LinuxScrollAutoInput::failed, this, [this](const QString &error) {
        uiBlocked_ = false; stopAutoScroll();
        if (overlay_) overlay_->show();
        setStatus(error);
    });
    captureTimer_.setSingleShot(true);
    connect(&captureTimer_, &QTimer::timeout, this, &LinuxScrollCaptureWindow::requestFrame);
    connect(&router_, &CaptureRouter::frameReady, this, [this](const QImage &image) {
        if (logicalRegion_.isEmpty()) acceptFrame(image);
        else acceptRegionFrame(image);
    });
    connect(&router_, &CaptureRouter::failed, this, [this](const QString &error) {
        pending_ = false;
        if (action_ != Action::None) { completeAction(); return; }
        if (!capturing_) return;
        if (autoMode_) stopAutoScroll();
        setStatus(QStringLiteral("采集暂时失败，正在重试：%1").arg(error));
        scheduleFrame(1000);
    });
    connect(&matcher_, &QFutureWatcher<LinuxScrollStitch::Result>::finished, this, &LinuxScrollCaptureWindow::matchFinished);
}
LinuxScrollCaptureWindow::~LinuxScrollCaptureWindow() {
    capturing_ = false;
    captureTimer_.stop(); autoTimer_.stop(); settleTimer_.stop(); autoInput_.stop();
    matcher_.waitForFinished();
    delete overlay_.data();
}
bool LinuxScrollCaptureWindow::hasOverlay() const { return !overlay_.isNull(); }
void LinuxScrollCaptureWindow::setStatus(const QString &text) {
    if (overlay_) overlay_->setStatus(text);
    if (text.contains(QStringLiteral("失败")) || text.contains(QStringLiteral("授权")) || text.contains(QStringLiteral("权限")))
        qInfo().noquote() << "长截图:" << text;
}
void LinuxScrollCaptureWindow::refreshPreview() {
    if (!overlay_) return;
    QImage image=settled_ ? stitch_.exportImage() : stitch_.image();
    overlay_->setPreview(image,frames_,horizontal_ ? region_.width() : region_.height(),horizontal_,stitch_.position());
}
void LinuxScrollCaptureWindow::startAutomaticCapture() {
    if (capturing_ || pending_ || matching_) return;
    if (!screen_ || screen_->geometry() != screenGeometry_ || screen_->name() != output_) {
        setStatus(QStringLiteral("显示器配置已改变，请重新开始长截图。")); return;
    }
    capturing_ = true;
    action_ = Action::None;
    if (overlay_) { overlay_->setControlsEnabled(true); overlay_->show(); }
    emit captureStateChanged(true);
    // 回到事件循环就开始采集。此前固定等 300ms，用户立即快滚时会丢掉最关键的首段重叠。
    // Begin on the next event-loop turn: an initial 300ms delay could miss the first overlap during fast scrolling.
    // 截图界面已由调用方隐藏；若首帧仍是原冻结画面，相同帧去重会忽略它并继续下一次采集。
    // The caller has hidden the frozen overlay; an identical stale first frame is deduplicated and retried.
    scheduleFrame(0);
    if(!settled_) settleTimer_.start(220);
}
void LinuxScrollCaptureWindow::stopAutomaticCapture() { finishWith(Action::Copy); }
void LinuxScrollCaptureWindow::finishWith(Action action) {
    if (action_ != Action::None) return;
    capturing_ = false;
    captureTimer_.stop(); settleTimer_.stop();
    stopAutoScroll();
    action_ = action;
    if (overlay_) overlay_->setControlsEnabled(false);
    if (overlay_) overlay_->hide();
    // 先处理已接收的中间帧再导出，避免点确认时丢掉队列尾部。
    // Drain accepted frames before exporting so completion includes the queued tail.
    completeAction();
}
void LinuxScrollCaptureWindow::completeAction() {
    if (pending_ || matching_ || !queue_.isEmpty() || action_ == Action::None) return;
    const Action action = std::exchange(action_, Action::None);
    // 控件操作也排空在途帧，避免 UI 线程在工作线程修改拼接器时改变方向/裁剪。
    // Control actions drain the same queue as exports, so no GUI/worker state race occurs.
    if (action == Action::Direction || action == Action::CropBefore || action == Action::CropAfter || action == Action::AutoStart) {
        if (action == Action::Direction) {
            horizontal_ = !horizontal_;
            stitch_ = LinuxScrollStitch(orient(accepted_), horizontal_ ? LinuxScrollStitch::Axis::Horizontal : LinuxScrollStitch::Axis::Vertical);
            frames_ = 1; submitted_ = accepted_; viewMatched_ = true;
            if (overlay_) overlay_->setDirection(horizontal_);
        } else if (action == Action::CropBefore || action == Action::CropAfter) {
            if (viewMatched_) stitch_.crop(action == Action::CropBefore);
            else setStatus(QStringLiteral("当前画面未匹配，未裁剪；请回退到预览绿框对应位置。"));
        }
        capturing_ = true; settled_=true; refreshPreview();
        if (overlay_) { overlay_->setControlsEnabled(true); overlay_->show(); }
        if (action == Action::AutoStart) {
            autoMode_ = true; autoIdleFrames_ = 0; uiBlocked_ = true;
            if (overlay_) { overlay_->setAutoScrolling(true); overlay_->hide(); }
            setStatus(QStringLiteral("请在桌面授权窗口中选择当前长截图所在显示器。"));
            const QRect logical = LinuxScrollCaptureOverlay::selectionInLogicalPixels(screenGeometry_.size(), screenPixels_, region_);
            autoInput_.start(screenGeometry_, QPointF(screenGeometry_.topLeft()) + QRectF(logical).center());
        } else scheduleFrame(0);
        return;
    }
    emit captureStateChanged(false);
    switch (action) {
    case Action::Copy: copyResult(); break;
    case Action::Save: saveResult(); break;
    case Action::Ocr: {
        const bool direct=settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::CopyOcr);
        
        // 根据用户设置选择 OCR 引擎
        std::function<LinuxOcrResult(const QImage &)> recognizer;
        QString engine = settings_ ? settings_->ocrEngine() : "auto";
        
        if (engine == "ppocr" || (engine == "auto" && LinuxPpocrEngine::available())) {
            if (LinuxPpocrEngine::available()) {
                recognizer = [](const QImage &img) { return LinuxPpocrEngine::recognize(img); };
            } else if (engine == "ppocr") {
                // 先关闭窗口，避免遮挡错误提示
                QImage img = resultImage();
                close();
                QTimer::singleShot(100, [img]() {
                    QMessageBox::warning(nullptr, QStringLiteral("PP-OCR 不可用"), 
                        QStringLiteral("PP-OCR 未安装或不可用。\n\n请安装 onnxruntime 和模型文件，或在设置中选择其他引擎。"));
                });
                return;
            }
        }
        // engine == "tesseract" 或 recognizer 为空时使用 Tesseract（默认）
        
        auto *window = new LinuxOcrResultWindow(resultImage(),nullptr,recognizer,direct);
        placeCaptureResult(window,screen_,logicalRegion_,settings_ ? settings_->ocrPosition() : "center");
        if (!direct) window->show(); close(); break;
    }
    case Action::Pin:
        if (settings_) {
            const auto image=resultImage();
            if (settings_->captureFlag(LinuxSettings::CaptureFlag::SavePin) && LinuxCaptureDelivery::save(*settings_,image,"长贴图_").isEmpty()) QMessageBox::warning(this,"自动保存失败","无法写入保存目录。");
            auto *pin = new LinuxPinWindow(image, *settings_);
            pin->showOnScreen(screen_, {});
            close();
        } else startAutomaticCapture();
        break;
    default: break;
    }
}
void LinuxScrollCaptureWindow::scheduleFrame(int delay) {
    if (capturing_ && !autoMode_ && !uiBlocked_ && !captureTimer_.isActive()) captureTimer_.start(delay);
}
void LinuxScrollCaptureWindow::requestFrame() {
    if (!capturing_ || pending_ || uiBlocked_) return;
    // 保持顺序且限制内存：队列满时施加背压，不丢掉尚未匹配的中间帧。
    // Apply bounded backpressure instead of replacing queued frames with the newest one.
    constexpr qint64 maxQueuedBytes = 96LL * 1024 * 1024;
    const qint64 frameBytes = qint64(region_.width()) * region_.height() * 4;
    if (queue_.size() >= 10 || (!queue_.isEmpty() && queuedBytes_ + frameBytes > maxQueuedBytes)) {
        scheduleFrame(10); return;
    }
    if (!screen_ || screen_->geometry() != screenGeometry_ || screen_->name() != output_) {
        setStatus(QStringLiteral("显示器配置已改变，请完成后重新框选。")); return;
    }
    pending_ = true;
    if (captureRequest_) captureRequest_();
    else if (!logicalRegion_.isEmpty())
        router_.captureRegion(output_, logicalRegion_.translated(screenGeometry_.topLeft()), screenGeometry_, CallerWindowPolicy::Hide);
    else router_.capture(output_);
}
void LinuxScrollCaptureWindow::acceptFrame(const QImage &image) {
    acceptCapturedFrame(image, false);
}
void LinuxScrollCaptureWindow::acceptRegionFrame(const QImage &image) {
    acceptCapturedFrame(image, true);
}
void LinuxScrollCaptureWindow::acceptCapturedFrame(const QImage &image, bool regionFrame) {
    pending_ = false;
    if (std::exchange(discardPending_, false) || uiBlocked_) {
        // 菜单/授权窗口出现前已发出的截图也必须丢弃，否则仍可能把弹窗拼入长图。
        // Discard captures issued just before a popup, not just captures scheduled while it is open.
        scheduleFrame(250); completeAction(); return;
    }
    // 两种回复不能共用整屏尺寸检查：区域接口返回的就是选区，不能再按屏幕坐标裁一次。
    // Area replies already contain the crop: do not compare them to an output or crop them a second time.
    // 区域首帧直接保留原始尺寸；后续必须完全相同，禁止用拉伸“修正”尺寸造成字形拖影。
    // Area frames must exactly match the native first frame; resizing to hide mismatches creates text ghosts.
    const bool validSize = regionFrame
        ? !image.isNull() && image.size()==region_.size()
        : image.size()==screenPixels_ && QRect(QPoint(),image.size()).contains(region_);
    if ((!capturing_ && action_ == Action::None) || !screen_ || screen_->geometry() != screenGeometry_ || !validSize) {
        setStatus(QStringLiteral("屏幕尺寸发生变化，请完成后重新框选。"));
        completeAction(); return;
    }
    QImage frame = (regionFrame ? image : image.copy(region_)).convertToFormat(QImage::Format_RGB32);
    frame.setDevicePixelRatio(1); // 拼接全部按原生像素计算 / Stitch exclusively in native pixels.
    if (frame != submitted_) {
        submitted_ = frame;
        queuedBytes_ += frame.sizeInBytes();
        queue_.enqueue(frame);
    } else if (autoMode_) autoFrameUnchanged();
    // 采集不再等待拼接完成；只有截图后端本身最多一个在途请求。
    // Capture independently of matching, with at most one backend request in flight.
    // 后端完成后立即排下次采集；固定等待会让快速滚动白白损失可重叠的中间帧。
    // Re-arm immediately after backend completion instead of losing overlap to a fixed delay.
    scheduleFrame(0);
    startNextMatch();
    completeAction();
}
void LinuxScrollCaptureWindow::startNextMatch() {
    if (matching_ || queue_.isEmpty()) return;
    const QImage frame = queue_.dequeue();
    queuedBytes_ -= frame.sizeInBytes();
    matching_ = true; matchingFrame_ = frame;
    matcher_.setFuture(QtConcurrent::run([this, frame] { return stitch_.append(orient(frame)); }));
}
void LinuxScrollCaptureWindow::matchFinished() {
    matching_ = false;
    const auto result = matcher_.result();
    viewMatched_ = result == LinuxScrollStitch::Result::Added || result == LinuxScrollStitch::Result::Navigated || result == LinuxScrollStitch::Result::Updated || result == LinuxScrollStitch::Result::Unchanged;
    if (viewMatched_) accepted_ = matchingFrame_;
    if (result == LinuxScrollStitch::Result::Added) {
        ++frames_; autoIdleFrames_ = 0;
        settled_=false; settleTimer_.start(220);
        refreshPreview();
        setStatus(QStringLiteral("已拼接 %1 帧").arg(frames_));
    } else if(result==LinuxScrollStitch::Result::Navigated) {
        settled_=false; settleTimer_.start(220); refreshPreview();
        setStatus(QStringLiteral("已拼接 %1 帧（回看历史，内容保持不变）").arg(frames_));
    } else if (result == LinuxScrollStitch::Result::Updated) {
        if(settled_) refreshPreview();
        else if(!settleTimer_.isActive()) settleTimer_.start(220);
        if(autoMode_) autoFrameUnchanged();
    } else if (result == LinuxScrollStitch::Result::Unchanged) {
        if(!settled_ && !settleTimer_.isActive()) settleTimer_.start(220);
        if (autoMode_) autoFrameUnchanged();
    } else if (result == LinuxScrollStitch::Result::NoOverlap) {
        if (autoMode_) stopAutoScroll();
        setStatus(horizontal_ ? QStringLiteral("滚动过快或画面未匹配，未追加；请向左回退至已采集区域补采。") : QStringLiteral("滚动过快或画面未匹配，未追加；请向上回退至已采集区域补采。"));
    } else if (result == LinuxScrollStitch::Result::TooLarge || result == LinuxScrollStitch::Result::Invalid) {
        captureTimer_.stop();
        queue_.clear(); queuedBytes_ = 0;
        setStatus(QStringLiteral("已达到大小限制或画面尺寸无效，请完成或保存。"));
        capturing_ = false; stopAutoScroll();
    }
    emit frameProcessed(frames_,viewMatched_);
    startNextMatch();
    if (autoMode_ && !autoTimer_.isActive()) autoTimer_.start(160);
    completeAction();
}
void LinuxScrollCaptureWindow::copyResult() {
    const auto image = resultImage();
    QString path;
    if (settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::AutoSave)) {
        path=LinuxCaptureDelivery::save(*settings_,image,"长图_");
        if (path.isEmpty()) QMessageBox::warning(this,"长图自动保存失败","无法写入保存目录。");
    }
    if (!LinuxCaptureDelivery::copy(image,settings_ && settings_->captureFlag(LinuxSettings::CaptureFlag::FileUrl) ? path : QString())) {
        QMessageBox::warning(this,"复制失败","无法生成 PNG 图片。"); startAutomaticCapture(); return;
    }
    close();
}
void LinuxScrollCaptureWindow::saveResult() {
    QFileDialog dialog(this, QStringLiteral("保存长截图"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilters({QStringLiteral("PNG 图片 (*.png)"), QStringLiteral("JPEG 图片 (*.jpg *.jpeg)"), QStringLiteral("PDF 文档 (*.pdf)")});
    dialog.setDefaultSuffix(QStringLiteral("png")); dialog.selectFile(QStringLiteral("长截图.png"));
    connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog](const QString &filter) {
        dialog.setDefaultSuffix(filter.startsWith("PDF") ? "pdf" : filter.startsWith("JPEG") ? "jpg" : "png");
    });
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) { startAutomaticCapture(); return; }
    if (!jietuba::SaveService::save_qimage_to_path(resultImage(), dialog.selectedFiles().first())) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法写入图片，请检查文件名和目录权限。"));
        startAutomaticCapture(); return;
    }
    close();
}

QImage LinuxScrollCaptureWindow::orient(const QImage &image) const {
    // 横向复用同一拼接器：顺时针旋转后，向右滚动变为纵向向下；导出时精确逆变换。
    // A lossless 90-degree transform reuses vertical matching for horizontal scrolling.
    return horizontal_ ? image.transformed(QTransform().rotate(90)) : image;
}
QImage LinuxScrollCaptureWindow::resultImage() const {
    // 完成操作排空已接收帧后导出最远末页；回看位置不会覆盖已采集内容。
    // Export the furthest saved tail after draining accepted frames; reviewing history never rewrites it.
    const QImage current=stitch_.exportImage();
    return horizontal_ ? current.transformed(QTransform().rotate(-90)) : current;
}
void LinuxScrollCaptureWindow::toggleDirection() { finishWith(Action::Direction); }
void LinuxScrollCaptureWindow::cropCurrentView(const QString &side) {
    if (side != QStringLiteral("top") && side != QStringLiteral("bottom")) return;
    finishWith(side == QStringLiteral("top") ? Action::CropBefore : Action::CropAfter);
}
void LinuxScrollCaptureWindow::pauseInteraction(bool active) {
    uiBlocked_ = active;
    if (active) {
        captureTimer_.stop(); discardPending_ |= pending_; stopAutoScroll();
    } else { scheduleFrame(0); if(!settled_) settleTimer_.start(220); }
}
void LinuxScrollCaptureWindow::toggleAutoScroll() {
    if (autoMode_) { stopAutoScroll(); return; }
    finishWith(Action::AutoStart);
}
void LinuxScrollCaptureWindow::stopAutoScroll() {
    autoMode_ = false; autoTimer_.stop(); autoInput_.stop();
    if (overlay_) overlay_->setAutoScrolling(false);
    scheduleFrame(250);
}
void LinuxScrollCaptureWindow::nextAutoStep() {
    if (!capturing_ || !autoMode_ || !autoInput_.ready() || uiBlocked_) return;
    // 每次滚轮后先等页面稳定、采集及匹配完成，绝不在队列积压时继续滚动。
    // Wait for the capture/match pipeline before injecting the next wheel notch.
    if (pending_ || matching_ || !queue_.isEmpty()) { autoTimer_.start(100); return; }
    autoInput_.step(horizontal_);
}
void LinuxScrollCaptureWindow::autoFrameUnchanged() {
    if (++autoIdleFrames_ >= 4) {
        stopAutoScroll(); setStatus(QStringLiteral("连续画面未变化，自动滚动已停止；可手动调整或完成。"));
    } else autoTimer_.start(300);
}

void LinuxScrollCaptureWindow::settlePreview() {
    if(!capturing_ || uiBlocked_ || action_!=Action::None) return;
    // 工作线程或队列忙碌时等待，不能在拼接器写入期间读图。
    // Wait for the serial matching queue before reading the stitcher from the UI thread.
    if(matching_ || !queue_.isEmpty()) { settleTimer_.start(30); return; }
    if(!viewMatched_) return;
    settled_=true; refreshPreview();
}
