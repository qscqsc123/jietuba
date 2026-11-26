#include "ppocr_wrapper.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <dlfcn.h>

// PP-OCR C API 函数指针类型定义
typedef void* JbPpocr;
typedef bool (*jb_ppocr_init_runtime_fn)(const char *path, char *error, size_t error_len);
typedef JbPpocr* (*jb_ppocr_open_fn)(const char *det, const char *rec, const char *dict, char *error, size_t error_len);
typedef void (*jb_ppocr_free_fn)(JbPpocr *engine);
typedef void (*jb_ppocr_close_fn)(JbPpocr *engine);

struct JbPpocrLine {
    char *text;
    float score;
    int32_t *box_x;
    int32_t *box_y;
    uint32_t box_points;
};

typedef bool (*jb_ppocr_recognize_fn)(JbPpocr *engine, const uint8_t *rgb, size_t len, 
                                     uint32_t w, uint32_t h, uint32_t stride,
                                     bool (*cancel_cb)(void*), void *cancel_data,
                                     JbPpocrLine **lines, size_t *count,
                                     char *error, size_t error_len);
typedef void (*jb_ppocr_lines_free_fn)(JbPpocrLine *lines, size_t count);

// 全局函数指针
static void *ppocr_lib = nullptr;
static jb_ppocr_init_runtime_fn ppocr_init_runtime = nullptr;
static jb_ppocr_open_fn ppocr_open = nullptr;
static jb_ppocr_free_fn ppocr_free = nullptr;
static jb_ppocr_close_fn ppocr_close = nullptr;
static jb_ppocr_recognize_fn ppocr_recognize = nullptr;
static jb_ppocr_lines_free_fn ppocr_lines_free = nullptr;

// 全局引擎实例（延迟初始化）
static JbPpocr *global_engine = nullptr;
static QString last_error;

static QString modelsRoot() {
    // 按优先级查找模型目录
    QStringList candidates;
    
    // 1. 与可执行文件同级的 models 目录（开发环境）
    candidates << QCoreApplication::applicationDirPath() + "/models";
    candidates << QCoreApplication::applicationDirPath() + "/../models";
    
    // 2. 系统数据目录（安装后）
    candidates << "/usr/share/jietuba/models";
    candidates << "/usr/local/share/jietuba/models";
    
    // 3. 用户数据目录
    candidates << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/models";
    
    for (const QString &path : candidates) {
        if (QDir(path).exists()) return path;
    }
    
    return candidates.first(); // 返回首选路径，即使不存在
}

static QString runtimePath() {
    QString root = modelsRoot();
    QStringList candidates = {
        root + "/libonnxruntime.so",
        root + "/onnxruntime.so",
        "/usr/lib/libonnxruntime.so",
        "/usr/local/lib/libonnxruntime.so"
    };
    
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) return path;
    }
    return candidates.first();
}

static bool loadPpocrLibrary() {
    if (ppocr_lib) return true;
    
    // 查找 libppocr_rust 库
    QStringList libCandidates = {
        QCoreApplication::applicationDirPath() + "/libppocr_rust.so",
        "/usr/lib/jietuba/libppocr_rust.so",
        "/usr/local/lib/jietuba/libppocr_rust.so"
    };
    
    QString libPath;
    for (const QString &path : libCandidates) {
        if (QFileInfo::exists(path)) {
            libPath = path;
            break;
        }
    }
    
    if (libPath.isEmpty()) {
        last_error = QStringLiteral("找不到 PP-OCR 库文件");
        return false;
    }
    
    ppocr_lib = dlopen(libPath.toUtf8().constData(), RTLD_LAZY);
    if (!ppocr_lib) {
        last_error = QStringLiteral("加载 PP-OCR 库失败: %1").arg(QString::fromUtf8(dlerror()));
        return false;
    }
    
    // 加载函数指针
    ppocr_init_runtime = reinterpret_cast<jb_ppocr_init_runtime_fn>(dlsym(ppocr_lib, "jb_ppocr_init_runtime"));
    ppocr_open = reinterpret_cast<jb_ppocr_open_fn>(dlsym(ppocr_lib, "jb_ppocr_open"));
    ppocr_free = reinterpret_cast<jb_ppocr_free_fn>(dlsym(ppocr_lib, "jb_ppocr_free"));
    ppocr_close = reinterpret_cast<jb_ppocr_close_fn>(dlsym(ppocr_lib, "jb_ppocr_close"));
    ppocr_recognize = reinterpret_cast<jb_ppocr_recognize_fn>(dlsym(ppocr_lib, "jb_ppocr_recognize"));
    ppocr_lines_free = reinterpret_cast<jb_ppocr_lines_free_fn>(dlsym(ppocr_lib, "jb_ppocr_lines_free"));
    
    if (!ppocr_init_runtime || !ppocr_open || !ppocr_free || !ppocr_close || !ppocr_recognize || !ppocr_lines_free) {
        last_error = QStringLiteral("PP-OCR 库缺少必要的符号");
        dlclose(ppocr_lib);
        ppocr_lib = nullptr;
        return false;
    }
    
    return true;
}

