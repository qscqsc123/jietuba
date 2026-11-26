#pragma once
#include "ui/reorderable_rows.h"
#include "ui/toolbar_layout.h"
#include <QDialog>
#include <QIcon>
#include <QJsonArray>
#include <QMap>
#include <functional>

class QLabel;

namespace jietuba {
class ComboBox;

/**
 * 工具栏调整列表中的一行：拖动柄、原按钮图标、名称和显示模式。
 * 行控件本身随排序一起移动，保证下拉框状态不会因拖放丢失；两平台复用同一个实现。
 * A customization row with grip, original icon, name and display-mode selector.
 * Reordering moves the widget itself, preserving its selector state on both platforms.
 */
class ToolbarLayoutRow : public DraggableRow {
    Q_OBJECT
public:
    ToolbarLayoutRow(const QString &key, const QIcon &icon, QWidget *parent = nullptr);
    QString mode() const;
    void set_mode(const QString &mode);
    QString key;
    ComboBox *combo = nullptr;
};

/**
 * Windows/Linux 共用的工具栏调整窗口，编辑 [[按钮标识, 显示模式], ...]。
 * 支持拖动排序、常驻/更多/隐藏及恢复默认；确认等锁定项不出现在可编辑列表中。
 * 窗口只维护未提交的副本，取消不改变工具栏；调用者负责确认后的持久化与重新排版。
 * Shared Windows/Linux toolbar editor for button/mode pairs, reorder and reset.
 * Locked actions are excluded from editable rows. Changes stay local until the
 * caller accepts, persists and reapplies them; cancellation leaves the toolbar intact.
 */
class ToolbarLayoutDialog : public QDialog {
    Q_OBJECT
public:
    ToolbarLayoutDialog(const QJsonArray &layout, const QMap<QString, QIcon> &icons, QWidget *parent = nullptr,
                        const toolbar_layout::LayoutProfile &profile = toolbar_layout::SCREENSHOT);
    // The edited layout; locked buttons go back to their positions from when the dialog was filled.
    QJsonArray entries() const;
    // 返回空字符串表示提交成功；失败在面板内显示错误，不能再开一个被覆盖层挡住的模态窗。
    // An empty result commits; errors stay inline instead of opening another obscured modal window.
    void set_submit_handler(std::function<QString()> handler) { submit_handler_ = std::move(handler); }
    void accept() override;
#ifdef JIETUBA_LINUX_UI_PREVIEW
    // Linux 截图采用 layer-shell，调整内容必须作为同一原生表面的子控件显示。
    // Present inside the capture surface, without a separate xdg window or Qt window modality.
    void show_in_capture_overlay(QWidget *host);
    void fit_to_available_size(const QSize &available);
#endif
private:
    void fill(const QJsonArray &layout);
    // Shows every adjustable row; the list scrolls only past the screen's available height.
    void fit_height_to_rows();
    const toolbar_layout::LayoutProfile &profile_;
    ReorderableRowList *list_ = nullptr;
    QLabel *error_ = nullptr;
    std::function<QString()> submit_handler_;
    QMap<QString, ToolbarLayoutRow *> rows_;
    std::vector<std::pair<int, QString>> locked_slots_;
};
}
