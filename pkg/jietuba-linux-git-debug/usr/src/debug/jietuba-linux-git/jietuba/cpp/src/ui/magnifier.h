#pragma once
#include "settings/color_formats.h"
#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPen>
#include <QWidget>
#include <optional>
#include <functional>

class QGraphicsView;

namespace jietuba {
class CanvasScene;
class CanvasView;
class ToolSettingsManager;

/**
 * 跟随取色点的原版放大镜：直接采样截图原始像素，局部缓存放大图，并显示坐标与颜色。
 * Linux 通过 Options 和交互回调接入同一套绘制；它是截图表面的透明输入子控件，
 * 不申请额外的 Wayland 窗口，也不会把放大镜或遮罩本身的颜色当成取色结果。
 * Source-pixel magnifier with a local image cache, coordinates and color formats.
 * Linux supplies Options/interaction callbacks to this shared renderer. It stays a
 * mouse-transparent capture child, never samples its own overlay and repaints locally.
 */
class MagnifierOverlay : public QWidget {
    Q_OBJECT
public:
    // 100% base sizes; apply_scale computes the scaled members of the same names.
    static constexpr int BASE_MAG_WIDTH = 172;   // round(150 * 1.15)
    static constexpr int BASE_MAG_HEIGHT = 126;  // round(120 * 1.05)
    static constexpr int BASE_INFO_WIDTH = BASE_MAG_WIDTH;
    static constexpr int BASE_INFO_LINE_HEIGHT = 20;
    static constexpr int BASE_INFO_PADDING_V = 10;  // top and bottom together
    // Sampled source pixels are content and do not scale with the UI.
    static constexpr int SAMPLE_SIZE = 48;
    static constexpr int BASE_EDGE_MARGIN = 16;
    static constexpr int SWATCH_SIZE = 16;
    static constexpr int INFO_FONT_PT = 13;  // upper bound; shrinks to fit
    static constexpr int HINT_FONT_PT = 9;
    // No grid below this cell size in screen pixels: the lines would cover the pixels.
    static constexpr int GRID_MIN_CELL = 5;

#ifdef JIETUBA_LINUX_UI_PREVIEW
    /**
     * Linux 注入的显示配置：复用原版像素采样和绘制，不依赖 Windows 设置或原生剪贴板。
     * 坐标始终是截图原始像素，Qt 视图变换只参与面板定位，不参与取色。
     * Linux display options reuse the original renderer without Windows services.
     * Sampling stays in source pixels; view transforms affect placement only.
     */
    struct Options {
        bool enabled = true, grid = false, selection = false, hints = true;
        double zoom = 4;
        QString copyKey = QStringLiteral("C");
        QVector<color_formats::ColorFormat> formats = color_formats::default_formats();
    };
    MagnifierOverlay(QWidget *parent, CanvasScene *scene, QGraphicsView *view, const Options &options);
    void configure(const Options &options);
    std::function<bool()> interaction_blocked;
    std::function<void(double)> zoom_changed;
#else
    MagnifierOverlay(QWidget *parent, CanvasScene *scene, CanvasView *view, ToolSettingsManager *config_manager = nullptr);
#endif
    // Image area plus info lines: coordinates, the current colour format, then hints.
    int combined_height() const;
    void apply_scale();
    // Live local sample; a null image returns to the scene background.
    void set_sample_image(const QImage &image, const QRectF &global_rect);
    // Next enabled colour format (Shift); no-op with a single format.
    void cycle_color_format();
    // Records the scene position, moves into place and repaints; decides show/hide.
    void update_cursor(const QPointF &scene_pos);
    void clear_cursor();
    // Repaint after outside state changes, such as the toolbar appearing.
    void refresh();
    // The format currently shown, i.e. the one Shift cycled to.
    QString get_color_info_text() const;
    bool copy_color_info();
    void adjust_zoom(int delta);
    double get_zoom_factor() const { return zoom_factor_; }
    // Odd side for the current zoom, so the centre pixel sits under the crosshair.
    int sample_size() const;
    bool should_render() const;

