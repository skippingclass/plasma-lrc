#!/usr/bin/env bash
# Собирает виджет и ставит его в систему.
# set -euo pipefail

builddir="${builddir:-build}"
prefix="${prefix:-/usr}"
jobs="${jobs:-$(nproc)}"

if [[ $EUID -eq 0 ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

cmake -S "$(dirname "$0")" -B "$builddir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$builddir" --parallel "$jobs"

echo "Installing into $prefix (system-wide: Plasma only looks for applet plugins in the Qt plugin dirs)"
$SUDO cmake --install "$builddir"

echo
echo "Done. Log out and back in, then add the widget:"
echo "    Plasma: \"Add Widgets…\" -> LRC Lyrics"
echo
echo "Remove it with:"
echo "    $SUDO kpackagetool6 --remove org.kde.plasma.lrc"
echo "    $SUDO rm -f $prefix/lib/qt6/plugins/plasma/applets/org.kde.plasma.lrc.so"
