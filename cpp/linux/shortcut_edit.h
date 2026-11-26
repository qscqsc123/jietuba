#pragma once
#include "ui/inapp_key_edit.h"
#include <QTimer>
/**
 * Linux 原版风格键位编辑器：先清除再点击空框录入，完整组合键短暂停顿后提交，Esc 恢复进入前的值。
 * × 独立表示删除；不把 Esc 当删除键。全局编辑不接受鼠标手势，本地编辑可记录中键。
 * 提交和系统确认分离，调用方仅在桌面/配置存储确认后更新 committed value。
 * Original-style Linux shortcut capture: clear before replacement, click empty slots to record, debounce complete chords, Escape cancels.
 * The clear affordance deletes explicitly. Global keys reject mouse gestures; local keys may use middle-click.
 * Editing is separate from confirmation; the owner acknowledges successful persistence/desktop registration.
 */
class LinuxShortcutEdit final : public jietuba::InAppKeyEdit {
    Q_OBJECT
public:
    explicit LinuxShortcutEdit(QWidget *parent=nullptr,bool global=false);
    void setCommittedValue(const QString &value);
    bool isRecording() const {return recording_;}
    void captureFromDesktop(const QString &key);
signals:
    void commitRequested(const QString &value);
    void recordingChanged(bool recording);
    void editCancelled();
protected:
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
private:
    void finish();
    void setRecording(bool recording);
    QString before_;
    QTimer debounce_;
    bool recording_=false;
};
