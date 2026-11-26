#include "ui/toolbar_layout_dialog.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/toolbar_layout.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#ifdef JIETUBA_LINUX_UI_PREVIEW
#include <QKeyEvent>
#include <QPointer>
#include <QResizeEvent>
#endif

namespace jietuba {
namespace {
QString tld_tr(const char *text) { return i18n_tr(text, "ToolbarLayoutDialog"); }

// Toolbar tooltips carry shortcut hints and are too long for a list, so rows use short names.
const char *button_name(const QString &key) {
    static const QMap<QString, const char *> names = {
        {"long_screenshot", "Long screenshot"}, {"save", "Save"}, {"quick_save", "Quick save"}, {"copy", "Copy"}, {"screenshot_translate", "Screenshot translate"},
        {"text_recognize", "Recognize text"}, {"scan_code", "Scan code"}, {"gif", "Screen recording"},
        {"pen", "Pen"}, {"highlighter", "Highlighter"}, {"mosaic", "Mosaic"}, {"spotlight", "Spotlight"},
        {"watermark", "Watermark"}, {"arrow", "Arrow"}, {"number", "Number"}, {"rect", "Rectangle"},
        {"ellipse", "Ellipse"}, {"text", "Text"}, {"eraser", "Eraser"}, {"undo", "Undo"}, {"redo", "Redo"},
        {"cancel", "Cancel screenshot"}, {"pin", "Pin image"}, {"confirm", "Confirm"},
        {"annotation_pointer", "Pass through"}, {"annotation_clear", "Clear all"}, {"annotation_hide", "Hide drawings"},
        {"annotation_exit", "Exit annotation"}, {"annotation_board", "Board"}, {"annotation_cursor", "Cursor highlight"},
    };
    return names.value(key, "");
}

const std::pair<QString, const char *> MODE_NAMES[] = {
    {toolbar_layout::SHOW, "Always show"}, {toolbar_layout::MORE, "In more menu"}, {toolbar_layout::HIDE, "Always hide"}};

#ifdef JIETUBA_LINUX_UI_PREVIEW
/**
 * 截图表面内的调整面板容器：遮挡背景输入，限定面板尺寸并管理焦点恢复。
 * layer-shell 覆盖层独占键盘时，普通 xdg 模态窗会被它遮住，且 Qt 会阻止原截图输入。
 * 本容器和调整内容都只是子控件，不申请原生窗口，也不进入 Qt 模态窗口栈；
 * 关闭时恢复原本可用的背景控件和焦点，不改选区、标注或截图窗口角色。
 * Capture-local editor container with input shielding, size bounds and focus restoration.
 * A native modal xdg window can sit beneath the keyboard-exclusive layer-shell capture.
 * Both container and editor remain child widgets on that same surface, outside Qt's
 * modal stack. Closing restores previously enabled siblings and focus without touching capture state.
 */
class ToolbarLayoutOverlay final : public QWidget {
public:
    ToolbarLayoutOverlay(ToolbarLayoutDialog *dialog, QWidget *host) : QWidget(host), dialog_(dialog), previous_focus_(QApplication::focusWidget()) {
        setObjectName(QStringLiteral("linuxToolbarLayoutOverlay"));
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral("#linuxToolbarLayoutOverlay { background: rgba(0,0,0,90); }"));
        setFocusPolicy(Qt::StrongFocus);
        // 禁用同表面的背景控件，让 Tab/鼠标不能穿透；不要禁用宿主，否则子面板也会被禁用。
        // Disable background siblings for mouse/Tab isolation, never the host that owns this panel.
        for (auto *child : host->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child != this && child != dialog && child->isEnabled()) {
                disabled_.append(child);
                child->setEnabled(false);
            }
        }
        dialog->setParent(this, Qt::Widget);
        dialog->setWindowModality(Qt::NonModal);
        host->installEventFilter(this);
        setGeometry(host->rect());
        connect(dialog, &QDialog::finished, this, [this] {
            hide();
            for (const auto &child : disabled_) if (child) child->setEnabled(true);
            if (previous_focus_ && previous_focus_->isVisible() && previous_focus_->isEnabled()) previous_focus_->setFocus(Qt::OtherFocusReason);
            deleteLater();
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
        return QWidget::eventFilter(watched, event);
    }
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        dialog_->fit_to_available_size(size() - QSize(24, 24));
        dialog_->move((width() - dialog_->width()) / 2, (height() - dialog_->height()) / 2);
    }
    void mousePressEvent(QMouseEvent *) override { setFocus(Qt::MouseFocusReason); }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) dialog_->reject();
        // 背景区域接到的按键不能冒泡给画布，例如删除图形或撤销。
        // Do not bubble background keystrokes into canvas actions such as delete or undo.
        event->accept();
    }
