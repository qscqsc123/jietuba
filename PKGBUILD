# Maintainer: qscqsc123 <wy56752466@gmail.com>
pkgname=jietuba-linux-git
pkgver=r606.856a338
pkgrel=1
pkgdesc="Screenshot, annotation, OCR, pin, and recording tool (Linux port)"
arch=('x86_64' 'aarch64')
url="https://github.com/qscqsc123/jietuba-linux"
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
    'onnxruntime: High-accuracy OCR with PP-OCR (strongly recommended)'
    'layer-shell-qt: Wayland layer-shell support for pins'
    'qt6-speech: Text-to-speech support'
    'speech-dispatcher: Alternative TTS backend'
    'at-spi2-core: Smart element detection'
)
provides=('jietuba-linux')
conflicts=('jietuba-linux')
source=("git+${url}.git#branch=dev")
sha256sums=('SKIP')

pkgver() {
    cd "$srcdir/jietuba-linux"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

prepare() {
    cd "$srcdir/jietuba-linux/cpp/linux/native_smart_erase"
    cargo fetch
}

build() {
    cd "$srcdir/jietuba-linux/cpp"
    
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    
    cmake --build build --target jietuba_linux_ui_preview
}

package() {
    cd "$srcdir/jietuba-linux/cpp"
    
    install -Dm755 "build/jietuba_linux_ui_preview" "$pkgdir/usr/bin/jietuba-linux"
    
    if [ -f "build/jietuba-smart-element" ]; then
        install -Dm755 "build/jietuba-smart-element" "$pkgdir/usr/bin/jietuba-smart-element"
    fi
    
    if [ -d "../models" ]; then
        install -dm755 "$pkgdir/usr/share/jietuba/models"
        install -Dm644 ../models/*.onnx "$pkgdir/usr/share/jietuba/models/" 2>/dev/null || true
        install -Dm644 ../models/*.txt "$pkgdir/usr/share/jietuba/models/" 2>/dev/null || true
    fi
    
    install -Dm644 "../LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 "../README.md" "$pkgdir/usr/share/doc/$pkgname/README.md"
}
