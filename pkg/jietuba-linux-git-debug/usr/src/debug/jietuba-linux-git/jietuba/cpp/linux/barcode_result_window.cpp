#include "barcode_result_window.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <exception>
#include <utility>

namespace {
QUrl safeWebLink(const QString &text) {
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    return url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        && !url.host().isEmpty() ? url : QUrl();
}
}

/**
 * 原图预览仅画识别边框，不修改扫码输入或被复制的文字。
 * Preview the original image with non-destructive code outlines.
 */
class BarcodeImagePreview final : public QWidget {
public:
    explicit BarcodeImagePreview(const QImage &image, QWidget *parent = nullptr) : QWidget(parent), image_(image) { setMinimumSize(280, 240); }
    void setCodes(const std::vector<jietuba::DecodedCode> &codes, int selected) { codes_ = codes; selected_ = selected; update(); }
    void setSelected(int row) { selected_ = row; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#202838")));
        if (image_.isNull()) return;
        const double scale = std::min({1.0, double(width()) / image_.width(), double(height()) / image_.height()});
        const QRectF target((width() - image_.width() * scale) / 2.0, (height() - image_.height() * scale) / 2.0,
                            image_.width() * scale, image_.height() * scale);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, image_);
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < static_cast<int>(codes_.size()); ++i) {
            painter.setPen(QPen(QColor(QStringLiteral("#40e0d0")), i == selected_ ? 3 : 2));
            painter.setBrush(Qt::NoBrush);
            QPolygonF polygon;
            for (const auto &point : codes_[i].outline)
                polygon << QPointF(target.left() + point.x() * scale, target.top() + point.y() * scale);
            painter.drawPolygon(polygon);
        }
    }
private:
    QImage image_;
    std::vector<jietuba::DecodedCode> codes_;
    int selected_ = -1;
};

LinuxBarcodeResultWindow::LinuxBarcodeResultWindow(const QImage &image, QWidget *parent,
    std::function<LinuxBarcodeResult(const QImage &)> decoder, bool directCopy) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("截图吧 — 扫码结果"));
    setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
    resize(900, 550);
    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(QStringLiteral("正在扫描选区…"), this);
    layout->addWidget(status_);
    auto *splitter = new QSplitter(this);
    preview_ = new BarcodeImagePreview(image, splitter);
    list_ = new QListWidget(splitter);
    list_->setMinimumWidth(300);
    splitter->addWidget(preview_); splitter->addWidget(list_);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    copy_ = new QPushButton(QStringLiteral("复制内容"), this); copy_->setEnabled(false);
    open_ = new QPushButton(QStringLiteral("打开链接"), this); open_->setEnabled(false);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    buttons->addWidget(copy_); buttons->addWidget(open_); buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(list_, &QListWidget::currentRowChanged, this, &LinuxBarcodeResultWindow::selectCode);
    connect(copy_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(codes_.size())) return;
        QApplication::clipboard()->setText(codes_[row].text);
        status_->setText(QStringLiteral("内容已复制到剪贴板。"));
    });
    connect(open_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(codes_.size())) return;
        const QUrl url = safeWebLink(codes_[row].text);
        if (!url.isEmpty()) QDesktopServices::openUrl(url);
    });
    connect(&watcher_, &QFutureWatcher<LinuxBarcodeResult>::finished, this, [this,directCopy] {
        const auto result = watcher_.result();
        if (directCopy && result.error.isEmpty() && result.codes.size()==1 && !result.codes.front().text.isEmpty()) { QApplication::clipboard()->setText(result.codes.front().text); deleteLater(); return; }
        if (directCopy) show(); // 空结果和错误仍可查看。 / Surface empty results and errors.
        if (!result.error.isEmpty()) { status_->setText(result.error); return; }
        codes_ = result.codes;
        for (const auto &code : codes_) list_->addItem(code.format_name + QStringLiteral("  ") + code.text);
        preview_->setCodes(codes_, codes_.empty() ? -1 : 0);
        status_->setText(codes_.empty() ? QStringLiteral("没有找到二维码或条码，请将完整图案框入选区。")
                                        : QStringLiteral("找到 %1 个二维码或条码。").arg(codes_.size()));
        if (!codes_.empty()) list_->setCurrentRow(0);
    });
    watcher_.setFuture(QtConcurrent::run([image, decoder = std::move(decoder)] {
        if (decoder) return decoder(image);
#ifdef JIETUBA_HAS_ZXING
        try { return LinuxBarcodeResult{jietuba::read_codes(image), {}}; }
        catch (const std::exception &e) { return LinuxBarcodeResult{{}, QString::fromUtf8(e.what())}; }
#else
        return LinuxBarcodeResult{{}, QStringLiteral("此程序编译时未找到 ZXing。安装 ZXing 开发包并重新编译后即可扫码。")};
#endif
    }));
}

void LinuxBarcodeResultWindow::selectCode(int row) {
    const bool valid = row >= 0 && row < static_cast<int>(codes_.size());
    preview_->setSelected(valid ? row : -1);
    copy_->setEnabled(valid);
    open_->setEnabled(valid && !safeWebLink(codes_[row].text).isEmpty());
}

QStringList LinuxBarcodeResultWindow::decodedTexts() const {
    QStringList texts;
    for (const auto &code : codes_) texts.append(code.text);
    return texts;
}
