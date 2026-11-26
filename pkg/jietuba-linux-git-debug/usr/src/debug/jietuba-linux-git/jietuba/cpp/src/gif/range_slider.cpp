#include "gif/range_slider.h"
#include "core/ui_scale.h"
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

namespace jietuba {
RangeSlider::RangeSlider(QWidget *parent) : QWidget(parent) {
    setCursor(QCursor(Qt::PointingHandCursor));
    apply_scale();
}

void RangeSlider::apply_scale() {
    setFixedHeight(scaled(BASE_HEIGHT));
    setMinimumWidth(scaled(BASE_MIN_WIDTH));
    update();
}

void RangeSlider::set_range(int total) {
    total_ = std::max(total, 1);
    trim_start_ = 0;
    trim_end_ = total_ - 1;
    playhead_ = 0;
    update();
}

void RangeSlider::set_playhead(int index) {
    playhead_ = std::max(0, std::min(index, total_ - 1));
    update();
}

void RangeSlider::set_trim(int start, int end) {
    trim_start_ = std::max(0, start);
    trim_end_ = std::min(end, total_ - 1);
    update();
}

QRect RangeSlider::track_rect() const {
    const int track_h = scaled(BASE_TRACK_H);
    const int margin = scaled(BASE_HANDLE_W);
    const int y = python_floordiv(height() - track_h, 2);
    return QRect(margin, y, width() - 2 * margin, track_h);
}

int RangeSlider::index_to_x(int index) const {
    const auto tr = track_rect();
    if (total_ <= 1) return tr.left();
    return tr.left() + static_cast<int>(static_cast<double>(index) / (total_ - 1) * tr.width());
}

int RangeSlider::x_to_index(int x) const {
    const auto tr = track_rect();
    const double ratio = std::max(0.0, std::min(1.0, static_cast<double>(x - tr.left()) / std::max(tr.width(), 1)));
    return static_cast<int>(python_round(ratio * (total_ - 1)));
}

QRect RangeSlider::handle_rect(bool start) const {
    const int w = scaled(BASE_HANDLE_W), h = scaled(BASE_HANDLE_H);
    const int x = start ? index_to_x(trim_start_)
                        : total_ == 1 ? track_rect().left() + track_rect().width() : index_to_x(trim_end_);
    const int y = python_floordiv(height() - h, 2);
    // 两个手柄分别画在边界外侧，选区很短时也不会互相遮住。
    // Draw each grip outside its boundary so even a very short range shows both grips.
    return QRect(start ? x - w : x, y, w, h);
}

void RangeSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto tr = track_rect();
    // The background track.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#8491a3"));
    p.drawRoundedRect(tr, 3, 3);
    // The selected range.
    const int x1 = handle_rect(true).right() + 1, x2 = handle_rect(false).left();
    p.setBrush(QColor("#63b3ff"));
    p.drawRoundedRect(QRect(x1, tr.top(), x2 - x1, tr.height()), 3, 3);
    // Trim handles.
    for (const bool start : {true, false}) {
        const auto rect = handle_rect(start);
        p.setBrush(QColor("#1476c9"));
        p.setPen(QPen(QColor("#b4deff"), 1));
        p.drawRoundedRect(rect, 2, 2);
        const QPointF center = QRectF(rect).center();
        const qreal direction = start ? 1 : -1;
        const auto offset = scaled(2), tip = scaled(4);
        p.setPen(QPen(Qt::white, scaled_f(1.7), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{center + QPointF(-direction * offset, -tip),
                                 center + QPointF(direction * offset, 0),
                                 center + QPointF(-direction * offset, tip)});
    }
    // The playhead.
    const int playhead_w = scaled(BASE_PLAYHEAD_W);
    const int px = index_to_x(playhead_);
    const int py = tr.bottom() + 1;
    p.setPen(QPen(QColor("#20252c"), 1));
    p.setBrush(QColor("#ffd387"));
    p.drawPolygon(QPolygonF{QPointF(px, py), QPointF(px - playhead_w/2, py + scaled(6)),
                            QPointF(px + playhead_w/2, py + scaled(6))});
}

void RangeSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || total_ <= 1) return;
    const auto pos = event->pos();
    const int slack = scaled(BASE_GRAB_SLACK);
    const int start_x = index_to_x(trim_start_), end_x = index_to_x(trim_end_);
    const bool start_hit = handle_rect(true).adjusted(-slack, -slack, slack, slack).contains(pos);
    const bool end_hit = handle_rect(false).adjusted(-slack, -slack, slack, slack).contains(pos);
    // 手柄很近时选择靠近鼠标的一个，避免右手柄被左手柄的热区吞掉。
    // Overlapping hit areas select the nearest handle; preserve the initial grab offset.
    if (start_hit && (!end_hit || std::abs(pos.x() - start_x) <= std::abs(pos.x() - end_x))) {
        dragging_ = "start";
        grab_offset_ = pos.x() - start_x;
    } else if (end_hit) {
        dragging_ = "end";
        grab_offset_ = pos.x() - end_x;
    } else {
        emit seek_requested(x_to_index(pos.x()));
    }
    event->accept();
}

void RangeSlider::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_.isEmpty()) {
        const int idx = x_to_index(event->pos().x() - grab_offset_);
        const auto previous = get_trim_range();
        if (dragging_ == "start") trim_start_ = std::max(0, std::min(idx, trim_end_ - 1));
        else trim_end_ = std::min(total_ - 1, std::max(idx, trim_start_ + 1));
        if (previous != get_trim_range()) emit trim_changed(trim_start_, trim_end_);
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !dragging_.isEmpty()) {
        mouseMoveEvent(event);
        dragging_.clear();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

}
