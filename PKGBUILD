# Maintainer: pirkov
pkgname=plasma-lrc
pkgver=1.0.0
pkgrel=1
pkgdesc="Plasma 6 panel widget showing the current lyric line via lrc_tty"
arch=('x86_64' 'aarch64')
url="https://github.com/larsgrah/lrc_tty"
license=('MIT')
depends=('plasma-workspace' 'kcoreaddons' 'kconfig' 'ki18n')
makedepends=('cmake' 'ninja' 'pkgconf')
optdepends=('lrc_tty: the tool this widget asks for lyrics')
source=()
sha256sums=()

build() {
    cmake -S "$startdir" -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib
    cmake --build build
}

package() {
    cmake --install build --destdir "$pkgdir"
}
