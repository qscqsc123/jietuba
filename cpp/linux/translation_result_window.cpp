#include "translation_result_window.h"
#include "linux_settings.h"
#include "settings_window.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <utility>

LinuxTranslationResultWindow::LinuxTranslationResultWindow(const QImage &image, LinuxSettings &settings, QWidget *parent,
    std::function<LinuxOcrResult(const QImage &)> recognizer)
    : QWidget(parent), settings_(settings), client_(this) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("截图吧 — 截图翻译"));
    resize(950, 600);
    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(QStringLiteral("正在识别选区文字…"), this);
    layout->addWidget(status_);
    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("目标语言："), this));
    auto *target = new QComboBox(this);
    target->addItem(QStringLiteral("简体中文"), QStringLiteral("zh")); target->addItem(QStringLiteral("英语"), QStringLiteral("en"));
    target->addItem(QStringLiteral("日语"), QStringLiteral("ja")); target->addItem(QStringLiteral("韩语"), QStringLiteral("ko"));
    target->setCurrentIndex(target->findData(settings_.translationTarget()));
    controls->addWidget(target);
    translateButton_ = new QPushButton(QStringLiteral("翻译 / 重试"), this); translateButton_->setEnabled(false);
    controls->addWidget(translateButton_);
    auto *configure = new QPushButton(QStringLiteral("翻译设置"), this);
    controls->addWidget(configure); controls->addStretch();
    layout->addLayout(controls);
    auto *splitter = new QSplitter(this);
    source_ = new QPlainTextEdit(splitter); source_->setPlaceholderText(QStringLiteral("OCR 原文（可修改）"));
    source_->setReadOnly(true);
    translated_ = new QPlainTextEdit(splitter); translated_->setPlaceholderText(QStringLiteral("译文"));
    splitter->addWidget(source_); splitter->addWidget(translated_);
    layout->addWidget(splitter, 1);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    copyButton_ = new QPushButton(QStringLiteral("复制译文"), this); copyButton_->setEnabled(false);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    buttons->addWidget(copyButton_); buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] {
        translateButton_->setEnabled(!source_->toPlainText().trimmed().isEmpty() && !client_.isWorking());
    });
    connect(translated_, &QPlainTextEdit::textChanged, this, [this] { copyButton_->setEnabled(!translated_->toPlainText().trimmed().isEmpty()); });
    connect(target, &QComboBox::currentIndexChanged, this, [this, target] {
        settings_.setTranslationTarget(target->currentData().toString()); settings_.sync();
    });
    connect(translateButton_, &QPushButton::clicked, this, &LinuxTranslationResultWindow::translate);
    connect(copyButton_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(translated_->toPlainText()); status_->setText(QStringLiteral("译文已复制到剪贴板。"));
    });
    connect(configure, &QPushButton::clicked, this, [this] {
        for (auto *window : QApplication::topLevelWidgets()) {
            if (auto *settings = qobject_cast<LinuxSettingsWindow *>(window)) { settings->showTranslationPage(); return; }
        }
        status_->setText(QStringLiteral("请从托盘打开设置 → 翻译页。"));
    });
    connect(&client_, &LinuxTranslationClient::finished, this, [this](const LinuxTranslationResult &result) {
        translateButton_->setEnabled(!source_->toPlainText().trimmed().isEmpty());
        if (!result.error.isEmpty()) { status_->setText(result.error); return; }
        translated_->setPlainText(result.text);
        status_->setText(result.detectedLanguage.isEmpty() ? QStringLiteral("翻译完成。")
                                                         : QStringLiteral("翻译完成，原文语言：%1。").arg(result.detectedLanguage));
    });
    connect(&ocr_, &QFutureWatcher<LinuxOcrResult>::finished, this, [this] {
        const auto result = ocr_.result();
        source_->setReadOnly(false);
        if (!result.error.isEmpty()) { status_->setText(result.error); return; }
        if (result.text.isEmpty()) { status_->setText(QStringLiteral("选区中没有识别到文字。")); return; }
        source_->setPlainText(result.text);
        translate();
    });
    // OCR 只持有原图副本；网络请求在用户配置有效时才会由当前窗口发出。
    // OCR owns an image copy; this window sends text only after validating the chosen provider.
    // 无图像时作为完整文字翻译窗口，不误启 OCR，也不自动发送输入文字。
    // Text-only mode skips OCR and waits for explicit translation.
    if(image.isNull()) {source_->setReadOnly(false); source_->setPlaceholderText(QStringLiteral("输入要翻译的文字…")); status_->setText(QStringLiteral("输入文字后点击翻译。")); return;}
    ocr_.setFuture(QtConcurrent::run([image, recognizer = std::move(recognizer)] {
        return recognizer ? recognizer(image) : LinuxTesseractOcr::recognize(image);
    }));
}

LinuxTranslationResultWindow::LinuxTranslationResultWindow(const QString &text,LinuxSettings &settings,QWidget *parent)
    :LinuxTranslationResultWindow(QImage(),settings,parent) {source_->setPlainText(text); setWindowTitle(QStringLiteral("截图吧 — 翻译"));}

void LinuxTranslationResultWindow::translate() {
    const LinuxTranslationConfig config{settings_.translationProvider(), settings_.translationApiKey(),
                                        settings_.translationEndpoint(), settings_.translationTarget(),
                                        settings_.googleApiKey(), settings_.youdaoAppId(), settings_.youdaoAppSecret()};
    const auto prepared = LinuxTranslationClient::prepare(source_->toPlainText(), config);
    if (!prepared.error.isEmpty()) { status_->setText(prepared.error); return; }
    translateButton_->setEnabled(false);
    status_->setText(QStringLiteral("正在翻译…"));
    client_.translate(source_->toPlainText(), config);
}

QString LinuxTranslationResultWindow::sourceText() const { return source_->toPlainText(); }
QString LinuxTranslationResultWindow::translatedText() const { return translated_->toPlainText(); }
