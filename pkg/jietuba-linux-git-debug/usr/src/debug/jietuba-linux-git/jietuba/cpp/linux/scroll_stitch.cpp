#include "scroll_stitch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <QTransform>

namespace {
int difference(QRgb a, QRgb b) {
    return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}
}
LinuxScrollStitch::LinuxScrollStitch(const QImage &first, Axis axis)
    : previous_(first.convertToFormat(QImage::Format_RGB32)), result_(previous_), tailFrame_(previous_),
      sourceRows_(first.height()), excluded_(first.height(), false), confirmedObscured_(first.height(), false), axis_(axis) {
    for (int y = 0; y < first.height(); ++y) sourceRows_[y] = y;
}

LinuxScrollStitch::Result LinuxScrollStitch::append(const QImage &input) { return appendImpl(input, 0); }

LinuxScrollStitch::Result LinuxScrollStitch::appendImpl(const QImage &input, int forcedShift) {
    if (previous_.isNull() || input.size() != previous_.size() || input.width() < 32 || input.height() < 48)
        return Result::Invalid;
    const QImage frame = input.convertToFormat(QImage::Format_RGB32);
    if (frame.isNull()) return Result::TooLarge;
    if (frame == previous_) return Result::Unchanged;
    // 没有滚动也可能发生底栏淡出、光标闪烁等变化，不能直接丢掉这类末页更新。
    // A stationary page can still change as overlays fade; preserve that latest full viewport.
    if (!forcedShift && refreshStationary(frame)) return Result::Updated;
    const int width = frame.width(), height = frame.height();
    const auto oldRow = [&](int y) { return reinterpret_cast<const QRgb *>(previous_.constScanLine(y)); };
    const auto newRow = [&](int y) { return reinterpret_cast<const QRgb *>(frame.constScanLine(y)); };
    // 固定侧栏/行号不能参与滚动匹配：只在本次画面变化足够的列中查找竖直位移。
    // Match only changing columns so fixed sidebars and line-number gutters cannot dominate.
    std::vector<int> columns;
    const int xStep = std::max(1, width / 100), probeStep = std::max(1, height / 100);
    for (int x = width / 30; x < width - width / 30; x += xStep) {
        int changed = 0, count = 0;
        for (int y = 0; y < height; y += probeStep) {
            changed += difference(oldRow(y)[x], newRow(y)[x]) > 36;
            ++count;
        }
        if (changed >= std::max(3, count / 8)) columns.push_back(x);
    }
    if (columns.size() < 3) return Result::NoOverlap;
    // 与原版 fixed_bars 思路一致：同位置的连续固定边界单独保留，不进入滚动匹配。
    // Like the original fixed_bars, separate unchanged edge bands from scrollable content.
    std::vector<bool> stable(height, false), stationary(height, false);
    for (int y = 0; y < height; ++y) {
        int changed = 0;
        for (int x : columns) changed += difference(oldRow(y)[x], newRow(y)[x]) > 12;
        stable[y] = changed <= int(columns.size()) / 40;
    }
    int movingTop = 0, movingBottom = height;
    while (movingTop < height && stable[movingTop]) ++movingTop;
    while (movingBottom > movingTop && stable[movingBottom - 1]) --movingBottom;
    if (movingBottom - movingTop < 32) return Result::NoOverlap;
    // 内部不动且有纹理的行带通常是悬浮页签；纯色空白不据此判为浮层。
    // Textured stationary interior bands are sticky tabs; plain whitespace alone is insufficient.
    for (int begin = movingTop; begin < movingBottom;) {
        if (!stable[begin]) { ++begin; continue; }
        int end = begin;
        int texture = 0;
        while (end < movingBottom && stable[end]) {
            for (int i = 1; i < int(columns.size()); ++i)
                texture += difference(newRow(end)[columns[i - 1]], newRow(end)[columns[i]]) > 36;
            ++end;
        }
        bool exact = true;
        for(int yy=begin; yy<end && exact; ++yy) for(int x:columns) if(difference(oldRow(yy)[x],newRow(yy)[x]) > 1) { exact=false; break; }
        if (exact && end - begin >= 3 && texture >= std::max(4, (end - begin) * int(columns.size()) / 80))
            std::fill(stationary.begin() + begin, stationary.begin() + end, true);
        begin = end;
    }
    // 快滚时仍可能保留较短的有效重叠。降低最小重叠，但仍要求纹理及候选唯一性。
    // Fast scrolling may leave a short overlap; accept it only with textured, unambiguous evidence.
    const int minOverlap=std::max(16,std::min(64,(movingBottom-movingTop)/30));
    const auto scoreShift = [&](int shift, bool fine) {
        const int begin = std::max(movingTop, movingTop - shift);
        const int end = std::min(movingBottom, movingBottom - shift);
        const int overlap = end - begin;
        const int yStep = std::max(1, overlap / (fine ? 80 : 22));
        const int columnStep = fine ? 1 : std::max(1, int(columns.size()) / 20);
        std::vector<double> rowErrors;
        int texture = 0;
        for (int y = begin; y < end; y += yStep) {
            if (stationary[y] || stationary[y + shift]) continue;
            const auto *a = oldRow(y + shift), *b = newRow(y);
            std::vector<int> errors;
            int rowTexture = 0;
            for (int i = 0; i < int(columns.size()); i += columnStep) {
                const int x = columns[i];
                errors.push_back(difference(a[x], b[x]));
                rowTexture += x > 1 && (difference(a[x],a[x-2]) > 24 || difference(b[x],b[x-2]) > 24);
                if (y > 1 && difference(b[x], newRow(y - 2)[x]) > 36) ++texture;
            }
            // 局部浮层或动画可能只挡住部分列，降低每行异常样本的权重，但仍保留区分相似文字行的证据。
            // Downweight outliers without discarding evidence that distinguishes repeated text lines.
            if (fine && !rowTexture) continue;
            const int keep = std::max(1, int(errors.size()) * 4 / 5);
            std::nth_element(errors.begin(), errors.begin() + keep - 1, errors.end());
            double total = 0;
            for (int i = 0; i < int(errors.size()); ++i) total += errors[i] * (i < keep ? 1.0 : 0.35);
            rowErrors.push_back(total / (3 * errors.size()));
        }
        const int required = fine ? 6 : 2;
        if (int(rowErrors.size()) < required || (fine && texture < 8)) return 255.0;
        // 按整行做稳健估计：同一浮层会同时污染对齐后的两个位置，单纯按列降权不够。
        // Score coherent inlier rows. A sticky/translucent band corrupts two aligned screen positions.
        // 仍需至少六个有纹理的支持行；纯背景或周期图案由后续唯一性检查拒绝。
        // Require textured support and a unique offset; blank/repetitive content is never guessed.
        std::sort(rowErrors.begin(), rowErrors.end());
        const int keepRows = std::max(required, int(rowErrors.size()) * 3 / 5);
        double total = 0, all = 0;
        for (int i=0; i<int(rowErrors.size()); ++i) {
            all += rowErrors[i]; if (i < keepRows) total += rowErrors[i];
        }
        // 保留少量尾部残差，防止只用字框内的重复竖线就接受错开几个像素的候选。
        // Retain a small outlier penalty to distinguish repeated glyph stems at nearby offsets.
        return total / keepRows + 0.15 * all / rowErrors.size();
    };
    // 先用有纹理行的像素指纹为位移投票。清晰重叠帧不必每次枚举所有位移，
    // 也不会被大面积空白或重复的代码行优先带到错误候选。
    // Textured-row fingerprints vote for exact translations before the more expensive tolerant search.
    const auto fingerprint=[&](const QRgb *row) -> std::uint64_t {
        std::uint64_t hash=1469598103934665603ULL;
        int texture=0;
        for(int x:columns) {
            texture+=x>0 && difference(row[x],row[x-1])>24;
            hash^=std::uint64_t(row[x]&0x00ffffff); hash*=1099511628211ULL;
        }
        return texture ? hash : 0;
    };
    std::unordered_map<std::uint64_t,std::vector<int>> oldFingerprints;
    for(int y=movingTop; y<movingBottom; ++y) if(!stationary[y]) {
        if(const auto hash=fingerprint(oldRow(y))) oldFingerprints[hash].push_back(y);
    }
    std::vector<int> votes(2*height+1,0);
    for(int y=movingTop; y<movingBottom; ++y) if(!stationary[y]) {
        const auto hash=fingerprint(newRow(y));
        const auto found=oldFingerprints.find(hash);
        // 过度重复的行不能作为可靠锚点，也避免纯重复纹理产生平方级比较。
        // Highly repetitive rows are not reliable anchors and must not create quadratic work.
        if(!hash || found==oldFingerprints.end() || found->second.size()>128) continue;
        for(int oldY:found->second) {
            const int shift=oldY-y;
            if(shift && std::abs(shift)<=height-minOverlap && (!forcedShift || shift==forcedShift)) ++votes[shift+height];
        }
    }
    std::vector<std::pair<int,int>> anchors;
    for(int shift=-height+minOverlap; shift<=height-minOverlap; ++shift)
        if(shift && votes[shift+height]>=4) anchors.emplace_back(votes[shift+height],shift);
    std::sort(anchors.begin(),anchors.end(),std::greater<>());
    int competing=0;
    if(!anchors.empty()) for(const auto &[count,shift]:anchors)
        if(std::abs(shift-anchors.front().second)>2) { competing=count; break; }
    const bool consensus=!anchors.empty() && anchors.front().first>=6 && anchors.front().first*3>competing*4;
    std::vector<std::pair<double,int>> verified;
    const auto verify=[&](int shift) {
        if(std::none_of(verified.begin(),verified.end(),[shift](const auto &v){return v.second==shift;}))
            verified.emplace_back(scoreShift(shift,true),shift);
    };
    for(int i=0; i<std::min(24,int(anchors.size())); ++i) verify(anchors[i].second);
    // 没有足够精确锚点时保留抗动画/半透明浮层的容错路径，不把快滚强行当作小位移。
    // Retain tolerant matching for animated/occluded frames when exact anchors are insufficient.
    if(!consensus || verified.empty() || std::min_element(verified.begin(),verified.end())->first>10) {
        std::vector<std::pair<double,int>> candidates;
        for(int shift=-height+minOverlap; shift<=height-minOverlap; ++shift)
            if(shift && (!forcedShift || shift==forcedShift)) candidates.emplace_back(scoreShift(shift,false),shift);
        const int shortlist=std::min(32,int(candidates.size()));
        std::partial_sort(candidates.begin(),candidates.begin()+shortlist,candidates.end());
        for(int i=0; i<shortlist; ++i) verify(candidates[i].second);
    }
    std::sort(verified.begin(), verified.end());
    if (qEnvironmentVariableIsSet("JIETUBA_STITCH_DIAGNOSTICS")) {
        std::fprintf(stderr, "match body=%d..%d columns=%zu:", movingTop, movingBottom, columns.size());
        for (int i=0; i<std::min(5, int(verified.size())); ++i) std::fprintf(stderr, " %d:%.2f", verified[i].second, verified[i].first);
        std::fprintf(stderr, "\n");
    }
    if (verified.empty() || verified.front().first > 10.0) return Result::NoOverlap;
    const auto [best, bestShift] = verified.front();
    for (const auto &[score, shift] : verified) {
        if (std::abs(shift - bestShift) > 2 && score <= best + 0.5) return Result::NoOverlap;
    }
    // 裁剪均值分数低还不够：至少六个有纹理行必须在正文像素上支持这个位移。
    // A low trimmed background error is insufficient: require foreground support from at least six rows.
    int support=0;
    for(int y=std::max(movingTop,movingTop-bestShift); y<std::min(movingBottom,movingBottom-bestShift); ++y) {
        if(stationary[y] || stationary[y+bestShift]) continue;
        const auto *a=oldRow(y+bestShift), *b=newRow(y);
        int textured=0, aligned=0;
        for(int x:columns) if(x>0 && (difference(a[x],a[x-1])>24 || difference(b[x],b[x-1])>24)) {
            ++textured; aligned+=difference(a[x],b[x])<=12;
        }
        support+=textured>=2 && aligned*5>=textured*4;
    }
    if(support<6) return Result::NoOverlap;
    const int proposedPosition=position_+bestShift;
    if(proposedPosition>=0 && proposedPosition+height<=result_.height()) {
        // 已采集范围内只移动定位帧，不能把状态栏/末页再粘到回看处。
        // Navigation inside captured history is read-only, including its fixed UI and saved tail.
        previous_=frame; position_=proposedPosition;
        movingColumns_=columns; movingTop_=movingTop; movingBottom_=movingBottom;
        if(position_==tailPosition_) tailFrame_=frame;
        return Result::Navigated;
    }
    // 向起点之前扩展仍可前插正文；翻转期间保护原来的末页快照，恢复后只平移它的坐标。
    // Prepending before the capture origin reuses the mirrored compositor while preserving the saved tail.
    if(bestShift<0) {
        const QImage savedTail=tailFrame_;
        const int savedTailPosition=tailPosition_, savedHeight=result_.height();
        mirror();
        const auto result=appendImpl(frame.transformed(QTransform().scale(1,-1)),-bestShift);
        mirror();
        tailFrame_=savedTail;
        tailPosition_=savedTailPosition+result_.height()-savedHeight;
        return result;
    }
    constexpr qint64 maxPixels = 100000000;
    const qint64 newHeight = std::max(qint64(result_.height()), qint64(position_) + bestShift + height);
    if (newHeight > 32767 || newHeight * width > maxPixels) return Result::TooLarge;
    const int newPosition = position_ + bestShift;
    // 核心区别：保留到旧正文末尾，再用新帧覆盖旧底栏，而非在旧底栏之后追加。
    // Replace the old footer at the document boundary instead of appending after it.
    const int keepRows = newPosition + height > result_.height() ? std::min(result_.height(), position_ + movingBottom) : result_.height();
    if (newPosition + movingTop > keepRows) return Result::NoOverlap;
    // 横向旋转后，一行对应原图一列；纵向浮层修补会把重复字干/代码底色当成遮挡，撕裂文字。
    // A rotated scanline is an original column. Vertical overlay repair can tear repeated glyphs.
    // 横向仅按已验证位移拼接，保留首帧重叠像素，不对正文做遮挡替换。
    // Horizontal mode preserves overlap pixels and only appends confidently translated content.
    if (axis_ == Axis::Vertical) {
    QImage obscured(width, height, QImage::Format_Grayscale8);
    if (obscured.isNull()) return Result::TooLarge;
    obscured.fill(0);
    for (int y = movingTop; y < movingBottom; ++y)
        if (stationary[y]) std::fill_n(obscured.scanLine(y), width, uchar(255));

    // 必须排除“另一帧浮层投影到此处”的影子：若任一可用方向给出清晰对齐，
    // 当前坐标就是正文证据，不能因为另一个方向撞到导航条而把它标坏。
    // A clean alignment in either available direction rules out an opaque stationary obstruction.
    const auto opaqueMismatch = [&](int x, int y, int threshold) {
        const bool forwardAvailable = y+bestShift < movingBottom;
        const bool backwardAvailable = y-bestShift >= movingTop;
        const bool forward = forwardAvailable && difference(oldRow(y+bestShift)[x],newRow(y)[x]) > threshold;
        const bool backward = backwardAvailable && difference(oldRow(y)[x],newRow(y-bestShift)[x]) > threshold;
        return forwardAvailable && backwardAvailable ? forward && backward : forward || backward;
    };
    // 局部悬浮面板：只把同位置稳定、但与已确定滚动位移不一致的块作为候选，
    // 用带纹理的块作种子，再沿相近背景扩展，避免把普通空白误判为页签。
    // Detect local stationary overlays using textured seeds and similarly colored neighboring tiles.
    constexpr int tile = 12;
    const int cols = (width + tile - 1) / tile, rows = (height + tile - 1) / tile;
    struct Tile { bool candidate = false; bool seed = false; int background = 0; };
    std::vector<Tile> tiles(cols * rows);
    std::vector<int> flood;
    std::vector<bool> selected(cols * rows, false);
    for (int ty = 0; ty < rows; ++ty) for (int tx = 0; tx < cols; ++tx) {
        const int x1 = tx * tile, x2 = std::min(width, x1 + tile);
        const int y1 = std::max(movingTop, ty * tile), y2 = std::min(movingBottom, (ty + 1) * tile);
        if (y2 <= y1) continue;
        int same = 0, differentWhenScrolled = 0, textured = 0, count = 0;
        std::vector<int> backgrounds;
        // 此处必须检查每个像素：按 2 像素步长会漏掉细字笔画，使空白块被误认作固定页签。
        // Check every pixel: a 2-pixel grid aliases thin glyph strokes into a false stationary tile.
        for (int y = y1; y < y2; ++y) for (int x = x1; x < x2; ++x) {
            const QRgb pixel = newRow(y)[x];
            same += difference(oldRow(y)[x], pixel) <= 1;
            const bool forwardMismatch = y+bestShift<movingBottom && difference(oldRow(y+bestShift)[x],pixel)>36;
            const bool backwardMismatch = y-bestShift>=movingTop && difference(oldRow(y)[x],newRow(y-bestShift)[x])>36;
            differentWhenScrolled += forwardMismatch || backwardMismatch;
            if (x > x1) textured += difference(newRow(y)[x - 1], pixel) > 36;
            backgrounds.push_back(qGray(pixel)); ++count;
        }
        auto &entry = tiles[ty * cols + tx];
        if (!count) continue;
        std::nth_element(backgrounds.begin(), backgrounds.begin() + backgrounds.size() / 2, backgrounds.end());
        entry.background = backgrounds[backgrounds.size() / 2];
        entry.candidate = same * 100 >= count * 97 && differentWhenScrolled * 5 >= count;
        entry.seed = entry.candidate && textured >= 2;
        if (entry.seed) { flood.push_back(ty * cols + tx); selected[ty * cols + tx] = true; }
    }
    for (size_t i = 0; i < flood.size(); ++i) {
        const int index = flood[i], x = index % cols, y = index / cols;
        for (const auto delta : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            const int nx = x + delta.first, ny = y + delta.second;
            if (nx < 0 || nx >= cols || ny < 0 || ny >= rows) continue;
            const int next = ny * cols + nx;
            if (!selected[next] && tiles[next].candidate && std::abs(tiles[next].background - tiles[index].background) <= 12) {
                selected[next] = true; flood.push_back(next);
            }
        }
    }
    for (int index : flood) {
        const int x1 = (index % cols) * tile, y1 = std::max(movingTop, (index / cols) * tile);
        const int x2 = std::min(width, x1 + tile), y2 = std::min(movingBottom, (index / cols + 1) * tile);
        for (int y=y1; y<y2; ++y) std::fill(obscured.scanLine(y)+x1,obscured.scanLine(y)+x2,uchar(255));
    }
    // 块检测只负责定位种子，最后逐像素扩到真实边界，避免页签恰好不对齐网格时漏掉边缘。
    // Refine tile seeds to pixel boundaries so overlays need not align with the detector grid.
    QImage nearby(width, height, QImage::Format_Grayscale8);
    if (nearby.isNull()) return Result::TooLarge;
    nearby.fill(0);
    std::vector<int> edgePixels;
    for (int index : flood) {
        const int x1 = (index % cols) * tile, y1 = std::max(movingTop, (index / cols) * tile);
        const int x2 = std::min(width, x1 + tile), y2 = std::min(movingBottom, (index / cols + 1) * tile);
        for (int y = std::max(movingTop, y1 - tile); y < std::min(movingBottom, y2 + tile); ++y)
            std::fill(nearby.scanLine(y) + std::max(0, x1 - tile), nearby.scanLine(y) + std::min(width, x2 + tile), uchar(255));
        for (int x = x1; x < x2; ++x) { edgePixels.push_back(y1 * width + x); edgePixels.push_back((y2 - 1) * width + x); }
        for (int y = y1; y < y2; ++y) { edgePixels.push_back(y * width + x1); edgePixels.push_back(y * width + x2 - 1); }
    }
    for (size_t i = 0; i < edgePixels.size(); ++i) {
        const int x = edgePixels[i] % width, y = edgePixels[i] / width;
        for (const auto delta : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            const int nx = x + delta.first, ny = y + delta.second;
            if (nx < 0 || nx >= width || ny < movingTop || ny >= movingBottom ||
                obscured.constScanLine(ny)[nx] || !nearby.constScanLine(ny)[nx]) continue;
            if (difference(oldRow(ny)[nx], newRow(ny)[nx]) > 1) continue;
            const bool forward = ny+bestShift<movingBottom && difference(oldRow(ny+bestShift)[nx],newRow(ny)[nx])>12;
            const bool backward = ny-bestShift>=movingTop && difference(oldRow(ny)[nx],newRow(ny-bestShift)[nx])>12;
            if (forward || backward) { obscured.scanLine(ny)[nx] = 255; edgePixels.push_back(ny * width + nx); }
        }
    }
    // 半透明底栏不会在同坐标逐像素相等。用已对齐的清晰正文拟合观测像素：
    // observed = transmission * document + constant。只接受有纹理、低残差的衰减，
    // 反向拟合的增益 > 1 不算遮挡，因此不会把浮层另一侧的清晰文字判成坏行。
    // Fit attenuation against aligned content, not equality at screen coordinates.
    // Positive attenuation with low residual distinguishes translucent UI from its clear counterpart.
    const auto attenuated = [&](const QRgb *observed, const QRgb *clear, int x1, int x2, double *model) {
        double sumX[3]{}, sumY[3]{}, xx = 0, xy = 0, yy = 0;
        int count = 0;
        for (int x = x1; x < x2; ++x) {
            const int a[3]{qRed(clear[x]), qGreen(clear[x]), qBlue(clear[x])};
            const int b[3]{qRed(observed[x]), qGreen(observed[x]), qBlue(observed[x])};
            for (int c = 0; c < 3; ++c) { sumX[c] += a[c]; sumY[c] += b[c]; xx += a[c]*a[c]; xy += a[c]*b[c]; yy += b[c]*b[c]; }
            ++count;
        }
        if (count < 8) return false;
        for (int c = 0; c < 3; ++c) { xx -= sumX[c]*sumX[c]/count; xy -= sumX[c]*sumY[c]/count; yy -= sumY[c]*sumY[c]/count; }
        if (xx < count * 3 * 64) return false;
        const double transmission = xy / xx;
        const double residual = std::max(0.0, yy - xy*xy/xx) / (3*count);
        if (!(transmission > 0.08 && transmission < 0.94 && residual < 9)) return false;
        model[0] = transmission;
        for (int c=0; c<3; ++c) model[c+1] = (sumY[c] - transmission*sumX[c])/count;
        return true;
    };
    for (int y = movingTop; y < movingBottom; ++y) {
        int blocked = 0, preciseBlocked = 0;
        for (int x=0; x<width; ++x) {
            blocked += bool(obscured.constScanLine(y)[x]);
            // 候选块之外再逐像素核实遮挡，拒绝另一个方向已有清晰对齐的影子。
            // Refine broad repair candidates with precise, uncontradicted obstruction evidence.
            preciseBlocked += obscured.constScanLine(y)[x] && difference(oldRow(y)[x],newRow(y)[x])<=1 && opaqueMismatch(x,y,12);
        }
        if (blocked>=std::max(6,width/20)) excluded_[y]=true;
        if (preciseBlocked>=std::max(6,width/20)) confirmedObscured_[y]=true;
        if (confirmedObscured_[y]) continue;
        // 分块拟合也能识别仅占部分宽度的半透明导航条，避免要求整行背景完全相同。
        // Local fits cover translucent bars narrower than the selection.
        const int block = std::max(12, width / 12);
        bool found = false;
        for (int x = 0; x + 8 <= width && !found; x += block) for (bool outgoing : {true, false}) {
            const int end = std::min(width, x + block);
            if ((outgoing && y < bestShift) || (!outgoing && y + bestShift >= height)) continue;
            double model[4];
            if (!attenuated(outgoing ? oldRow(y) : newRow(y), outgoing ? newRow(y-bestShift) : oldRow(y+bestShift), x, end, model)) continue;
            const auto agrees = [&](int row) {
                const int other = row + (outgoing ? -bestShift : bestShift);
                if (row < movingTop || row >= movingBottom || other < 0 || other >= height) return false;
                const auto *observed = outgoing ? oldRow(row) : newRow(row);
                const auto *clear = outgoing ? newRow(other) : oldRow(other);
                double error = 0;
                for (int xx=x; xx<end; ++xx) {
                    const int a[3]{qRed(clear[xx]),qGreen(clear[xx]),qBlue(clear[xx])};
                    const int b[3]{qRed(observed[xx]),qGreen(observed[xx]),qBlue(observed[xx])};
                    for(int c=0;c<3;++c) { const double d=b[c]-model[0]*a[c]-model[c+1]; error += d*d; }
                }
                return error / ((end-x)*3) < 9;
            };
            // 纹理行拟合到的同一透明度/颜色模型可以验证相邻空白行。
            // Extend the SAME fitted blend through flat rows, stopping at the actual bar edge.
            // 不跨越边界猜测，也不凭“空白”把正文标为遮挡。
            // Flatness alone is never evidence of obstruction.
            int begin=y, stop=y+1;
            while (agrees(begin-1)) --begin;
            while (agrees(stop)) ++stop;
            std::fill(excluded_.begin()+begin, excluded_.begin()+stop, true);
            std::fill(confirmedObscured_.begin()+begin, confirmedObscured_.begin()+stop, true);
            found = true; break;
        }
    }
    }
    // 重要：来源以整行为单位。绝不把不同帧的单个字形像素交错写进一行。
    // Preserve coherent scanlines. Never interleave individual glyph pixels from different frames.
    QImage joined(width, int(newHeight), QImage::Format_RGB32);
    if (joined.isNull()) return Result::TooLarge;
    joined.fill(Qt::black);
    std::vector<int> sources(int(newHeight), -1);
    for (int y = 0; y < keepRows; ++y) {
        std::copy_n(result_.constScanLine(y), width * 4, joined.scanLine(y));
        sources[y] = sourceRows_[y];
    }
    for (int y = movingTop; y < height; ++y) {
        const int dest = newPosition + y;
        const int oldSource = sources[dest];
        // 已有清晰整行优先；有遮挡的来源只允许被后续清晰整行替换。
        // A clear stored row wins; replace a blocked row only with a clear observation.
        if (oldSource >= 0) {
            // 临停时位移可小于候选块尺寸：候选块会跨入新露出的清晰正文。
            // A final small scroll may expose clear rows inside a coarse repair tile.
            // 已确认受遮挡的旧行允许被边界外的新行替换，不能让粗候选阻止最后几像素的修复。
            // A broad candidate must not block a clear-edge replacement of a confirmed obscured source.
            const bool incomingBlocked = excluded_[y] && (confirmedObscured_[y] || !confirmedObscured_[oldSource]);
            if (!excluded_[oldSource] || incomingBlocked) continue;
        }
        std::copy_n(frame.constScanLine(y), width * 4, joined.scanLine(dest));
        sources[dest] = y;
    }
    result_ = std::move(joined); sourceRows_ = std::move(sources);
    previous_ = frame; position_ = newPosition;
    tailFrame_=frame; tailPosition_=newPosition;
    movingColumns_=columns; movingTop_=movingTop; movingBottom_=movingBottom;
    return Result::Added;
}

