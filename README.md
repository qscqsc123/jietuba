<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="jietuba logo" />

# jietuba-linux

**截图、屏幕标注、OCR、贴图、翻译工具 - Linux 移植版**

[![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) ![platform](https://img.shields.io/badge/Linux-x64-orange?style=flat-square)

基于 [jietuba](https://github.com/1003129155/jietuba) 的 Linux 原生移植版本

</div>

## 概述

jietuba-linux 是将 Windows 版 jietuba 移植到 Linux 平台的原生版本，提供截图标注、屏幕绘制、OCR 文字识别、贴图、翻译等功能。

## 主要特性

- **完整的截图工具集**：智能选区（窗口/元素识别）、丰富的标注工具（画笔、箭头、文字、马赛克、聚光灯等）
- **高精度 OCR**：支持 PP-OCR（95%+ 中文识别率）和 Tesseract，可自由切换
- **贴图功能**：一键将截图或图片钉在屏幕上，支持缩放、标注、文字识别
- **长截图**：支持自动滚动拼接，四个方向均可操作
- **屏幕录制**：支持 GIF 和 MP4 格式，可选硬件加速
- **翻译功能**：集成多种翻译服务（DeepL、Google、有道等）

## 系统要求

### 必需依赖
- Qt6 (>= 6.5)
- CMake (>= 3.21)
- C++20 编译器
- Rust toolchain (>= 1.70)
- Tesseract OCR

### 可选依赖（推荐）
- **ONNX Runtime** (>= 1.24) - 启用 PP-OCR 高精度识别
- FFmpeg - 视频录制支持

### Arch Linux
```bash
sudo pacman -S qt6-base qt6-multimedia cmake rust tesseract onnxruntime-cpu ffmpeg
```

### Ubuntu/Debian
```bash
sudo apt install qt6-base-dev qt6-multimedia-dev cmake rustc tesseract-ocr libonnxruntime-dev ffmpeg
```

## 快速安装（Arch Linux）

在 `dev` 分支的仓库根目录构建并安装：

```bash
makepkg -si
```

主包内置 PP-OCR 桥接库和两个识别模型，pacman 会安装 `onnxruntime-cpu`。如果已安装旧的独立扩展 `jietuba-linux-ppocr`，升级时按提示同意替换即可。

安装后可在应用菜单中搜索“截图吧”。如需桌面快捷方式，可通过桌面环境的应用菜单将“截图吧”添加到桌面。

## 从源码编译

### 1. 克隆仓库
```bash
git clone https://github.com/qscqsc123/jietuba-linux.git
cd jietuba-linux
```

### 2. 编译 Rust 库（PP-OCR）
```bash
cd rust_libs/ppocr_rust
cargo build --release --locked --features load-dynamic
sudo mkdir -p /usr/lib/jietuba
sudo cp ../../rust_libs/target/release/libppocr_rust.so /usr/lib/jietuba/
cd ../..
```

### 3. 编译 C++ 程序
```bash
cd cpp
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 4. 安装
```bash
# 安装主程序
mkdir -p ~/.local/bin
cp jietuba_linux_ui_preview ~/.local/bin/jietuba-linux

# 安装 PP-OCR 模型（首次使用）
sudo mkdir -p /usr/share/jietuba/models
sudo cp ../../models/*.onnx /usr/share/jietuba/models/
```

### 5. 运行
```bash
jietuba-linux
```

## OCR 引擎说明

### PP-OCR（推荐）
- **精度**：95%+ 中文识别率
- **速度**：2-3 秒
- **依赖**：需要安装 `onnxruntime` 和模型文件
- **模型位置**：`/usr/share/jietuba/models/`

### Tesseract
- **精度**：60-75% 中文识别率
- **速度**：较慢（8秒+）
- **依赖**：系统自带 Tesseract

### 切换引擎
打开设置 → 截图 → OCR 设置，选择：
- **自动选择**：优先使用 PP-OCR（如果可用）
- **PP-OCR (高精度)**：强制使用 PP-OCR
- **Tesseract**：使用系统 Tesseract

## 技术架构

- **C++/Qt6**：主程序和 UI
- **Rust**：PP-OCR 引擎、图像处理核心
- **CMake**：构建系统

## 配置文件

设置保存在：`~/.config/jietuba-linux/settings.ini`

## 已知问题

- Wayland 支持仍在完善中，建议使用 X11
- 部分桌面环境可能需要手动配置全局快捷键

## 上游项目

本项目基于 [jietuba](https://github.com/1003129155/jietuba) Windows 版本移植。

原始项目作者：[1003129155](https://github.com/1003129155)

## 许可证

Apache License 2.0 - 详见 [LICENSE](LICENSE) 文件

## 贡献

欢迎提交 Issue 和 Pull Request！

---

**注意**：这是社区维护的 Linux 移植版本，非官方版本。
<img width="100%" alt="OCR" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR</b><br>Windows 11 Snipping Tool OCR and PP-OCR, offline; wrapped lines are joined and punctuation tidied.</p>
</td>
</tr>
## License

Copyright and the MIT license are in [LICENSE](LICENSE); Qt and other third-party components are described in [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md).
