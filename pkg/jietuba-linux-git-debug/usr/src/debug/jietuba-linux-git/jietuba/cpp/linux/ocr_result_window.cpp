#include "ocr_result_window.h"
#include "tesseract_ocr.h"
#include "ppocr_wrapper.h"
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <utility>

LinuxOcrResultWindow::LinuxOcrResultWindow(const QImage &image, QWidget *parent,
                                         std::function<LinuxOcrResult(const QImage &)> recognizer, bool directCopy)
    : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("截图吧 — 文字识别"));
    setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
    resize(700, 500);
    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(QStringLiteral("正在识别选区文字…"), this);
    layout->addWidget(status_);
    text_ = new QPlainTextEdit(this);
    text_->setPlaceholderText(QStringLiteral("识别结果会显示在这里，也可以直接修改。"));
    text_->setReadOnly(true);
    layout->addWidget(text_, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    copy_ = new QPushButton(QStringLiteral("复制文字"), this);
    copy_->setEnabled(false);
    buttons->addWidget(copy_);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(text_->toPlainText());
        status_->setText(QStringLiteral("文字已复制到剪贴板。"));
    });
    connect(text_, &QPlainTextEdit::textChanged, this, [this] { copy_->setEnabled(!text_->toPlainText().trimmed().isEmpty()); });
    connect(&watcher_, &QFutureWatcher<LinuxOcrResult>::finished, this, [this,directCopy] {
        const auto result = watcher_.result();
        if (directCopy && result.error.isEmpty() && !result.text.trimmed().isEmpty()) { QApplication::clipboard()->setText(result.text); deleteLater(); return; }
        if (directCopy) show(); // 空结果和错误仍可查看。 / Surface empty results and errors.
        text_->setReadOnly(false);
        if (!result.error.isEmpty()) { status_->setText(result.error); return; }
        text_->setPlainText(result.text);
        status_->setText(result.text.isEmpty() ? QStringLiteral("选区中没有识别到文字。")
                                           : QStringLiteral("识别完成（%1），可修改或复制结果。").arg(result.language));
    });
    // 线程只持有 QImage 的共享副本；窗口销毁时 Qt 自动断开结果回调。
    // The worker owns a shared image copy; QObject disconnects its result on window destruction.
    watcher_.setFuture(QtConcurrent::run([image, recognizer = std::move(recognizer)] {
        return recognizer ? recognizer(image) : LinuxTesseractOcr::recognize(image);
    }));
}

QString LinuxOcrResultWindow::text() const { return text_->toPlainText(); }
