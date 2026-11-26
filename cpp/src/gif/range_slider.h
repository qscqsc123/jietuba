#pragma once

#include <QWidget>
#include <utility>

namespace jietuba {
// GIF 时间轴：两个手柄定义保留区间，播放指针独立显示当前帧。
// Shared GIF timeline: the two handles select a range independently of the playhead.
class RangeSlider : public QWidget {
    Q_OBJECT
public:
    // Base sizes (pixels at 100%); hit testing uses the same values, so the visible and
    // clickable areas stay aligned at any scale.
    static constexpr int BASE_TRACK_H = 6;
    static constexpr int BASE_HANDLE_W = 16;
    static constexpr int BASE_HANDLE_H = 22;
    static constexpr int BASE_PLAYHEAD_W = 8;
    static constexpr int BASE_HEIGHT = 32;
    static constexpr int BASE_MIN_WIDTH = 200;
    static constexpr int BASE_GRAB_SLACK = 4;
    explicit RangeSlider(QWidget *parent = nullptr);
    // Recomputes the timeline's own size for the current scale.
    void apply_scale();
    void set_range(int total);
    void set_playhead(int index);
    void set_trim(int start, int end);
    int trim_start() const { return trim_start_; }
    int trim_end() const { return trim_end_; }
    std::pair<int, int> get_trim_range() const { return {trim_start_, trim_end_}; }
signals:
    void trim_changed(int start, int end);
    // A click on empty track jumps there.
    void seek_requested(int index);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    QRect track_rect() const;
    int index_to_x(int index) const;
    int x_to_index(int x) const;
    QRect handle_rect(bool start) const;
    int total_ = 1;
    int trim_start_ = 0;
    int trim_end_ = 0;
    int playhead_ = 0;
    // "start", "end" or empty.
    QString dragging_;
    int grab_offset_ = 0;
};

}