void LinuxScrollStitch::mirror() {
    const int height = previous_.height();
    previous_ = previous_.transformed(QTransform().scale(1, -1));
    result_ = result_.transformed(QTransform().scale(1, -1));
    position_ = result_.height() - height - position_;
    std::reverse(sourceRows_.begin(), sourceRows_.end());
    for (auto &row : sourceRows_) if (row >= 0) row = height - 1 - row;
    std::reverse(excluded_.begin(), excluded_.end());
    std::reverse(confirmedObscured_.begin(),confirmedObscured_.end());
    if(movingBottom_>movingTop_) {
        const int top=movingTop_;
        movingTop_=height-movingBottom_; movingBottom_=height-top;
    }
}

void LinuxScrollStitch::crop(bool before) {
    // 裁剪基于当前已匹配视口，保留的坐标也同步平移，之后仍可继续滚动。
    // Crop relative to the last matched viewport and rebase its document coordinate.
    const int begin = before ? position_ : 0;
    const int end = before ? result_.height() : std::min(result_.height(), position_ + previous_.height());
    result_ = result_.copy(0, begin, result_.width(), end - begin);
    sourceRows_ = std::vector<int>(sourceRows_.begin() + begin, sourceRows_.begin() + end);
    position_ -= begin;
    if (axis_ == Axis::Horizontal && movingBottom_ > movingTop_) {
        // 裁剪使当前视口成为新边界，只在该边界恢复固定栏，保留中间已经拼好的正文。
        // A crop exposes a new edge. Restore just that edge's gutter, leaving the composed body intact.
        const int first = before ? 0 : movingBottom_;
        const int last = before ? movingTop_ : previous_.height();
        for (int y = first; y < last; ++y) {
            std::copy_n(previous_.constScanLine(y), previous_.width() * 4, result_.scanLine(position_ + y));
            sourceRows_[position_ + y] = y;
        }
    }
    // 只有用户显式裁去末页时才更换尾部快照；普通上划绝不走此路径。
    // Only an explicit crop can replace a removed tail with the current viewport.
    if(tailPosition_<begin || tailPosition_+tailFrame_.height()>end) {
        tailFrame_=previous_; tailPosition_=position_;
    } else tailPosition_-=begin;
}

