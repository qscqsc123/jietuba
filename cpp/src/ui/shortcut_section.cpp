#include "shortcut_section.h"
#include "ui/fluent_lite/cards.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#ifdef JIETUBA_LINUX_UI_PREVIEW
namespace jietuba {
ShortcutSection::ShortcutSection(const IconSource &icon,const QString &title,const QString &caption,QWidget *parent):QFrame(parent) {
    setFrameShape(QFrame::NoFrame); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    body_=new QVBoxLayout(this); body_->setContentsMargins(dialog_scaled(20),0,dialog_scaled(20),dialog_scaled(4)); body_->setSpacing(0);
    auto *header=new QWidget(this); header->setMinimumHeight(dialog_scaled(50));
    auto *row=new QHBoxLayout(header); row->setContentsMargins(0,0,0,0); row->setSpacing(dialog_scaled(10));
    row->addWidget(new IconBadge(icon,QString(),header,20,19,false));
    auto *name=new QLabel(title,header),*hint=new QLabel(caption,header);
    hint->setWordWrap(true); hint->setMinimumWidth(0); hint->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    row->addWidget(name); row->addWidget(hint,1); body_->addWidget(header);
    const auto refresh=[this,name,hint] {
        const auto &t=ui_tokens(this);
        name->setStyleSheet(QString("background:transparent;color:%1;font-size:%2px;font-weight:600;").arg(t.text).arg(dialog_scaled(15)));
        hint->setStyleSheet(QString("background:transparent;color:%1;font-size:%2px;").arg(t.text_muted).arg(dialog_scaled(12)));
        update();
    };
    connect(get_ui_theme(),&UIThemeManager::theme_changed,this,refresh); refresh();
}
void ShortcutSection::addRow(QWidget *widget) {
    auto *line=new QFrame(this); line->setFixedHeight(1);
    const auto refresh=[this,line] {line->setStyleSheet(QString("background:%1;border:none;").arg(ui_tokens(this).border));};
    connect(get_ui_theme(),&UIThemeManager::theme_changed,line,refresh); refresh(); body_->addWidget(line); body_->addWidget(widget);
}
void ShortcutSection::addWidget(QWidget *widget) {body_->addWidget(widget);}
void ShortcutSection::paintEvent(QPaintEvent *) {
    const auto &t=ui_tokens(this); QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(css_color(t.border)); painter.setBrush(css_color(t.surface));
    const int radius=dialog_scaled(15); painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),radius,radius);
}
}
#endif
