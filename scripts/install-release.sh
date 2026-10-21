#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Installs the prebuilt Arch Linux release archive. The source checkout uses install.sh.
set -euo pipefail

srcdir="$(cd "$(dirname "$0")" && pwd)"
prefix="${prefix:-/usr}"
plugin_id=org.kde.plasma.lrc
restart=1
for argument in "$@"; do
    case "$argument" in
        --no-restart) restart=0 ;;
        -h | --help)
            echo "Usage: ./install.sh [--no-restart]"
            echo "Installs the prebuilt widget into $prefix."
            exit 0 ;;
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

library="$srcdir/usr/lib/qt6/plugins/plasma/applets/$plugin_id.so"
plasmoid="$srcdir/usr/share/plasma/plasmoids/$plugin_id"
if [[ ! -f "$library" || ! -f "$plasmoid/metadata.json" || ! -f "$srcdir/LICENSE" ]]; then
    echo "Release files missing. Extract the complete release archive before installing." >&2
    exit 1
fi

SUDO=()
if [[ $EUID -ne 0 && ! -w "$prefix" ]]; then
    SUDO=(sudo)
fi

target="$prefix/lib/qt6/plugins/plasma/applets"
"${SUDO[@]}" install -d "$target" "$prefix/share/plasma/plasmoids/$plugin_id"
# Replace the inode atomically: truncating a library mapped by plasmashell can crash it.
temporary="$target/.$plugin_id.so.new.$$"
trap '"${SUDO[@]}" rm -f -- "$temporary"' EXIT
"${SUDO[@]}" install -m 755 "$library" "$temporary"
"${SUDO[@]}" mv -f -- "$temporary" "$target/$plugin_id.so"
"${SUDO[@]}" cp -a "$plasmoid/." "$prefix/share/plasma/plasmoids/$plugin_id/"
"${SUDO[@]}" install -Dm644 "$srcdir/LICENSE" "$prefix/share/licenses/plasma-lrc/LICENSE"
echo "Installed Plasma LRC into $prefix."

if [[ $restart -eq 1 ]] && systemctl --user is-active --quiet plasma-plasmashell.service; then
    systemctl --user restart plasma-plasmashell.service
else
    echo "Restart plasmashell to load the updated widget."
fi
