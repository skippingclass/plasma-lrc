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
if [[ ! -f "$library" || ! -f "$plasmoid/metadata.json" || ! -f "$plasmoid/contents/ui/main.qml" || ! -f "$srcdir/LICENSE" ]]; then
    echo "Release files missing. Extract the complete release archive before installing." >&2
    exit 1
fi

SUDO=()
if [[ $EUID -ne 0 && ! -w "$prefix" ]]; then
    SUDO=(sudo)
fi

plugin_dir="$prefix/lib/qt6/plugins/plasma/applets"
package_dir="$prefix/share/plasma/plasmoids"
package_target="$package_dir/$plugin_id"
"${SUDO[@]}" install -d "$plugin_dir" "$package_dir"

exec {install_lock}<"$package_dir"
flock "$install_lock"

temporary_library=""
package_placed=0
committed=0
stage_dir=$("${SUDO[@]}" mktemp -d "$package_dir/.$plugin_id.XXXXXX")

cleanup() {
    if [[ $committed -eq 0 ]]; then
        if [[ $package_placed -eq 1 ]]; then
            "${SUDO[@]}" mv -T -- "$package_target" "$stage_dir/incomplete" || return
        fi
        if "${SUDO[@]}" test -e "$stage_dir/previous" || "${SUDO[@]}" test -L "$stage_dir/previous"; then
            if ! "${SUDO[@]}" mv -T -- "$stage_dir/previous" "$package_target"; then
                echo "Could not restore the previous package from $stage_dir/previous" >&2
                return 1
            fi
        fi
    fi
    if [[ -n "$temporary_library" ]]; then
        "${SUDO[@]}" rm -f -- "$temporary_library"
    fi
    "${SUDO[@]}" rm -rf -- "$stage_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

temporary_library=$("${SUDO[@]}" mktemp "$plugin_dir/.$plugin_id.so.XXXXXX")
"${SUDO[@]}" install -m755 "$library" "$temporary_library"
"${SUDO[@]}" install -d -m755 "$stage_dir/new"
"${SUDO[@]}" cp -a --no-preserve=ownership "$plasmoid/." "$stage_dir/new/"
"${SUDO[@]}" install -Dm644 "$srcdir/LICENSE" "$prefix/share/licenses/plasma-lrc/LICENSE"

if [[ -e "$package_target" || -L "$package_target" ]]; then
    "${SUDO[@]}" mv -T -- "$package_target" "$stage_dir/previous"
fi
"${SUDO[@]}" mv -T -- "$stage_dir/new" "$package_target"
package_placed=1
# Replace the inode atomically while the previous plugin may still be mapped.
"${SUDO[@]}" mv -fT -- "$temporary_library" "$plugin_dir/$plugin_id.so"
committed=1
cleanup
trap - EXIT
echo "Installed Plasma LRC into $prefix."

if [[ $restart -eq 1 ]] && systemctl --user is-active --quiet plasma-plasmashell.service; then
    systemctl --user restart plasma-plasmashell.service
else
    echo "Restart plasmashell to load the updated widget."
fi
