#pragma once
#ifndef JIETUBA_LINUX_UI_PREVIEW
#include "ui/settings_ui/components.h"
// Windows 保留原有 SectionCard 类型，搜索/缩放等依赖其类型的逻辑无需改变。
// Keep the original Windows type so search and scaling retain their existing behavior.
namespace jietuba {using ShortcutSection=SectionCard;}
#else
#include "ui/fluent_lite/theme.h"
#include <QFrame>
class QVBoxLayout;
namespace jietuba {
/**
 * Windows/Linux 共用的快捷键分组卡片；标题与说明位于同一个圆角容器内部。
 * 不依赖平台热键钩子和设置存储，只处理主题绘制和行布局。
 * Shared Windows/Linux shortcut section with an internal title/caption and separated rows.
 * Theme painting and layout are independent of native hooks and persistence.
 */
class ShortcutSection final : public QFrame {
    Q_OBJECT
public:
    ShortcutSection(const IconSource &icon,const QString &title,const QString &caption,QWidget *parent=nullptr);
    void addRow(QWidget *widget);
    void addWidget(QWidget *widget);
private:
    void paintEvent(QPaintEvent *) override;
    QVBoxLayout *body_;
};
}
#endif
