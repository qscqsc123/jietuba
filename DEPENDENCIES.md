# jietuba-linux 组件依赖说明

## 📦 依赖组件详解

### 必需依赖（REQUIRED）

| 组件 | Arch 包名 | 版本要求 | 用途 |
|------|----------|---------|------|
| **Qt6 Core** | `qt6-base` | >= 6.8 | 核心框架、GUI、网络 |
| **Qt6 SVG** | `qt6-svg` | >= 6.8 | SVG 图标渲染 |
| **Qt6 Multimedia** | `qt6-multimedia` | >= 6.8 | 音频录制支持 |
| **GStreamer** | `gstreamer` | 1.0 | 视频流处理 |
| **GStreamer Plugins** | `gst-plugins-base` | - | 基础编解码器 |
| | `gst-plugins-good` | - | 额外编解码器 |
| | `gst-plugin-pipewire` | - | PipeWire 视频流 |
| **Wayland** | `wayland` | - | Wayland 协议支持 |
| **X11 库** | `libx11` | - | X11 截图支持 |
| | `libxfixes` | - | X11 光标捕获 |
| | `libxrandr` | - | X11 多显示器 |

### 可选依赖（OPTIONAL）

| 组件 | Arch 包名 | 用途 | 影响 |
|------|----------|------|------|
| **ONNX Runtime** | `onnxruntime` | 高精度 OCR (PP-OCR) | ⭐ **强烈推荐** - 中文识别精度 60% → 95% |
| **Tesseract** | `tesseract` | 基础 OCR | 回退方案，精度较低 |
| | `tesseract-data-eng` | 英文识别 | 必需（Tesseract） |
| | `tesseract-data-chi_sim` | 中文简体识别 | 推荐（Tesseract） |
| **ZXing** | `zxing-cpp` | 二维码/条形码识别 | 无此包时扫码按钮不可用 |
| **LayerShell** | `layer-shell-qt` | Wayland 贴图精确定位 | 增强 Wayland 体验 |
| **Qt6 TTS** | `qt6-speech` | 文字转语音 | 朗读识别结果 |
| **Speech Dispatcher** | `speech-dispatcher` | 备选 TTS 后端 | 朗读识别结果 |
| **AT-SPI2** | `at-spi2-core` | 智能元素检测 | 自动识别 UI 元素 |

---

## 🎯 推荐安装方案

### 最小安装（基础功能）
```bash
sudo pacman -S qt6-base qt6-svg qt6-multimedia \
               gstreamer gst-plugins-base gst-plugins-good gst-plugin-pipewire \
               wayland libx11 libxfixes libxrandr
```

### 推荐安装（完整功能）
```bash
sudo pacman -S qt6-base qt6-svg qt6-multimedia \
               gstreamer gst-plugins-base gst-plugins-good gst-plugin-pipewire \
               wayland libx11 libxfixes libxrandr \
               onnxruntime tesseract tesseract-data-eng tesseract-data-chi_sim \
               zxing-cpp layer-shell-qt qt6-speech
```

### 完整安装（所有功能）
```bash
sudo pacman -S qt6-base qt6-svg qt6-multimedia \
               gstreamer gst-plugins-base gst-plugins-good gst-plugin-pipewire \
               wayland libx11 libxfixes libxrandr \
               onnxruntime tesseract tesseract-data-eng tesseract-data-chi_sim \
               zxing-cpp layer-shell-qt qt6-speech speech-dispatcher at-spi2-core
```

---

## 📋 AUR 自动依赖处理

在 `PKGBUILD` 中，依赖通过以下字段声明：

### depends=()（必需依赖）
```bash
depends=(
    'qt6-base>=6.8'
    'qt6-svg'
    'qt6-multimedia'
    'gstreamer'
    'gst-plugins-base'
    'gst-plugins-good'
    'gst-plugin-pipewire'
    'wayland'
    'libx11'
    'libxfixes'
    'libxrandr'
    'tesseract'
    'tesseract-data-eng'
    'tesseract-data-chi_sim'
    'zxing-cpp'
)
```

**AUR 行为：** 
- ✅ 安装时会**自动检查**这些包
- ✅ 缺失的包会**自动安装**
- ❌ 如果某个包不存在会**中止安装**

### optdepends=()（可选依赖）
```bash
optdepends=(
    'onnxruntime: High-accuracy OCR with PP-OCR (strongly recommended)'
    'layer-shell-qt: Wayland layer-shell support for pins'
    'qt6-speech: Text-to-speech support'
    'speech-dispatcher: Alternative TTS backend'
    'at-spi2-core: Smart element detection'
)
```

**AUR 行为：**
- ℹ️ 安装时会**显示提示**
- ⚠️ **不会自动安装**
- ✅ 用户可以**稍后手动安装**
- ✅ 程序会**自动检测**并启用对应功能

