#pragma once
#include <QObject>
#include <QDBusContext>
#include <QTemporaryFile>
#include <QPointF>
#include <QRect>

/** KDE 的临时光标观察器；不抓取输入，关闭标注时卸载脚本。
 * Temporary KWin cursor observer: never grabs input and unloads on annotation exit. */
class KWinPointerTracker final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.jietuba.Pointer")
public:
    explicit KWinPointerTracker(QObject *parent = nullptr);
    ~KWinPointerTracker() override;
    void start(bool hitWindows=false);
signals:
    void windowHit(QPoint point,QRect bounds,int pid);
    void moved(const QPointF &global);
public slots:
    void hit(int x,int y,int left,int top,int width,int height,int pid);
    void position(int x, int y);
private:
    QTemporaryFile script_;
    QString name_, path_, owner_;
    bool started_ = false;
};
