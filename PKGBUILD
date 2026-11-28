# Maintainer: qscqsc123 <wy56752466@gmail.com>
pkgname=jietuba-linux-git
pkgver=r607.dab3f08
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
    'onnxruntime-cpu>=1.24'
    'zxing-cpp'
)
makedepends=(
    'git'
    'cmake>=3.24'
    'cargo'
    'wayland-protocols'
    'qt6-tools'
    'ffmpeg'
)
optdepends=(
    'layer-shell-qt: Wayland layer-shell support for pins'
    'qt6-speech: Text-to-speech support'
    'speech-dispatcher: Alternative TTS backend'
    'at-spi2-core: Smart element detection'
)
provides=('jietuba-linux' 'jietuba-linux-ppocr')
conflicts=('jietuba-linux' 'jietuba-linux-ppocr')
replaces=('jietuba-linux-ppocr')
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
    cargo build --release --locked --features load-dynamic \
        --manifest-path ../rust_libs/ppocr_rust/Cargo.toml --target-dir build/ppocr
}

package() {
    cd "$srcdir/jietuba-linux/cpp"
    
    install -Dm755 "build/jietuba_linux_ui_preview" "$pkgdir/usr/bin/jietuba-linux"

    local app_id=io.github.jietuba.LinuxPreview size icon_dir
    install -d "$pkgdir/usr/share/applications"
    sed 's|@CMAKE_INSTALL_FULL_BINDIR@|/usr/bin|g' linux/jietuba-linux.desktop.in \
        > "$pkgdir/usr/share/applications/$app_id.desktop"
    for size in 16 24 32 48 64 128 256; do
        icon_dir="$pkgdir/usr/share/icons/hicolor/${size}x${size}/apps"
        install -d "$icon_dir"
        ffmpeg -nostdin -loglevel error -i ../app.ico -vf "scale=$size:$size:flags=lanczos" \
            -frames:v 1 -update 1 "$icon_dir/$app_id.png"
    done
    
    if [ -f "build/jietuba-smart-element" ]; then
        install -Dm755 "build/jietuba-smart-element" "$pkgdir/usr/bin/jietuba-smart-element"
    fi
    
    install -Dm755 build/ppocr/release/libppocr_rust.so "$pkgdir/usr/lib/jietuba/libppocr_rust.so"
    install -Dm644 -t "$pkgdir/usr/share/jietuba/models" \
        ../models/PP-OCRv6_det_small.onnx ../models/PP-OCRv6_rec_small.onnx
    install -Dm644 -t "$pkgdir/usr/share/licenses/$pkgname" ../rust_libs/ppocr_rust/*NOTICES.txt
    
    install -Dm644 "../LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 "../README.md" "$pkgdir/usr/share/doc/$pkgname/README.md"
}