    CanvasScene *scene = nullptr;
#ifdef JIETUBA_LINUX_UI_PREVIEW
    QGraphicsView *view = nullptr;
#else
    CanvasView *view = nullptr;
#endif
    ToolSettingsManager *config_manager = nullptr;
    std::optional<QPointF> cursor_scene_pos;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    // Pens built once from the current theme.
    void refresh_theme_colors();
    // Display switches, read once.
    void refresh_display_options();
    QStringList hint_texts() const;
    static QFont fitted_font(QPainter &painter, const QString &text, int max_width, int base_size = 11, int min_size = 7);
    static QString widest(QPainter &painter, const QStringList &texts);
    // Sized once from the longest possible texts (largest virtual desktop coordinates).
    void ensure_fixed_fonts(QPainter &painter, int text_width, int value_width);
    // Grid cells follow the source rect actually drawn, not the nominal zoom.
    void draw_pixel_grid(QPainter &painter, const QRect &mag_rect, const QRect &src) const;
    // Mask outside the selection and its border on the pixel boundaries, mapped like the grid.
    void draw_selection(QPainter &painter, const QRect &mag_rect, const QRect &src) const;
    void draw_combined_magnifier(QPainter &painter, const QRect &rect, const QColor &color);
    void draw_info_lines(QPainter &painter, const QRect &info_rect, const QColor &color);
    QImage background_image() const;
    // Scene position of the background image's top-left pixel.
    std::optional<QPointF> image_origin() const;
    std::optional<QPoint> scene_to_image_point(const QPointF &scene_pos) const;
    std::optional<QPointF> scene_to_pixel_center(const QPointF &scene_pos) const;
    QColor sample_color(const QImage &image) const;
    void update_sample_cache(const QImage &image);
    QPoint calc_widget_pos(const QPointF &scene_pos) const;
    void clear_sample_cache();
    void clear_font_cache();

    int mag_width_ = BASE_MAG_WIDTH, mag_height_ = BASE_MAG_HEIGHT, info_width_ = BASE_INFO_WIDTH;
    int info_line_height_ = BASE_INFO_LINE_HEIGHT, info_padding_v_ = BASE_INFO_PADDING_V, edge_margin_ = BASE_EDGE_MARGIN;
    int radius_ = 6;
    QImage sample_image_;
    std::optional<QRectF> sample_rect_;
    QFont font_, info_font_;
    double zoom_factor_ = 2.0, zoom_min_ = 1.0, zoom_max_ = 8.0;
    std::optional<QPoint> last_sample_pt_;
    std::optional<QRect> cached_source_rect_;
    // Background image pixel the cached sample starts at.
    QPoint cached_sample_origin_;
    QImage cached_sample_image_;
    std::optional<QFont> fixed_info_font_, fixed_hint_font_;
    std::optional<QFontMetrics> fixed_info_metrics_, fixed_hint_metrics_;
    QPen pen_teal_2_, pen_teal_1_, pen_grid_;
    QBrush brush_crosshair_;
    QPen pen_white_2_ = QPen(QColor(255, 255, 255), 2);
    QBrush brush_bg_ = QBrush(QColor(40, 40, 45, 220));
    QBrush brush_black_a_ = QBrush(QColor(0, 0, 0, 180));
    QColor color_pos_ = QColor(100, 240, 220);
    QColor color_value_ = QColor(255, 200, 100);
    QColor color_hint_ = QColor(180, 180, 180);
    // Swatch outline, so dark samples stay visible on the dark background.
    QPen pen_swatch_ = QPen(QColor(255, 255, 255, 200), 1);
    QColor mask_color_ = QColor(0, 0, 0, 120);
    QColor selection_color_;
    bool enabled_ = true, show_grid_ = false, show_hint_ = true, show_selection_ = false;
    QVector<color_formats::ColorFormat> formats_;
    int format_index_ = 0;
#ifdef JIETUBA_LINUX_UI_PREVIEW
    QString copy_key_ = QStringLiteral("C");
#endif
};
}
