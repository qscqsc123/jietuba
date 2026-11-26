#pragma once

#include <QImage>
#include <QString>

/**
 * Linux OCR 后端的结果。识别失败与“没有文字”分开表示，便于界面给出准确提示。
 * The Linux OCR result separates an engine failure from a valid empty recognition.
 */
struct LinuxOcrResult {
    QString text;
    QString language;
    QString error;
};

/**
 * 使用系统 Tesseract 在调用线程识别截图，不访问 Wayland 或 Qt 界面控件。
 * 默认优先中英双语，缺少其中一种时回退到已安装的语言；开发包缺失仍能编译应用。
 *
 * Thread-local Tesseract adapter. It reads only a QImage, prefers Chinese plus
 * English and falls back to one installed language. The build also works without
 * Tesseract development files, returning an actionable error instead.
 */
class LinuxTesseractOcr {
public:
    static LinuxOcrResult recognize(const QImage &image, const QString &language = {});
};