private:
    ToolbarLayoutDialog *dialog_;
    QPointer<QWidget> previous_focus_;
    QList<QPointer<QWidget>> disabled_;
};
#endif
}

ToolbarLayoutRow::ToolbarLayoutRow(const QString &key, const QIcon &icon, QWidget *parent) : DraggableRow(parent), key(key) {
    // White backing: toolbar icons are dark glyphs drawn for a white toolbar and vanish on dark themes.
    auto *icon_label = new QLabel(this);
    icon_label->setFixedSize(dialog_scaled(28), dialog_scaled(28));
    icon_label->setAlignment(Qt::AlignCenter);
    icon_label->setPixmap(icon.pixmap(dialog_scaled(20), dialog_scaled(20)));
    set_own_style(icon_label, QString("background: #FFFFFF; border-radius: %1px;").arg(dialog_scaled(6)));
    combo = new ComboBox(this);
    configure_dialog_control(*combo);
    for (const auto &[mode, text] : MODE_NAMES) combo->addItem(tld_tr(text), mode);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(dialog_scaled(8), dialog_scaled(3), dialog_scaled(8), dialog_scaled(3));
    layout->setSpacing(dialog_scaled(10));
    layout->addWidget(grip);
    layout->addWidget(icon_label);
    auto *name_label = new BodyLabel(tld_tr(button_name(key)), this);
    configure_dialog_control(*name_label);
    layout->addWidget(name_label, 1);
    layout->addWidget(combo);
}

QString ToolbarLayoutRow::mode() const { return combo->currentData().toString(); }

void ToolbarLayoutRow::set_mode(const QString &mode) { combo->setCurrentIndex(combo->findData(mode)); }

ToolbarLayoutDialog::ToolbarLayoutDialog(const QJsonArray &layout, const QMap<QString, QIcon> &icons, QWidget *parent,
                                         const toolbar_layout::LayoutProfile &profile)
    : QDialog(parent), profile_(profile) {
    scale_dialog_font(*this);
    setWindowTitle(tld_tr("Customize Toolbar"));
    // Opened from the full-screen topmost capture window, so it stays on top too.
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
    setStyleSheet(QString("QDialog { background: %1; }").arg(ui_tokens(this).window));
    list_ = new ReorderableRowList(this);
    // Locked buttons get no disabled row; only adjustable buttons have one.
    for (const auto &key : profile.order)
        if (!profile.locked.contains(key)) rows_[key] = new ToolbarLayoutRow(key, icons.value(key));
    auto *hint = new CaptionLabel(tld_tr("Drag the handle to reorder. Use the drop-down to change visibility."), this);
    configure_dialog_control(*hint);
    auto *reset_btn = new TransparentPushButton(tld_tr("Restore defaults"), this);
    configure_dialog_control(*reset_btn);
    connect(reset_btn, &QPushButton::clicked, this, [this] { fill(toolbar_layout::default_layout(profile_)); });
    auto *ok_btn = new PrimaryPushButton(tld_tr("OK"), this);
    configure_dialog_control(*ok_btn);
    connect(ok_btn, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancel_btn = new PushButton(tld_tr("Cancel"), this);
    configure_dialog_control(*cancel_btn);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(reset_btn);
    buttons->addStretch(1);
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14));
    root->setSpacing(dialog_scaled(10));
    root->addWidget(list_, 1);
    root->addWidget(hint);
    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->hide();
    root->addWidget(error_);
    root->addLayout(buttons);
    fill(layout);
    fit_height_to_rows();
}

