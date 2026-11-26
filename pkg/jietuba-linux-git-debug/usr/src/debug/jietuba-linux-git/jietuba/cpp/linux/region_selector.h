#pragma once
#include <QPointer>
#include <QRectF>
#include <QWidget>
#include <QVariantAnimation>
#include <QTimer>
#include <QProcess>
#include <optional>

class QScreen;

/**
 * 实时桌面的透明选区层：只绘制边框与提示，不持有截图，不用黑色底图遮盖桌面。
 * 用户松开鼠标后发送屏幕内逻辑坐标，由会话撤下所有选区层，再调用采集后端。
 * 支持 Escape/右键取消、Enter/双击全屏，并在输出变动时取消，避免使用过期坐标。
 * A transparent live-desktop selector, holding no screenshot or opaque background.
 * Emits output-local logical bounds on release; the session unmaps every selector
 * before capturing. Escape/right-click cancel; Enter/double-click select the output.
 * Output changes cancel selection rather than capturing with stale coordinates.
 */
class LinuxRegionSelector final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxRegionSelector(QScreen *screen);
    void configureAssist(bool crosshair,const QString &mode,bool animate);
    void showPrepared();
    bool isDragging() const { return dragging_; }
    void captureCurrent();
    void setParked(bool parked);
    // 撤场先提交透明帧，关闭动画不得把旧边框保留进下一次区域采集。
    // Submit a transparent frame before unmapping so closing animations cannot retain the outline.
    void withdraw();
    static QRect nativeRegion(const QRectF &logical, const QSize &output, const QSize &pixels);
signals:
    void selected(const QRectF &logicalRegion);
    void cancelled();
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    void setSnap(QRect area);
    void probeElement();
    void leaveEvent(QEvent *) override;
    QVariantAnimation snapAnimation_;
    QTimer probeTimer_;
    QProcess probe_;
    QRect target_, drawn_, window_, pressSnap_, probeWindow_;
    QPoint hitPoint_, probePoint_;
    quint64 probeSerial_=0;
    int hitPid_=0, probePid_=0;
    bool crosshair_=false, animate_=false, elements_=false;
    std::optional<QPoint> pointer_;
    QPointF bounded(const QPointF &position) const;
    void acceptRegion(const QRectF &region);
    QPointer<QScreen> screen_;
    QPointF start_;
    QRectF region_;
    bool dragging_ = false, completed_ = false, layer_ = false;
    bool withdrawing_ = false, parked_ = false, heldRegion_ = false;
};
