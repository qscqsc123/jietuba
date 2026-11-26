#pragma once

#include "tesseract_ocr.h"
#include <QFutureWatcher>
#include <QWidget>
#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;

/**
 * Linux OCR 结果窗口：截图界面关闭后独立存活，并在后台线程识别选区底图。
 * 结果可以直接编辑、复制；关闭窗口不会把后台任务的结果送到已销毁的控件。
 *
 * A standalone OCR result window. It starts recognition on a worker after the
 * capture closes, lets the user edit/copy text, and receives results only while alive.
 */
class LinuxOcrResultWindow final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxOcrResultWindow(const QImage &image, QWidget *parent = nullptr,
                                  std::function<LinuxOcrResult(const QImage &)> recognizer = {}, bool directCopy = false);
    QString text() const;
    bool isWorking() const { return watcher_.isRunning(); }
private:
    QFutureWatcher<LinuxOcrResult> watcher_;
    QLabel *status_ = nullptr;
    QPlainTextEdit *text_ = nullptr;
    QPushButton *copy_ = nullptr;
};
