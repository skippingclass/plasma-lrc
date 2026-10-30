# SPDX-License-Identifier: MIT
#
# Maintainer: skippingclass
pkgname=plasma-lrc
pkgdesc="Plasma 6 panel widget that shows the lyric being sung, word by word when the source has timings"
arch=('x86_64' 'aarch64')
url="https://github.com/skippingclass/plasma-lrc"
license=('MIT')
# qt6-declarative carries Qt6::Qml, which the widget links against, and the QML
# engine modules the panel views import.
depends=('plasma-workspace>=6.0' 'kconfig' 'kcoreaddons' 'ki18n' 'qt6-base' 'qt6-declarative')
makedepends=('cmake' 'ninja' 'qt6-declarative')
source=("git+$url.git")
sha256sums=('SKIP')

# makepkg 7 no longer picks up a $pkgname.install file on its own: without this
# line the script is silently left out of the package.
install=plasma-lrc.install

# Placeholders: makepkg 7 refuses an empty one, and pkgver() below replaces this
# with the real thing on every build.
pkgver=1.8.5.r0
pkgrel=1

pkgver() {
    cd "$pkgname"
    # The version lives in metadata.json, because that is the one the panel shows
    # and the one Discover reads. Duplicating it here is how they drift apart.
    # The commits and the hash go after it, so a new commit is always a newer
    # package even when nobody remembered to bump the number.
    printf '%s.r%s.g%s\n' \
        "$(sed -n 's/.*"Version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' metadata.json)" \
        "$(git rev-list --count HEAD)" \
        "$(git rev-parse --short=7 HEAD)"
}

build() {
    cmake -S "$pkgname" -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=None \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DPLASMA_LRC_TESTS=ON
    cmake --build build
}

check() {
    ctest --test-dir build --output-on-failure
}

package() {
    # cmake has no --destdir; DESTDIR is the environment variable it reads.
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 "$pkgname/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
