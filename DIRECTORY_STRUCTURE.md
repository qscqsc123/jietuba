# jietuba-linux 项目目录结构

## 📁 核心目录

```
jietuba-linux/
├── cpp/                          # C++ 源代码（主程序）
│   ├── linux/                    # Linux 特定实现
│   │   ├── capture/              # 截图后端（KWin/Wayland/X11/Portal）
│   │   ├── ppocr_wrapper.*       # PP-OCR 包装器
│   │   ├── tesseract_ocr.*       # Tesseract OCR 包装器
│   │   ├── pin_window.*          # 贴图窗口
│   │   ├── gif_capture_window.*  # GIF/MP4 录制
│   │   └── ...
│   ├── src/                      # 跨平台源代码
│   │   ├── canvas/               # 画布和标注工具
│   │   ├── tools/                # 绘图工具（箭头、文字、马赛克等）
│   │   ├── ui/                   # UI 组件
│   │   └── ...
│   ├── CMakeLists.txt            # 构建配置
│   └── build/                    # 编译输出（.gitignore）
│
├── rust_libs/                    # Rust 库（性能关键模块）
│   ├── ppocr_rust/               # PP-OCR 引擎
│   ├── smart_erase/              # 智能擦除
│   ├── gifrecorder/              # GIF 录制
│   ├── longstitch/               # 长截图拼接
│   └── ...
│
├── models/                       # OCR 模型文件
│   ├── PP-OCRv6_det_small.onnx   # 文字检测模型
│   ├── PP-OCRv6_rec_small.onnx   # 文字识别模型
│   └── ppocr_keys_v1.txt         # 字符字典
│
├── svg/                          # SVG 图标资源
│   └── fluent_icons/             # Fluent 风格图标
│
├── docs/                         # 文档
│   └── MODULES.md                # 模块说明
│
├── licenses/                     # 第三方许可证
│
└── scripts/                      # 工具脚本

## 📄 重要文件

### 构建相关
- `cpp/CMakeLists.txt` - CMake 构建配置
- `rust_libs/Cargo.toml` - Rust 工作空间配置

### 打包相关
- `PKGBUILD` - Arch Linux AUR 打包脚本

### 文档
- `README*.md` - 多语言说明文档
- `LINUX_PORT_ANALYSIS.md` - Linux 移植分析报告
- `IMPLEMENTATION_SUMMARY.md` - 实施总结
- `TEST_CHECKLIST.md` - 测试清单
- `LICENSE` - MIT 许可证

### 配置
- `.gitignore` - Git 忽略规则
- `.gitattributes` - Git 属性配置

## 🚫 已忽略的目录/文件

- `build/` - 编译输出
- `cpp/build/` - C++ 编译输出
- `.vscode/` - VS Code 配置
- `backups/` - 备份文件
- `.agents/`, `.aws/`, `.codex/` - IDE 私有配置
- `*.o`, `*.so`, `*.a` - 编译产物
- `*.swp`, `*~` - 编辑器临时文件

## 🔨 构建流程

```bash
# 1. 配置
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# 2. 编译
make -j$(nproc)

# 3. 运行
./jietuba_linux_ui_preview
```

## 📦 安装

```bash
# 安装到系统
sudo make install

# 或使用 AUR
yay -S jietuba-linux-git
```
