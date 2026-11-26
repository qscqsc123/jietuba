# Maintainer: Your Name <your.email@example.com>
pkgname=jietuba-linux-git
pkgver=r1.0.0
pkgrel=1
pkgdesc="Screenshot, annotation, OCR, pin, and recording tool (Linux port)"
arch=('x86_64' 'aarch64')
url="https://github.com/YOUR_USERNAME/jietuba-linux"
license=('MIT')
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
makedepends=(
    'git'
    'cmake>=3.24'
    'cargo'
    'wayland-protocols'
    'qt6-tools'
)
optdepends=(
    'layer-shell-qt: Wayland layer-shell support for pins'
    'qt6-speech: Text-to-speech support'
    'speech-dispatcher: Alternative TTS backend'
    'onnxruntime: High-accuracy OCR with PP-OCR (recommended)'
    'at-spi2-core: Smart element detection'
)
provides=('jietuba-linux')
conflicts=('jietuba-linux')
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
    cd "$srcdir/${pkgname%-git}"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

prepare() {
    cd "$srcdir/${pkgname%-git}"
    # 准备 Cargo 依赖（离线构建）
    cd rust_libs
    cargo fetch --locked
}

build() {
    cd "$srcdir/${pkgname%-git}/cpp"
    
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DJIETUBA_LINUX_CAPTURE_TESTS=OFF
    
    cmake --build build --parallel
}

check() {
    cd "$srcdir/${pkgname%-git}/cpp/build"
    # 运行测试（需要虚拟显示器）
    # ctest --output-on-failure || true
}

package() {
    cd "$srcdir/${pkgname%-git}/cpp"
    
    # 安装主程序
    install -Dm755 "build/jietuba-linux" "$pkgdir/usr/bin/jietuba-linux"
    
    # 安装可选辅助工具
    if [ -f "build/jietuba-smart-element" ]; then
        install -Dm755 "build/jietuba-smart-element" "$pkgdir/usr/bin/jietuba-smart-element"
    fi
    
    # 安装桌面文件
    install -Dm644 "build/linux-ui/io.github.jietuba.LinuxPreview.desktop" \
        "$pkgdir/usr/share/applications/jietuba-linux.desktop"
    
    # 安装图标
    for size in 16 32 48 64 128 256; do
        install -Dm644 "../svg/托盘.svg" \
            "$pkgdir/usr/share/icons/hicolor/${size}x${size}/apps/jietuba-linux.svg"
    done
    
    # 安装 OCR 模型（如果存在）
    if [ -d "../models" ]; then
        install -dm755 "$pkgdir/usr/share/jietuba/models"
        install -Dm644 ../models/*.onnx "$pkgdir/usr/share/jietuba/models/" 2>/dev/null || true
        install -Dm644 ../models/*.txt "$pkgdir/usr/share/jietuba/models/" 2>/dev/null || true
    fi
    
    # 安装许可证
    install -Dm644 "../LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    
    # 安装文档
    install -Dm644 "../README.md" "$pkgdir/usr/share/doc/$pkgname/README.md"
    install -Dm644 "../LINUX_PORT_ANALYSIS.md" "$pkgdir/usr/share/doc/$pkgname/LINUX_PORT_ANALYSIS.md"
}
