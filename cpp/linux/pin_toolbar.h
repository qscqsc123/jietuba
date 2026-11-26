#pragma once
#include <QPointer>
#include <QRectF>
#include <QWidget>
class LinuxPinWindow;
class LinuxSettings;
class QScreen;
namespace jietuba { class Toolbar; }
#ifdef JIETUBA_HAS_LAYER_SHELL
namespace LayerShellQt { class Window; }
#endif

/**
 * Linux 贴图工具栏的透明宿主：复用原版 Toolbar 和 PIN 布局，仅适配 Wayland 表面。
 * 工具栏、更多菜单和调整面板位于同一表面；输入区域只包含可见控件，其余桌面可正常点击。
 * 使用输出内逻辑坐标跟随贴图，避免依赖 Wayland 不保证有效的顶层窗口全局位置。
 * 贴图画布接管绘图工具和撤销栈；宿主只负责面板布局及输入区域。
 *
 * Transparent Linux host for the original Toolbar/PIN profile. The bar, More popup
 * and layout editor share one Wayland surface. An input mask leaves the rest of the
 * desktop interactive; output-local coordinates keep positioning independent of
 * synthetic global window positions. The pin canvas owns the drawing tools and undo history.
 */
class LinuxPinToolbar final : public QWidget {
    Q_OBJECT
public:
    LinuxPinToolbar(LinuxPinWindow *pin, LinuxSettings &settings);
    jietuba::Toolbar *toolbar() const { return toolbar_; }
    void showFor(QScreen *screen, const QRectF &pinRect);
    void syncPosition(QScreen *screen, const QRectF &pinRect);
    void hideControls();
    bool editingLayout() const;
    static QRect toolbarGeometry(const QSize &output, const QRectF &pinRect, const QSize &bar);
signals:
    void controlsClosing();
    void panelPositionChanged(bool below);
    void hoverChanged(bool hovered);
    void layoutEditingChanged(bool editing);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void positionToolbar();
    void refreshInputRegion();
    jietuba::Toolbar *toolbar_ = nullptr;
    QPointer<LinuxPinWindow> pin_;
    QPointer<QScreen> screen_;
    QRectF pinRect_;
    bool refreshing_ = false;
#ifdef JIETUBA_HAS_LAYER_SHELL
    LayerShellQt::Window *layer_ = nullptr;
#endif
};
