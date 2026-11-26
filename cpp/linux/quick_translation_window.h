#pragma once
#include "translation_client.h"
#include <QWidget>
#include <QPointer>
class LinuxSettings;
class LinuxSpeechDispatcher;
class LinuxPointerLocator;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QScreen;
/**
 * 鼠标旁的文字翻译浮窗，复用已配置的翻译服务；Enter 提交，Shift+Enter 换行，Esc 关闭。
 * 顶部固定预留状态行，朗读/翻译/完整窗口集中于底栏，输入与译文左右对齐。
 * layer-shell 使用固定透明输出宿主，浮窗作为子控件移动；拖动只用宿主局部坐标，
 * 避免异步改变原生窗口边距导致跳动。普通 Wayland 窗口交给合成器 startSystemMove。
 * Cursor-adjacent translator with a reserved header status and aligned editors/action footer.
 * Layer-shell uses a fixed transparent output host and local child dragging, avoiding asynchronous
 * margin feedback. Ordinary Wayland windows delegate dragging to the compositor's system move.
 */
class LinuxQuickTranslationWindow final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxQuickTranslationWindow(LinuxSettings &settings);
    ~LinuxQuickTranslationWindow() override;
    void showNearCursor();
    static QRect placement(const QPoint &cursor,const QRect &available,const QSize &size);
protected:
    bool eventFilter(QObject *,QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
private:
    void translate();
    void speak();
    void refreshSpeechState();
    void speechUnavailable(const QString &details);
    void showAt(QScreen *screen,const QPoint &cursor);
    void fitContent();
    void fitChoices();
    void place();
    void setStatus(const QString &text,bool error=false);
    void refreshStatus();
    bool beginDrag(const QPointF &position);
    void dragTo(const QPointF &position);
    void endDrag();
    static void flashButton(QPushButton *button);
    LinuxSettings &settings_;
    LinuxTranslationClient client_;
    QPlainTextEdit *input_=nullptr,*output_=nullptr;
    QLabel *status_=nullptr;
    QComboBox *provider_,*target_;
    QPushButton *submit_,*speak_;
    QWidget *header_=nullptr;
    QPointer<QScreen> outputScreen_;
    QPointer<QWidget> surface_;
    QPoint anchor_,manualTopLeft_;
    QPointF dragOffset_;
    bool dragging_=false,manualPosition_=false;
    QString statusText_;
    QPointer<LinuxPointerLocator> locator_;
    QPointer<LinuxSpeechDispatcher> dispatcher_;
};
