#pragma once
#include "tesseract_ocr.h"  // 复用 LinuxOcrResult 定义
#include <QImage>
#include <QString>
#include <atomic>

/**
 * Linux PP-OCR 包装器：使用 PaddleOCR v6 + ONNX Runtime 进行文字识别。
 * 与 Tesseract 共用相同的结果结构，便于在 UI 层统一处理。
 *
 * Linux PP-OCR wrapper using PaddleOCR v6 + ONNX Runtime for text recognition.
 * Shares the result structure with Tesseract for unified UI handling.
 */

class LinuxPpocrEngine {
public:
    /**
     * 识别图像中的文字。
     * @param image 要识别的图像
     * @param cancelled 可选的取消标志，支持实时中断
     * @return 识别结果，包含文字、语言和错误信息
     */
    static LinuxOcrResult recognize(const QImage &image, const std::atomic_bool *cancelled = nullptr);
    
    /**
     * 检查 PP-OCR 引擎是否可用（模型和运行时都存在）
     * @return true 如果可以使用 PP-OCR
     */
    static bool available();
    
    /**
     * 获取引擎状态信息
     * @return "ready" 表示就绪，否则返回错误描述
     */
    static QString status();
    
    /**
     * 检查 ONNX Runtime 是否可用
     * @return true 如果运行时已加载
     */
    static bool runtimeAvailable();
    
    /**
     * 检查模型文件是否存在
     * @return true 如果检测和识别模型都存在
     */
    static bool modelsAvailable();
};
