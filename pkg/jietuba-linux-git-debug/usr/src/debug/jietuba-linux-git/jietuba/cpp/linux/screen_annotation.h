#pragma once
#include <QWidget>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
class QScreen;
class QPushButton;
class LinuxSettings;
class LinuxAnnotationCanvas;
class KWinPointerTracker;
namespace jietuba {class Toolbar;}
/**
 * Linux 独立屏幕标注表面。透明画布直接叠在当前输出，使用原版 ANNOTATION 布局、
 * 图标、工具、样式面板和撤销栈；不截图、不冻结下方应用。
 * 穿透模式仅工具栏/面板接收输入，绘图模式才接管屏幕。原生输入区域与绘制区域分离，
 * 防止 QWidget mask 造成拖影。屏幕配置变化时安全退出，避免遗留输入遮罩。
 * Transparent annotation overlay for the current output, sharing Windows toolbar layout,
 * icons, tools, panels and undo history without freezing the desktop. Pass-through mode
 * limits native input to controls; drawing mode receives the canvas. Painting remains unmasked.
 * Output reconfiguration exits safely instead of leaving a stale input-blocking overlay.
 */
class LinuxScreenAnnotation final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxScreenAnnotation(QScreen *screen,LinuxSettings &settings);
    ~LinuxScreenAnnotation() override;
    void showPrepared();
    static QRect toolbarPlacement(const QRect &available,const QSize &toolbar);
    LinuxAnnotationCanvas *canvas() const {return canvas_;}
protected:
    bool eventFilter(QObject *,QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void closeEvent(QCloseEvent *) override;
private:
    void sync();
    void placeToolbar();
    void refreshInput();
    QPointer<QScreen> output_;
    jietuba::Toolbar *toolbar_=nullptr;
    LinuxAnnotationCanvas *canvas_=nullptr;
    QPushButton *pointer_,*boardButton_,*highlight_,*hide_;
    bool board_=false,hidden_=false,halo_=false,queued_=false,closing_=false,placing_=false;
    QPointF pointerPosition_{-100,-100};
    KWinPointerTracker *tracker_ = nullptr;
    QWidget *effects_ = nullptr;
    struct Ripple { QPointF position; qint64 born; bool right; };
    QList<Ripple> ripples_;
    QElapsedTimer clock_;
    QTimer rippleTimer_;
};
