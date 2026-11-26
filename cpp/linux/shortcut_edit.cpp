#include "shortcut_edit.h"
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalBlocker>
LinuxShortcutEdit::LinuxShortcutEdit(QWidget *parent,bool global):InAppKeyEdit(parent,!global,true) {
    setPlaceholderText(QStringLiteral("添加快捷键")); debounce_.setSingleShot(true); debounce_.setInterval(500);
    setToolTip(QStringLiteral("先点 × 清除原快捷键，再点击空框录入；Esc 撤销当前录入。"));
    connect(&debounce_,&QTimer::timeout,this,&LinuxShortcutEdit::finish);
}
void LinuxShortcutEdit::setRecording(bool value) {
    if(recording_==value) return;
    recording_=value; setPlaceholderText(value ? QStringLiteral("按下组合键…") : QStringLiteral("添加快捷键"));
    emit recordingChanged(value);
}
void LinuxShortcutEdit::setCommittedValue(const QString &value) {
    if(recording_) return;
    const QSignalBlocker blocker(this); before_=value; setText(value); setErrorState(false);
}
void LinuxShortcutEdit::focusInEvent(QFocusEvent *event) {before_=text(); setRecording(text().isEmpty()); InAppKeyEdit::focusInEvent(event);}
void LinuxShortcutEdit::focusOutEvent(QFocusEvent *event) {InAppKeyEdit::focusOutEvent(event); if(recording_) finish();}
void LinuxShortcutEdit::mousePressEvent(QMouseEvent *event) {
    if(event->button()==Qt::LeftButton && !text().isEmpty() && event->position().x()>=width()-height()) {
        event->accept(); debounce_.stop(); setRecording(false); setText({}); clearFocus(); emit commitRequested({}); return;
    }
    // 已有绑定只能显式删除，不能因误点或随后敲键被覆盖；空槽才进入录入。
    // Require explicit deletion before replacement; only empty slots start a capture.
    if(!recording_ && !text().isEmpty()) {event->accept(); return;}
    if(!recording_) {before_=text(); setRecording(true);}
    InAppKeyEdit::mousePressEvent(event);
    if(event->button()==Qt::MiddleButton && text().contains("mousemiddle")) debounce_.start();
}
void LinuxShortcutEdit::keyPressEvent(QKeyEvent *event) {
    if(!recording_) {event->accept(); return;}
    debounce_.stop();
    if(event->key()==Qt::Key_Escape) {
        event->accept(); {const QSignalBlocker blocker(this); setText(before_);} setRecording(false); clearFocus(); emit editCancelled(); return;
    }
    if((event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) && event->modifiers()==Qt::NoModifier) {event->accept(); finish(); return;}
    InAppKeyEdit::keyPressEvent(event);
}
void LinuxShortcutEdit::keyReleaseEvent(QKeyEvent *event) {
    if(!recording_) {event->accept(); return;}
    InAppKeyEdit::keyReleaseEvent(event);
    if(recording_ && !(text().endsWith('+') && !text().endsWith("++"))) debounce_.start();
}
void LinuxShortcutEdit::captureFromDesktop(const QString &key) {if(recording_ && !key.isEmpty()) {setText(key.toLower()); debounce_.start();}}
void LinuxShortcutEdit::finish() {
    // 完整组合键在松键后提交；只有宿主确认写盘/桌面绑定成功才更新 committed value。
    // Commit after chord release; persistence and desktop acknowledgement remain the owner's responsibility.
    debounce_.stop(); if(!recording_) return;
    const auto value=text(); setRecording(false); clearFocus();
    if(value.endsWith('+') && !value.endsWith("++")) {setCommittedValue(before_); return;}
    if(value.compare(before_,Qt::CaseInsensitive)!=0) emit commitRequested(value);
}
