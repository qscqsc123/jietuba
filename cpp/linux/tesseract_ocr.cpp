#include "tesseract_ocr.h"
#include <QPainter>
#include <memory>
#include <vector>
#ifdef JIETUBA_HAS_TESSERACT
#include <tesseract/baseapi.h>
#endif

LinuxOcrResult LinuxTesseractOcr::recognize(const QImage &image, const QString &language) {
    if (image.isNull()) return {{}, {}, QStringLiteral("没有可识别的选区图片。")};
#ifndef JIETUBA_HAS_TESSERACT
    Q_UNUSED(language);
    return {{}, {}, QStringLiteral("此程序编译时未找到 Tesseract 开发包。安装后重新编译，即可使用文字识别。")};
#else
    // 每个任务创建独立的引擎实例，避免并发请求共享 Tesseract 可变状态。
    // Each worker owns its API instance; Tesseract's mutable recognition state is not shared.
    std::unique_ptr<tesseract::TessBaseAPI> api;
    QString selected;
    const QStringList candidates = language.isEmpty()
        ? QStringList{QStringLiteral("chi_sim+eng"), QStringLiteral("chi_sim"), QStringLiteral("eng")}
        : QStringList{language};
    for (const auto &candidate : candidates) {
        auto attempt = std::make_unique<tesseract::TessBaseAPI>();
        if (attempt->Init(nullptr, candidate.toUtf8().constData()) == 0) {
            api = std::move(attempt);
            // 组合语言可能只加载成功其中一个；显示实际载入的模型而非请求名。
            // A combined request may load only one model; report what Tesseract actually loaded.
            std::vector<std::string> loaded;
            api->GetLoadedLanguagesAsVector(&loaded);
            QStringList names;
            for (const auto &name : loaded) names.append(QString::fromStdString(name));
            selected = names.isEmpty() ? candidate : names.join(QLatin1Char('+'));
            break;
        }
    }
    if (!api)
        return {{}, {}, language.isEmpty()
            ? QStringLiteral("没有可用的中文或英文 Tesseract 语言数据。请安装 chi_sim 或 eng 语言包后重试。")
            : QStringLiteral("未安装 Tesseract %1 语言数据。").arg(language)};

    // 将透明区域铺白，避免透明像素转成黑色后干扰截图中的文字。
    // Flatten alpha over white so transparent pixels cannot become dark OCR noise.
    QImage rgb(image.size(), QImage::Format_RGB888);
    rgb.fill(Qt::white);
    { QPainter painter(&rgb); painter.drawImage(QPoint(0, 0), image); }
    api->SetImage(rgb.constBits(), rgb.width(), rgb.height(), 3, rgb.bytesPerLine());
    api->SetSourceResolution(96);
    std::unique_ptr<char[]> raw(api->GetUTF8Text());
    if (!raw) return {{}, selected, QStringLiteral("Tesseract 识别失败，请换一块更清晰的选区重试。")};
    return {QString::fromUtf8(raw.get()).trimmed(), selected, {}};
#endif
}
