#include "scroll_stitch.h"
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QPainter>
#include <QTransform>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // 使用已知随机页面验证拼接像素，覆盖多帧、重复、反向和无重叠。
    // A deterministic page validates exact output pixels and failed-frame preservation.
    QImage page(120, 1600, QImage::Format_RGB32);
    QRandomGenerator random(42);
    for (int y = 0; y < page.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(page.scanLine(y));
        for (int x = 0; x < page.width(); ++x) row[x] = qRgb(random.bounded(256), random.bounded(256), random.bounded(256));
    }
    LinuxScrollStitch stitch(page.copy(0, 0, 120, 200));
    using Result = LinuxScrollStitch::Result;
    if (stitch.append(page.copy(0, 80, 120, 200)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 1, __LINE__); return 1; }
    if (stitch.image() != page.copy(0, 0, 120, 280)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 2, __LINE__); return 2; }
    if (stitch.append(page.copy(0, 80, 120, 200)) != Result::Unchanged) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 3, __LINE__); return 3; }
    if (stitch.append(page.copy(0, 0, 120, 200)) != Result::Navigated || stitch.position() != 0) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 4, __LINE__); return 4; }
    if (stitch.append(page.copy(0, 400, 120, 200)) != Result::NoOverlap) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 5, __LINE__); return 5; }
    if (stitch.image() != page.copy(0, 0, 120, 280)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 6, __LINE__); return 6; }
    if (stitch.append(page.copy(0, 160, 120, 200)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 7, __LINE__); return 7; }
    if (stitch.image() != page.copy(0, 0, 120, 360)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 8, __LINE__); return 8; }
    if (stitch.append(QImage(20, 20, QImage::Format_RGB32)) != Result::Invalid) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 9, __LINE__); return 9; }
    QImage flat(120, 200, QImage::Format_RGB32); flat.fill(Qt::white);
    LinuxScrollStitch blank(flat);
    flat.setPixelColor(60, 40, Qt::black);
    if (blank.append(flat) != Result::NoOverlap) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 10, __LINE__); return 10; }
    const QImage fixture(QStringLiteral(JIETUBA_STITCH_PAGE));
    if (fixture.isNull()) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 11, __LINE__); return 11; }
    const QImage reference = fixture.convertToFormat(QImage::Format_RGB32);
    // 将真实文字页面转为横向文档，验证右滚、回看和再次右滚后的每个像素。
    // Rotate a text fixture into a horizontal document and verify forward/reverse/export pixels.
    const auto horizontalPage=reference.transformed(QTransform().rotate(-90));
    const auto horizontalFrame=[&](int left) {
        return horizontalPage.copy(left,0,300,360).transformed(QTransform().rotate(90));
    };
    LinuxScrollStitch horizontal(horizontalFrame(0), LinuxScrollStitch::Axis::Horizontal);
    int horizontalExtent=300;
    for(int left:{120,260,120,400,520}) {
        const auto result=horizontal.append(horizontalFrame(left));
        if(result!=Result::Added && result!=Result::Navigated) return 70;
        horizontalExtent=std::max(horizontalExtent,left+300);
        // Compare the frontier, including a historical viewport that must not replace the tail.
        if(horizontal.exportImage().transformed(QTransform().rotate(-90))!=horizontalPage.copy(0,0,horizontalExtent,360)) return 71;
    }
    if(horizontal.exportImage().transformed(QTransform().rotate(-90))!=horizontalPage.copy(0,0,820,360)) return 71;
    // 固定的文件树/行号栏在停止滚动时也只能在左边出现一次，右侧小地图同理。
    // Include both stationary gutters and check exact pixels when settling, revisiting and cropping the viewport.
    {
        const auto rotate = QTransform().rotate(90);
        const auto unrotate = QTransform().rotate(-90);
        const auto withGutters = [&](int left, int bodyWidth) {
            QImage image(94 + bodyWidth + 20, 120, QImage::Format_RGB32);
            QPainter painter(&image);
            painter.fillRect(image.rect(), QColor("#182430"));
            painter.drawImage(0, 0, horizontalPage.copy(16, 10, 92, 120));
            painter.drawImage(94, 0, horizontalPage.copy(left, 0, bodyWidth, 120));
            painter.drawImage(94 + bodyWidth, 0, horizontalPage.copy(35, 45, 20, 120));
            painter.end();
            return image;
        };
        const auto frame = [&](int left) { return withGutters(left, 240).transformed(rotate); };
        LinuxScrollStitch editor(frame(120), LinuxScrollStitch::Axis::Horizontal);
        int leftEdge = 120, rightEdge = 360;
        for (int left : {200, 340, 240, 120, 40, 160, 280, 420}) {
            const auto status = editor.append(frame(left));
            if (status != Result::Added && status != Result::Navigated) return 72;
            leftEdge = std::min(leftEdge, left);
            rightEdge = std::max(rightEdge, left + 240);
            const QImage expected = withGutters(leftEdge, rightEdge - leftEdge);
            if (editor.append(frame(left)) != Result::Unchanged || editor.image().transformed(unrotate) != expected
                || editor.exportImage().transformed(unrotate) != expected) return 73;
        }
        if (editor.append(frame(300)) != Result::Navigated) return 74;
        editor.crop(true);
        if (editor.exportImage().transformed(unrotate) != withGutters(300, rightEdge - 300)) return 75;
        editor.crop(false);
        if (editor.exportImage() != frame(300)) return 76;
    }
    LinuxScrollStitch pageStitch(reference.copy(0, 0, 360, 300));
    for (int top : {120, 260, 400, 520}) {
        if (pageStitch.append(reference.copy(0, top, 360, 300)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 12, __LINE__); return 12; }
        if (pageStitch.image() != reference.copy(0, 0, 360, top + 300)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 13, __LINE__); return 13; }
    }
    // 固定侧栏和顶部栏与正文不一起滚动，不能让它们否定正文中正确的位移。
    // Fixed sidebars and headers must not overwhelm the moving document match.
    const auto editorFrame = [&](int top) {
        QImage frame(720, 320, QImage::Format_RGB32);
        frame.fill(QColor("#20252c"));
        QPainter painter(&frame);
        painter.drawImage(360, 20, reference.copy(0, top, 360, 300));
        painter.drawImage(0, 20, reference.copy(0, 0, 360, 300)); // stationary sidebar
        painter.fillRect(0, 0, 720, 20, QColor("#445566"));
        return frame;
    };
    LinuxScrollStitch editor(editorFrame(0));
    if (editor.append(editorFrame(100)) != Result::Added || editor.image().height() != 420) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 14, __LINE__); return 14; }
    if (editor.append(editorFrame(200)) != Result::Added || editor.image().height() != 520) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 15, __LINE__); return 15; }
    LinuxScrollStitch fast(page.copy(0, 0, 120, 200));
    if (fast.append(page.copy(0, 170, 120, 200)) != Result::Added || fast.image() != page.copy(0, 0, 120, 370)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 16, __LINE__); return 16; }
    // 固定底部必须被替换而不是重复追加，包含底栏大于滚动正文的情况。
    // A fixed footer is replaced once, even when it is taller than the scrollable document area.
    for (int footerHeight : {80, 200}) {
        const int headerHeight = 12, bodyHeight = 160;
        QImage header(120, headerHeight, QImage::Format_RGB32); header.fill(QColor("#112233"));
        const QImage footer = page.copy(0, 400, 120, footerHeight);
        const auto framed = [&](int top) {
            QImage image(120, headerHeight + bodyHeight + footerHeight, QImage::Format_RGB32);
            QPainter painter(&image); painter.drawImage(0, 0, header);
            painter.drawImage(0, headerHeight, page.copy(0, top, 120, bodyHeight));
            painter.drawImage(0, headerHeight + bodyHeight, footer); return image;
        };
        LinuxScrollStitch fixed(framed(0));
        for (int top : {40, 80, 120}) {
            if (fixed.append(framed(top)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 17, __LINE__); return 17; }
            QImage expected(120, headerHeight + bodyHeight + top + footerHeight, QImage::Format_RGB32);
            QPainter painter(&expected); painter.drawImage(0, 0, header);
            painter.drawImage(0, headerHeight, page.copy(0, 0, 120, bodyHeight + top));
            painter.drawImage(0, headerHeight + bodyHeight + top, footer); painter.end();
            QImage currentExpected=expected;
            { QPainter painter(&currentExpected); painter.drawImage(0,top,framed(top)); }
            if (fixed.exportImage()!=currentExpected) {
                std::fprintf(stderr,"fixed footer current viewport was cropped or misplaced\n"); return 37;
            }
            if (fixed.image() != expected) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 18, __LINE__); return 18; }
        }
    }
    const auto whitespaceFooter = [&](int top) {
        QImage image(120,240,QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.drawImage(0,0,page.copy(0,top,120,160)); return image;
    };
    LinuxScrollStitch whitespace(whitespaceFooter(0));
    if (whitespace.append(whitespaceFooter(48)) != Result::Added || whitespace.exportImage() != whitespace.image()) {
        std::fprintf(stderr,"plain footer whitespace was cropped without UI evidence\n"); return 41;
    }
    // 固定在正文中间的悬浮页签及局部浮层，用后续未遮挡帧补回正文，不能反复写入。
    // Later clear observations repair stationary sticky tabs and local overlays at document coordinates.
    for (bool fullWidth : {true, false}) for (int offset : {0, 5}) {
        const auto floating = [&](int top) {
            QImage image = page.copy(0, top, 120, 200);
            const int left = fullWidth ? 0 : 24 + offset, right = fullWidth ? 120 : 48 + offset;
            for (int y = 120 + offset; y < 144 + offset; ++y) for (int x = left; x < right; ++x)
                image.setPixelColor(x, y, ((x / 3 + y / 3) % 2) ? QColor("#fa10b0") : QColor("#201040"));
            return image;
        };
        LinuxScrollStitch floatingStitch(floating(0));
        for (int top : {48, 96, 144}) {
            if (floatingStitch.append(floating(top)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 19, __LINE__); return 19; }
            if (floatingStitch.image() != page.copy(0, 0, 120, 200 + top)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 20, __LINE__); return 20; }
        }
    }
    // 浮层在新露出的底部：旧副本应被修补，只能保留尚未有清晰观测的末尾一份。
    // A footer-adjacent overlay is repaired in older views; only the still-occluded tail may retain it.
    const auto bottomOverlay = [&](int top) {
        QImage image = page.copy(0, top, 120, 200);
        for (int y = 156; y < 180; ++y) for (int x = 24; x < 48; ++x)
            image.setPixelColor(x, y, ((x / 3 + y / 3) % 2) ? QColor("#fa10b0") : QColor("#201040"));
        return image;
    };
    LinuxScrollStitch bottomFloating(bottomOverlay(0));
    for (int top : {48, 96, 144}) if (bottomFloating.append(bottomOverlay(top)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 21, __LINE__); return 21; }
    QImage expectedTail = page.copy(0, 0, 120, 344);
    const QImage lastView = bottomOverlay(144);
    { QPainter painter(&expectedTail); painter.drawImage(QRect(24, 300, 24, 24), lastView, QRect(24, 156, 24, 24)); }
    if (bottomFloating.image() != expectedTail) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 22, __LINE__); return 22; }
    // 停滚时按当前完整框选替换末页，不裁尾、不修改历史拼接缓存。
    // Idle output replaces the complete current viewport without trimming or mutating history.
    QImage currentTail=page.copy(0,0,120,344);
    { QPainter painter(&currentTail); painter.drawImage(0,144,lastView); }
    if(bottomFloating.exportImage()!=currentTail || bottomFloating.image()!=expectedTail) return 32;
    // 半透明导航条及局部浮层：验证每一个输出像素，而非只比较最终尺寸。
    // Alpha-composited bottom bars and local overlays must not leave repeated or fragmented text.
    for (int alpha : {64, 140, 216}) for (bool bottom : {false, true}) {
        const QRect bar = bottom ? QRect(0, 176, 120, 24) : QRect(24, 120, 48, 24);
        const auto translucent = [&](int top) {
            QImage image = page.copy(0, top, 120, 200);
            QPainter painter(&image); painter.fillRect(bar, QColor(30, 44, 60, alpha)); return image;
        };
        LinuxScrollStitch alphaStitch(translucent(0));
        for (int top : {48, 96, 144}) if (alphaStitch.append(translucent(top)) != Result::Added) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 23, __LINE__); return 23; }
        QImage expected = page.copy(0, 0, 120, 344);
        if (bottom) { QPainter painter(&expected); painter.drawImage(QRect(0, 320, 120, 24), translucent(144), bar); }
        QImage current=expected;
        { QPainter painter(&current); painter.drawImage(0,144,translucent(144)); }
        if(alphaStitch.exportImage()!=current) {
            std::fprintf(stderr,"current viewport must retain all pixels: alpha=%d bottom=%d\n",alpha,bottom); return 33;
        }
        // 无位移时浮层消失/改变，最新选区仍必须更新，且长图不能增高或缩短。
        // A fading overlay at zero displacement updates the page without changing image dimensions.
        const QImage clear=page.copy(0,144,120,200);
        if(alphaStitch.append(clear)!=Result::Updated || alphaStitch.position()!=144 || alphaStitch.exportImage()!=page.copy(0,0,120,344)) return 42;
        // 已刷新末页不能改变用于历史拼接的缓存。
        // Refreshing the final viewport must preserve historical compositing state.

        if (alphaStitch.image() != expected) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 24, __LINE__); return 24; }
    }
    // 真实文字页面同时有悬浮页签和半透明导航条，滚动距离不对齐字行或检测网格。
    // Text fixture: sticky tab plus translucent navigation, with irregular scroll distances.
    const QRect tab(95, 103, 130, 31), nav(0, 260, 360, 40);
    const auto decorated = [&](int top) {
        QImage image = reference.copy(0, top, 360, 300);
        QPainter painter(&image); painter.fillRect(tab, QColor("#252a32"));
        painter.drawImage(tab.adjusted(6,6,-6,-6), reference, QRect(45, 100, 118, 19));
        painter.fillRect(nav, QColor(30, 36, 44, 130)); return image;
    };
    LinuxScrollStitch textUi(decorated(0));
    for (int top : {37, 93, 151, 220, 280, 340}) {
        if (textUi.append(decorated(top)) != Result::Added) { std::fprintf(stderr, "decorated frame %d did not match\n", top); return 30; }
    }
    QImage decoratedExpected = reference.copy(0, 0, 360, 640);
    { QPainter painter(&decoratedExpected); painter.drawImage(QRect(0, 600, 360, 40), decorated(340), nav); }
    if (textUi.image() != decoratedExpected) { std::fprintf(stderr, "decorated output is not exact: %dx%d\n", textUi.image().width(), textUi.image().height());
        int badRows=0;
        for(int y=0; y<std::min(textUi.image().height(),decoratedExpected.height()); ++y) {
            int n=0; for(int x=0;x<360;++x) n += textUi.image().pixel(x,y) != decoratedExpected.pixel(x,y);
            if(n) { if(badRows++ < 35) std::fprintf(stderr,"row %d: %d different pixels\n",y,n); }
        }
        std::fprintf(stderr,"total bad rows: %d\n",badRows); return 31; }
    QImage currentText=decoratedExpected;
    { QPainter painter(&currentText); painter.drawImage(0,340,decorated(340)); }
    if(textUi.exportImage()!=currentText) { std::fprintf(stderr,"current decorated page misplaced\n"); return 34; }
    // 完全重复的停滚帧不会追加高度；导出保留当前完整页面。
    // Duplicate idle frames do not append rows, and the entire viewport is retained.
    if(textUi.append(decorated(340))!=Result::Unchanged || textUi.exportImage()!=currentText) return 38;
    // 最后几次小位移结束后，完整末页仍从已匹配的位置替换。
    // Small final scrolls still replace the full viewport at its matched offset.
    LinuxScrollStitch settling(decorated(0));
    for(int top:{37,93,151,156,158}) if(settling.append(decorated(top))!=Result::Added) return 39;
    QImage currentSettled=settling.image();
    { QPainter painter(&currentSettled); painter.drawImage(0,158,decorated(158)); }
    if(settling.exportImage()!=currentSettled || currentSettled.height()!=458) return 40;
    // 不匹配的全新画面不能被当成零位移，不能覆盖历史。
    // An unmatched page must never overwrite the previous viewport as an idle update.
    const QImage unchangedExport=settling.exportImage();
    QImage unrelated(reference.size(),QImage::Format_RGB32); unrelated.fill(Qt::red);
    if(settling.append(unrelated.copy(0,0,360,300))!=Result::NoOverlap || settling.exportImage()!=unchangedExport) return 43;
    // 无遮挡时末尾文字、空白和仅一帧的截图必须保持全部像素。
    // Preserve clean final content, whitespace and single-frame captures exactly.
    if (pageStitch.exportImage() != pageStitch.image()) return 35;
    LinuxScrollStitch oneFrame(decorated(0));
    if (oneFrame.exportImage() != decorated(0)) return 36;
    // 回看与停滚刷新不能把末页或当前固定栏贴到历史；末页快照必须始终留在原处。
    // Navigation and idle updates inside history must preserve both the assembled image and saved tail.
    const QImage savedTailExport=textUi.exportImage();
    if(textUi.append(decorated(280))!=Result::Navigated) return 46;
    const QImage history=textUi.image();
    const QImage currentHistory=reference.copy(0,280,360,300);
    if(textUi.append(currentHistory)!=Result::Updated || textUi.position()!=280) return 47;
    if(textUi.exportImage()!=savedTailExport || textUi.image()!=history || savedTailExport.height()!=640) return 48;
    // 返回历史视口后裁剪上下两端，位置必须同步更新，之后仍可准确追加。
    // Revisit history, crop both sides and continue stitching with rebased coordinates.
    LinuxScrollStitch cropped(page.copy(0, 100, 120, 200));
    for (int top : {180, 260, 180}) if (cropped.append(page.copy(0, top, 120, 200)) != (top==180 && cropped.image().height()==360 ? Result::Navigated : Result::Added)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 25, __LINE__); return 25; }
    cropped.crop(true);
    if (cropped.position() != 0 || cropped.image() != page.copy(0, 180, 120, 280)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 26, __LINE__); return 26; }
    cropped.crop(false);
    if (cropped.image() != page.copy(0, 180, 120, 200)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 27, __LINE__); return 27; }
    if (cropped.append(page.copy(0, 260, 120, 200)) != Result::Added || cropped.image() != page.copy(0, 180, 120, 280)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 28, __LINE__); return 28; }
    if (cropped.append(page.copy(0, 100, 120, 200)) != Result::Added || cropped.image() != page.copy(0, 100, 120, 360)) { std::fprintf(stderr, "stitch check %u failed at line %u\n", 29, __LINE__); return 29; }
    // 回看中间页时，只替换已匹配区域，不能错贴到长图末端，也不能截断后面的历史。
    // Revisiting history updates its matched viewport, preserving both earlier and later content.
    LinuxScrollStitch revisit(page.copy(0,0,120,200));
    for(int top:{80,160,80}) if(revisit.append(page.copy(0,top,120,200))!=(top==80 && revisit.image().height()==360 ? Result::Navigated : Result::Added)) return 44;
    if(revisit.position()!=80 || revisit.exportImage()!=page.copy(0,0,120,360)) return 45;
    // 快滚仍有 18 像素重叠：每一步都校验全部像素，不能只验证“长图变高”。
    // Fast scroll with 18px overlap must reproduce every pixel without omitted document rows.
    LinuxScrollStitch fastBurst(page.copy(0,0,120,200));
    for(int top:{182,364,546,728,910}) {
        if(fastBurst.append(page.copy(0,top,120,200))!=Result::Added || fastBurst.exportImage()!=page.copy(0,0,120,top+200)) {
            std::fprintf(stderr,"fast burst lost content at %d\n",top); return 49;
        }
    }
    LinuxScrollStitch fastText(reference.copy(0,0,360,300));
    for(int top:{270,540,810}) {
        if(fastText.append(reference.copy(0,top,360,300))!=Result::Added || fastText.exportImage()!=reference.copy(0,0,360,top+300)) {
            std::fprintf(stderr,"fast text lost content at %d\n",top); return 50;
        }
    }
    // 完全跳过的区域不可臆造或静默漏掉；失败保留长图，回退补采后必须还能继续准确增长。
    // A skipped non-overlapping interval must preserve output until overlapping frames bridge the gap.
    const QImage beforeGap=fastBurst.exportImage();
    if(fastBurst.append(page.copy(0,1300,120,200))!=Result::NoOverlap || fastBurst.exportImage()!=beforeGap) return 51;
    for(int top:{1092,1274}) if(fastBurst.append(page.copy(0,top,120,200))!=Result::Added) return 52;
    if(fastBurst.exportImage()!=page.copy(0,0,120,1474)) return 53;
    // 回看时即使导航条内容发生变化，也不能把当前 UI/最后一页盖进中间历史。
    // Reviewing altered fixed UI is read-only; resuming beyond the frontier appends real content again.
    const QImage frontier=textUi.exportImage(), historyPixels=textUi.image();
    if(textUi.append(decorated(220))!=Result::Navigated) return 54;
    if(textUi.append(reference.copy(0,220,360,300))!=Result::Updated) return 55;
    if(textUi.exportImage()!=frontier || textUi.image()!=historyPixels || textUi.position()!=220) return 56;
    if(textUi.append(decorated(340))!=Result::Navigated || textUi.append(decorated(400))!=Result::Added) return 57;
    if(textUi.image().height()!=700 || textUi.position()!=400 || textUi.exportImage().copy(0,400,360,300)!=decorated(400)) return 58;
    // 编辑器底部半透明面板同时有固定文字和图标，不能在各次滚动边界留下重复副本。
    // A translucent editor footer also contains stationary glyphs/icons, not just a flat alpha fill.
    const auto editorFooter=[&](int top) {
        QImage image=reference.copy(0,top,360,300);
        QPainter painter(&image);
        painter.fillRect(QRect(0,245,360,55),QColor(30,36,44,140));
        painter.drawImage(QPoint(10,260),reference.copy(45,100,118,19));
        painter.drawImage(QPoint(165,260),reference.copy(80,125,118,19));
        return image;
    };
    LinuxScrollStitch footerText(editorFooter(0));
    for (int top:{37,93,151,220,280,340}) if(footerText.append(editorFooter(top))!=Result::Added) return 59;
    QImage footerExpected=reference.copy(0,0,360,640);
    { QPainter painter(&footerExpected); painter.drawImage(0,340,editorFooter(340)); }
    if (footerText.exportImage()!=footerExpected) {
        std::fprintf(stderr,"stationary footer text left ghosts in scroll history\n"); return 60;
    }
    std::fprintf(stderr, "scroll stitch tests passed\n");
    return 0;
}
