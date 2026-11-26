#include "tesseract_ocr.h"
#include <QFont>
#include <QPainter>
#include <QtTest>

class TesseractOcrTest final : public QObject {
    Q_OBJECT
private slots:
    void rejectsEmptyImage() {
        QVERIFY(!LinuxTesseractOcr::recognize({}).error.isEmpty());
    }
    void recognizesRenderedText() {
#ifndef JIETUBA_HAS_TESSERACT
        QSKIP("Tesseract development package is not installed");
#else
        // 用装有的 Afrikaans 模型读拉丁文字；其他机器没有该模型时跳过真实识别。
        // Use the locally installed Afrikaans model for Latin text, skipping when absent.
        QImage image(700, 160, QImage::Format_RGB32); image.fill(Qt::white);
        {
            QPainter painter(&image);
            QFont font(QStringLiteral("DejaVu Sans")); font.setPixelSize(80); font.setBold(true);
            painter.setFont(font); painter.setPen(Qt::black);
            painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("HELLO"));
        }
        const auto result = LinuxTesseractOcr::recognize(image, QStringLiteral("afr"));
        if (!result.error.isEmpty() && result.error.contains(QStringLiteral("未安装"))) QSKIP("Afrikaans model is not installed");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.text.contains(QStringLiteral("HELLO"), Qt::CaseInsensitive), qPrintable(result.text));
        // 默认中英双语模型也必须能处理相同图片；这覆盖截图翻译的真实入口配置。
        // The default Chinese-plus-English path must recognize the same screenshot text.
        const auto mixed = LinuxTesseractOcr::recognize(image);
        if (mixed.error.contains(QStringLiteral("没有可用"))) QSKIP("Chinese and English models are not installed");
        QVERIFY2(mixed.error.isEmpty(), qPrintable(mixed.error));
        QVERIFY2(mixed.text.contains(QStringLiteral("HELLO"), Qt::CaseInsensitive), qPrintable(mixed.text));
#endif
    }
};

QTEST_MAIN(TesseractOcrTest)
#include "tesseract_ocr_test.moc"
