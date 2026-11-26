#pragma once
#include <QImage>
#include <QString>
#include <vector>

/**
 * Linux 长截图的逐帧拼接器。
 * 支持纵向和横向双向拼接：以变化列和固定行带估计正文位移，替换旧边栏，
 * 并利用后续清晰帧修补同一文档位置的固定浮层。未观测到的内容保留原始像素，
 * 不生成或臆造被遮住的正文。只有位移可信时才提交合成结果。
 * 已采集范围内回看只更新定位；最远末页的快照独立保留。只有跨出采集边界才扩展图像。
 * 不匹配、重复画面或尺寸变化均保留原结果，避免误拼；像素及总高度受限。
 *
 * Bidirectional strip stitcher for vertical and rotated horizontal captures on Linux. It appends only confidently matched
 * overlaps; mismatches, duplicate frames and size changes never corrupt the result.
 * Navigation within captured history is read-only; the frontier viewport is saved independently.
 */
class LinuxScrollStitch {
public:
    enum class Result { Added, Navigated, Updated, Unchanged, NoOverlap, Invalid, TooLarge };
    enum class Axis { Vertical, Horizontal };
    // 横向调用方先旋转帧；坐标匹配共用，覆盖末页和浮层修补只用于纵向。
    // Horizontal callers rotate the frames first. Only vertical captures repair overlays and restore a live tail.
    explicit LinuxScrollStitch(const QImage &first, Axis axis = Axis::Vertical);
    Result append(const QImage &frame);
    const QImage &image() const { return result_; }
    // 横向直接导出拼好的列；纵向只用最远采集边界的快照更新末页，回看不写历史。
    // Export the composed columns horizontally; refresh only the furthest viewport vertically, never the review position.
    QImage exportImage() const;
    void crop(bool before);
    int position() const { return position_; }
private:
    QImage previous_;
    QImage result_;
    // 定位帧与末页快照必须独立：previous_ 可随回看变化，tailFrame_ 只随采集边界变化。
    // Separate the navigation anchor from the immutable tail while reviewing history.
    QImage tailFrame_;
    int tailPosition_ = 0;
    // 零位移核实仅使用曾经真正滚动的正文列/行带，固定侧栏不能成为替换页面的证据。
    // Verify idle against the previously moving document, never a stationary sidebar.
    std::vector<int> movingColumns_;
    int movingTop_ = 0, movingBottom_ = 0;
    // 每一输出行记录来源视口的行号；遮挡识别只决定整行替换，杜绝文字碎片化。
    // Row provenance allows coherent repairs without a per-pixel validity mosaic.
    std::vector<int> sourceRows_;
    std::vector<bool> excluded_;
    // 已确认遮挡与粗候选分开，临停的小位移仍能用清晰行修补旧导航条。
    // Distinguish confirmed obstruction from coarse repair candidates for small final scrolls.
    std::vector<bool> confirmedObscured_;
    bool refreshStationary(const QImage &frame);
    Result appendImpl(const QImage &input, int forcedShift);
    void mirror();
    int position_ = 0;
    Axis axis_;
};