void ToolbarLayoutDialog::accept() {
    if (submit_handler_) {
        const QString error = submit_handler_();
        if (!error.isEmpty()) {
            error_->setText(error);
            error_->show();
#ifdef JIETUBA_LINUX_UI_PREVIEW
            if (!isWindow() && parentWidget()) {
                fit_to_available_size(parentWidget()->size() - QSize(24, 24));
                move((parentWidget()->width() - width()) / 2, (parentWidget()->height() - height()) / 2);
            }
#endif
            return;
        }
    }
    QDialog::accept();
}

#ifdef JIETUBA_LINUX_UI_PREVIEW
void ToolbarLayoutDialog::show_in_capture_overlay(QWidget *host) {
    // 在 winId()/open() 之前改成子控件；open() 会重新启用窗口模态，不能在这里调用。
    // Reparent before any winId()/open(); open() would reintroduce window modality.
    auto *overlay = new ToolbarLayoutOverlay(this, host);
    auto *header = new QHBoxLayout;
    auto *title = new BodyLabel(windowTitle(), this);
    configure_dialog_control(*title);
    header->addWidget(title, 1);
    auto *close = new TransparentPushButton(QStringLiteral("×"), this);
    close->setObjectName(QStringLiteral("toolbarLayoutClose"));
    close->setFixedWidth(dialog_scaled(32));
    configure_dialog_control(*close);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    header->addWidget(close);
    static_cast<QVBoxLayout *>(layout())->insertLayout(0, header);
    // 高度限制由容器统一计算，小屏幕滚动列表，底部确认/取消始终留在可见范围。
    // Bound the list to the host so OK/Cancel remain visible on smaller outputs.
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    fit_to_available_size(host->size() - QSize(24, 24));
    move((host->width() - width()) / 2, (host->height() - height()) / 2);
    overlay->show();
    overlay->raise();
    show();
    setFocusPolicy(Qt::StrongFocus);
    setFocus(Qt::OtherFocusReason);
}

void ToolbarLayoutDialog::fit_to_available_size(const QSize &available) {
    const int content_height = list_->content_height();
    list_->setFixedHeight(content_height);
    layout()->activate();
    const int non_list_height = sizeHint().height() - content_height;
    const int height = std::max(1, std::min(sizeHint().height(), available.height()));
    list_->setFixedHeight(std::max(0, height - non_list_height));
    resize(std::max(1, std::min(dialog_scaled(480), available.width())), height);
}
#endif

QJsonArray ToolbarLayoutDialog::entries() const {
    QJsonArray entries;
    for (auto *row : list_->rows()) {
        auto *layout_row = static_cast<ToolbarLayoutRow *>(row);
        entries.append(QJsonArray{layout_row->key, layout_row->mode()});
    }
    for (const auto &[index, key] : locked_slots_) entries.insert(std::min(index, static_cast<int>(entries.size())), QJsonArray{key, toolbar_layout::SHOW});
    return toolbar_layout::normalize_layout(entries, profile_);
}

void ToolbarLayoutDialog::fill(const QJsonArray &layout) {
    const auto normalized = toolbar_layout::normalize_layout(layout, profile_);
    locked_slots_.clear();
    int editable_index = 0;
    for (int index = 0; index < normalized.size(); ++index) {
        const auto entry = normalized[index].toArray();
        const auto key = entry[0].toString();
        if (profile_.locked.contains(key)) {
            locked_slots_.emplace_back(index, key);
            continue;
        }
        auto *row = rows_[key];
        list_->add_row(row, editable_index++);
        row->set_mode(entry[1].toString());
    }
}

void ToolbarLayoutDialog::fit_height_to_rows() {
    int content_height = list_->content_height();
    list_->setFixedHeight(content_height);
    int desired_height = sizeHint().height();
    auto *screen = parentWidget() ? parentWidget()->screen() : QApplication::primaryScreen();
    if (screen) {
        const int max_height = std::max(dialog_scaled(360), screen->availableGeometry().height() - dialog_scaled(40));
        if (desired_height > max_height) {
            const int non_list_height = desired_height - content_height;
            content_height = std::max(dialog_scaled(180), max_height - non_list_height);
            list_->setFixedHeight(content_height);
            desired_height = non_list_height + content_height;
        }
    }
    resize(dialog_scaled(480), desired_height);
}
}