### makedepends=()（构建依赖）
```bash
makedepends=(
    'git'
    'cmake>=3.24'
    'cargo'
    'wayland-protocols'
    'qt6-tools'
)
```

**AUR 行为：**
- ✅ 编译时**自动安装**
- ✅ 编译完成后可以**卸载**（不影响运行）

---

## 🔍 依赖检测机制

### 编译时检测
CMake 会在配置阶段检测依赖：

```cmake
# 必需依赖 - 缺失会报错
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Svg)

# 可选依赖 - 缺失只警告
find_package(ZXing QUIET)
if(TARGET ZXing::ZXing)
    # 启用二维码功能
endif()
```

### 运行时检测
程序启动时会检测可选组件：

```cpp
// PP-OCR 运行时检测
if (LinuxPpocrEngine::available()) {
    // 使用高精度 PP-OCR
} else {
    // 回退到 Tesseract
}
```

---

## 🎨 功能-依赖对应表

| 功能 | 必需依赖 | 可选依赖 | 缺失影响 |
|------|---------|---------|---------|
| **基础截图** | Qt6, X11/Wayland | - | 核心功能 |
| **OCR 识别** | Tesseract | ONNX Runtime | 精度大幅下降 |
| **贴图置顶** | Qt6 | LayerShell | Wayland 定位精度降低 |
| **GIF/MP4 录制** | GStreamer, PipeWire | - | 核心功能 |
| **长截图** | Qt6 | - | 核心功能 |
| **二维码识别** | - | ZXing | 功能完全不可用 |
| **文字朗读** | - | Qt6-Speech/SpeechD | 功能完全不可用 |
| **智能元素** | - | AT-SPI2 | 功能完全不可用 |

---

## ⚙️ 编译选项

CMake 会根据检测到的依赖自动启用功能：

```bash
# 检查编译了哪些功能
grep -E "JIETUBA_HAS_" cpp/build/CMakeCache.txt

# 应该看到：
# JIETUBA_HAS_X11:BOOL=ON           # X11 支持
# JIETUBA_HAS_XFIXES:BOOL=ON       # X11 光标
# JIETUBA_HAS_TESSERACT:BOOL=ON    # Tesseract OCR
# JIETUBA_HAS_ZXING:BOOL=ON        # 二维码
# JIETUBA_HAS_LAYER_SHELL:BOOL=ON  # LayerShell
```

---

## 🚨 常见依赖问题

### 问题1：ONNX Runtime 找不到
```bash
# 安装 CPU 版本
sudo pacman -S onnxruntime-cpu

# 或 GPU 版本（需要 NVIDIA 显卡）
sudo pacman -S onnxruntime-cuda
```

### 问题2：PP-OCR 模型缺失
```bash
# 模型应该在源码中，构建时会自动安装
ls models/PP-OCR*.onnx

# 如果缺失，从原版仓库下载
cd models
wget https://github.com/1003129155/jietuba/raw/refs/heads/master2/models/PP-OCRv6_det_small.onnx
wget https://github.com/1003129155/jietuba/raw/refs/heads/master2/models/PP-OCRv6_rec_small.onnx
wget https://github.com/1003129155/jietuba/raw/refs/heads/master2/models/ppocr_keys_v1.txt
```

### 问题3：ZXing 版本不兼容
```bash
# 确保安装的是 zxing-cpp（C++ 版本）
pacman -Qi zxing-cpp
```

### 问题4：LayerShell 在非 KDE 环境
```bash
# LayerShell 仅支持部分 Wayland 合成器
# KDE Plasma 6, Hyprland, Sway 支持
# GNOME 不支持（会回退到普通置顶窗口）
```

---

## 📊 依赖大小估算

| 类别 | 包大小（约） | 说明 |
|------|-------------|------|
| Qt6 基础 | ~50 MB | 核心依赖 |
| GStreamer | ~30 MB | 视频处理 |
| X11/Wayland | ~5 MB | 显示协议 |
| Tesseract | ~20 MB | 基础 OCR |
| ONNX Runtime | ~15 MB | 高精度 OCR |
| ZXing | ~2 MB | 二维码 |
| **总计（完整）** | **~120 MB** | 所有依赖 |

---

## ✅ 验证依赖安装

```bash
# 检查必需依赖
pacman -Q qt6-base qt6-svg gstreamer wayland libx11

# 检查可选依赖
pacman -Q onnxruntime tesseract zxing-cpp layer-shell-qt 2>/dev/null || echo "某些可选依赖未安装"

# 运行时检查
./jietuba_linux_ui_preview
# 在日志中查看："PP-OCR 可用" 或 "回退到 Tesseract"
```