bool LinuxScrollStitch::refreshStationary(const QImage &frame) {
    // 仅在未遮挡的正文能证明零位移时接受当前页。尺寸相同或变化较少本身不是依据，
    // 否则快滚/跳页后的无重叠画面会被错误贴到旧位置。
    // Require a zero-offset match in unobscured content; never treat an unmatched page jump as idle.
    const int width=frame.width(), height=frame.height();
    const int step=std::max(1,width/160);
    int samples=0, changed=0, evidence=0, changedEvidence=0, rows=0;
    int cellEvidence[64]{}, cellChanged[64]{};
    const int top=movingColumns_.empty() ? 1 : std::max(1,movingTop_);
    const int bottom=movingColumns_.empty() ? height-1 : std::min(height-1,movingBottom_);
    std::vector<int> probeColumns=movingColumns_;
    if(probeColumns.empty()) for(int x=1;x<width;x+=step) probeColumns.push_back(x);
    for(int y=top; y<bottom; ++y) {
        if(excluded_[y]) continue;
        ++rows;
        const auto *old=reinterpret_cast<const QRgb *>(previous_.constScanLine(y));
        const auto *now=reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for(int x:probeColumns) {
            if(x<=0) continue;
            const bool different=difference(old[x],now[x])>6;
            ++samples; changed+=different;
            const bool textured=difference(old[x],old[x-1])>24 || difference(now[x],now[x-1])>24;
            evidence+=textured; changedEvidence+=textured && different;
            const int cell=std::min(7,y*8/height)*8+std::min(7,x*8/width);
            cellEvidence[cell]+=textured; cellChanged[cell]+=textured && different;
        }
    }
    if(rows<std::min(48,(bottom-top)/3) || evidence<32 || !samples || changed*200>samples || changedEvidence*100>evidence)
        return false;
    // 固定侧栏不能淹没一小块正文的真实滚动：每个有纹理分区也必须支持零位移。
    // A large fixed sidebar cannot hide movement in a smaller document area.
    for(int cell=0;cell<64;++cell) if(cellEvidence[cell]>=8 && cellChanged[cell]*20>cellEvidence[cell]) return false;
    previous_=frame;
    if(position_==tailPosition_) tailFrame_=frame;
    return true;
}

QImage LinuxScrollStitch::exportImage() const {
    // 横向与 Windows 相同，预览和导出使用拼好的列。把最后整个视口再贴一遍，会把固定行号栏贴进正文。
    // Keep the composed horizontal strips for both settled preview and export. A raw tail would duplicate the fixed gutter.
    if (axis_ == Axis::Horizontal) return result_;
    if(result_.isNull() || tailFrame_.isNull() || tailFrame_.width()!=result_.width() ||
       tailPosition_<0 || tailPosition_+tailFrame_.height()>result_.height()) return result_;
    // 末页保留当前位置的真实完整画面；回看使用 previous_/position_，不影响这里的快照。
    // Keep the complete real tail viewport, independently of the current navigation anchor.
    QImage output=result_;
    for(int y=0; y<tailFrame_.height(); ++y)
        std::copy_n(tailFrame_.constScanLine(y),tailFrame_.width()*4,output.scanLine(tailPosition_+y));
    return output;
}