bool LinuxPpocrEngine::runtimeAvailable() {
    return QFileInfo::exists(runtimePath());
}

bool LinuxPpocrEngine::modelsAvailable() {
    QString root = modelsRoot();
    return QFileInfo::exists(root + "/PP-OCRv6_det_small.onnx") &&
           QFileInfo::exists(root + "/PP-OCRv6_rec_small.onnx") &&
           QFileInfo::exists(root + "/ppocr_keys_v1.txt");
}

bool LinuxPpocrEngine::available() {
    return runtimeAvailable() && modelsAvailable() && loadPpocrLibrary();
}

QString LinuxPpocrEngine::status() {
    if (!runtimeAvailable()) {
        return QStringLiteral("missing_runtime");
    }
    if (!modelsAvailable()) {
        return QStringLiteral("missing_models");
    }
    if (!loadPpocrLibrary()) {
        return last_error;
    }
    return QStringLiteral("ready");
}

static bool initEngine() {
    if (global_engine) return true;
    
    if (!loadPpocrLibrary()) return false;
    
    // 初始化 ONNX Runtime
    char error[512] = {0};
    QString runtime = runtimePath();
    if (!ppocr_init_runtime(runtime.toUtf8().constData(), error, sizeof(error))) {
        last_error = QStringLiteral("初始化 ONNX Runtime 失败: %1").arg(QString::fromUtf8(error));
        return false;
    }
    
    // 加载模型
    QString root = modelsRoot();
    QString detModel = root + "/PP-OCRv6_det_small.onnx";
    QString recModel = root + "/PP-OCRv6_rec_small.onnx";
    QString dict = root + "/ppocr_keys_v1.txt";
    
    global_engine = ppocr_open(detModel.toUtf8().constData(),
                              recModel.toUtf8().constData(),
                              dict.toUtf8().constData(),
                              error, sizeof(error));
    
    if (!global_engine) {
        last_error = QStringLiteral("加载 PP-OCR 模型失败: %1").arg(QString::fromUtf8(error));
        return false;
    }
    
    return true;
}

static bool cancelCallback(void *data) {
    auto *cancelled = static_cast<const std::atomic_bool*>(data);
    return cancelled && cancelled->load();
}

LinuxOcrResult LinuxPpocrEngine::recognize(const QImage &image, const std::atomic_bool *cancelled) {
    if (image.isNull()) {
        return {{}, {}, QStringLiteral("没有可识别的选区图片。")};
    }
    
    if (!available()) {
        QString statusMsg = status();
        if (statusMsg == "missing_runtime") {
            return {{}, {}, QStringLiteral("PP-OCR 需要 ONNX Runtime 1.24+。请安装 onnxruntime 包。")};
        } else if (statusMsg == "missing_models") {
            return {{}, {}, QStringLiteral("PP-OCR 模型文件未找到。请下载模型到 %1").arg(modelsRoot())};
        } else {
            return {{}, {}, QStringLiteral("PP-OCR 不可用: %1").arg(statusMsg)};
        }
    }
    
    if (!initEngine()) {
        return {{}, {}, last_error};
    }
    
    // 转换为 RGB888 格式
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        return {{}, {}, QStringLiteral("图像格式转换失败。")};
    }
    
    // 调用 PP-OCR 识别
    JbPpocrLine *lines = nullptr;
    size_t count = 0;
    char error[512] = {0};
    
    bool success = ppocr_recognize(global_engine,
                                   rgb.constBits(),
                                   rgb.sizeInBytes(),
                                   rgb.width(),
                                   rgb.height(),
                                   rgb.bytesPerLine(),
                                   cancelled ? cancelCallback : nullptr,
                                   const_cast<void*>(static_cast<const void*>(cancelled)),
                                   &lines,
                                   &count,
                                   error,
                                   sizeof(error));
    
    if (!success) {
        if (cancelled && cancelled->load()) {
            return {{}, {}, QStringLiteral("已取消")};
        }
        return {{}, {}, QStringLiteral("PP-OCR 识别失败: %1").arg(QString::fromUtf8(error))};
    }
    
    // 组合识别结果
    QStringList textParts;
    for (size_t i = 0; i < count; ++i) {
        if (lines[i].text) {
            textParts << QString::fromUtf8(lines[i].text);
        }
    }
    
    QString result = textParts.join('\n');
    
    // 释放内存
    if (lines) {
        ppocr_lines_free(lines, count);
    }
    
    return {result, QStringLiteral("PP-OCR v6"), {}};
}
