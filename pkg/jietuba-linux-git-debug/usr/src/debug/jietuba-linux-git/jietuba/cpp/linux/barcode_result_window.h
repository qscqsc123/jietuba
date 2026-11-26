#pragma once

#include "barcode/reader.h"
#include <QFutureWatcher>
#include <QWidget>
#include <functional>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class BarcodeImagePreview;

struct LinuxBarcodeResult {
    std::vector<jietuba::DecodedCode> codes;
    QString error;
};

/**
 * Linux 扫码结果窗口：后台解码原始选区，按阅读顺序列出所有二维码和条码。
 * 预览图标出码的位置；选中的结果可以复制，HTTP(S) 地址可由用户主动打开。
 *
 * Independent scan result window. A worker decodes the base capture; the UI
 * shows every result with its outline and explicit copy/open-link actions.
 */
class LinuxBarcodeResultWindow final : public QWidget {
    Q_OBJECT
public:
    explicit LinuxBarcodeResultWindow(const QImage &image, QWidget *parent = nullptr,
        std::function<LinuxBarcodeResult(const QImage &)> decoder = {}, bool directCopy = false);
    bool isWorking() const { return watcher_.isRunning(); }
    QStringList decodedTexts() const;
private:
    void selectCode(int row);
    QFutureWatcher<LinuxBarcodeResult> watcher_;
    std::vector<jietuba::DecodedCode> codes_;
    QLabel *status_ = nullptr;
    BarcodeImagePreview *preview_ = nullptr;
    QListWidget *list_ = nullptr;
    QPushButton *copy_ = nullptr;
    QPushButton *open_ = nullptr;
};
