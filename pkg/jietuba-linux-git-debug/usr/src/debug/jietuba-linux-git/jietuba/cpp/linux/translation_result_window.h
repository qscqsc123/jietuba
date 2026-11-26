#pragma once

#include "tesseract_ocr.h"
#include "translation_client.h"
#include <QFutureWatcher>
#include <QWidget>
#include <functional>

class LinuxSettings;
class QLabel;
class QPlainTextEdit;
class QPushButton;

/**
 * 截图翻译窗口：先在后台 OCR 选区底图，再向设置中选定的服务提交文字。
 * 原文、译文都可以复制；缺少密钥或服务地址时显示设置入口，不发送网络请求。
 *
 * Standalone screenshot translation flow: background OCR followed by an
 * explicitly configured HTTP provider. Missing configuration never sends data.
 */
class LinuxTranslationResultWindow final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxTranslationResultWindow(const QImage &image, LinuxSettings &settings, QWidget *parent = nullptr,
                                          std::function<LinuxOcrResult(const QImage &)> recognizer = {});
    explicit LinuxTranslationResultWindow(const QString &text,LinuxSettings &settings,QWidget *parent=nullptr);
    QString sourceText() const;
    QString translatedText() const;
    bool isRecognizing() const { return ocr_.isRunning(); }
private:
    void translate();
    LinuxSettings &settings_;
    QFutureWatcher<LinuxOcrResult> ocr_;
    LinuxTranslationClient client_;
    QLabel *status_ = nullptr;
    QPlainTextEdit *source_ = nullptr;
    QPlainTextEdit *translated_ = nullptr;
    QPushButton *translateButton_ = nullptr;
    QPushButton *copyButton_ = nullptr;
};
