#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

source_dir=$1
scratch_dir=$(mktemp -d)
trap 'rm -rf -- "$scratch_dir"' EXIT

plugin_id=org.kde.plasma.lrc
export XDG_CONFIG_HOME="$scratch_dir/config"
export prefix="$scratch_dir/custom prefix"
fixture="$scratch_dir/expected-config"
config="$XDG_CONFIG_HOME/plasma-org.kde.plasma.desktop-appletsrc"
package="$prefix/share/plasma/plasmoids/$plugin_id"
library="$prefix/lib/qt6/plugins/plasma/applets/$plugin_id.so"

release="$scratch_dir/release"
release_package="$release/usr/share/plasma/plasmoids/$plugin_id"
release_library="$release/usr/lib/qt6/plugins/plasma/applets/$plugin_id.so"
mkdir -p "$XDG_CONFIG_HOME" "$prefix" "$release_package/contents/ui" "$(dirname "$release_library")" "$scratch_dir/bin"
cp "$source_dir/scripts/install-release.sh" "$release/install.sh"
cp "$source_dir/LICENSE" "$release/LICENSE"
printf 'previous library\n' > "$release_library"
printf 'previous metadata\n' > "$release_package/metadata.json"
touch "$release_package/contents/ui/main.qml"
bash "$release/install.sh" --no-restart
cmp "$release_library" "$library"
cmp "$release_package/metadata.json" "$package/metadata.json"

# If the final library replacement fails, the previous package must come back.
PACKAGE_TEST_LIBRARY="$library"
PACKAGE_TEST_REAL_MV=$(command -v mv)
export PACKAGE_TEST_LIBRARY PACKAGE_TEST_REAL_MV
cat > "$scratch_dir/bin/mv" <<'STUB'
#!/usr/bin/env bash
if [[ ${!#} == "$PACKAGE_TEST_LIBRARY" ]]; then
    exit 1
fi
exec "$PACKAGE_TEST_REAL_MV" "$@"
STUB
chmod +x "$scratch_dir/bin/mv"
printf 'updated library\n' > "$release_library"
printf 'updated metadata\n' > "$release_package/metadata.json"
if PATH="$scratch_dir/bin:$PATH" bash "$release/install.sh" --no-restart; then
    echo "Installer reported success after the library replacement failed." >&2
    exit 1
fi
[[ $(cat "$library") == "previous library" && $(cat "$package/metadata.json") == "previous metadata" ]]

cat > "$config" <<'CONFIG'
[Containments][1][Applets][7]
plugin=org.kde.plasma.lrc

[Containments][1][Applets][7][Configuration][General]
wordStyle=2

[Containments][1][Applets][70]
plugin=org.kde.plasma.digitalclock

[Containments][1][Applets][70][Configuration]
plugin=org.kde.plasma.lrc
clockMode=other

[Containments][1][Applets][2]
plugin=org.kde.plasma.lrc

[Containments][1][Applets][2][Configuration]
wordStyle=1

[No Group]
showTimestamp=true
keepSetting=untouched
placeholderText=Old placeholder
CONFIG
cp "$config" "$scratch_dir/original"
cat > "$fixture" <<'CONFIG'
[Containments][1][Applets][70]
plugin=org.kde.plasma.digitalclock

[Containments][1][Applets][70][Configuration]
plugin=org.kde.plasma.lrc
clockMode=other

[No Group]
keepSetting=untouched
CONFIG

# Keep the fixture insulated from real system-wide package changes.
cat > "$scratch_dir/bin/kpackagetool6" <<'STUB'
#!/usr/bin/env bash
touch "$XDG_CONFIG_HOME/global-package-command"
STUB
cat > "$scratch_dir/bin/sudo" <<'STUB'
#!/usr/bin/env bash
exec "$@"
STUB
chmod +x "$scratch_dir/bin/"*
PATH="$scratch_dir/bin:$PATH" bash "$source_dir/uninstall.sh" --yes --no-restart

failed=0
diff -u "$fixture" "$config" || failed=1
cmp "$scratch_dir/original" "$config.bak-lrc-remove" || failed=1
if [[ -e "$package" || -e "$library" ]]; then
    echo "Uninstall left the fixture package or library behind." >&2
    failed=1
fi
if [[ -e "$XDG_CONFIG_HOME/global-package-command" ]]; then
    echo "Uninstall with a temporary prefix targeted the system's package registry." >&2
    failed=1
fi

if (cd "$scratch_dir" && source "$source_dir/PKGBUILD" && pkgver) > "$scratch_dir/pkgver.out" 2>&1; then
    echo "pkgver reported success when the source checkout was missing." >&2
    failed=1
fi

(
    export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
    release_repo="$scratch_dir/source"
    mkdir -p "$release_repo/scripts" "$release_repo/src/services" "$release_repo/src/providers"
    cp "$source_dir/scripts/release.sh" "$release_repo/scripts/"
    cp "$source_dir/CMakeLists.txt" "$source_dir/metadata.json" "$source_dir/PKGBUILD" "$release_repo/"
    cp "$source_dir/src/services/"{lrclib,spicylyrics}.cpp "$release_repo/src/services/"
    cp "$source_dir/src/providers/"{amllprovider,lrcredprovider}.cpp "$release_repo/src/providers/"
    cd "$release_repo"
    git init -q
    git config user.name "Packaging test"
    git config user.email "packaging@example.invalid"
    git add .
    git commit -qm Fixture
    original_commit=$(git rev-parse HEAD)
    git tag v99.0.0
    for command in cmake ctest; do
        printf '#!/usr/bin/env bash\nexit 0\n' > "$scratch_dir/bin/$command"
        chmod +x "$scratch_dir/bin/$command"
    done
    if PATH="$scratch_dir/bin:$PATH" bash scripts/release.sh 99.0.0 > "$scratch_dir/release.out" 2>&1; then
        echo "Release reported success for a preexisting tag." >&2
        exit 1
    fi
    if [[ $(git rev-parse HEAD) != "$original_commit" || -n $(git status --porcelain) ]]; then
        echo "Release changed the checkout before rejecting a preexisting tag." >&2
        exit 1
    fi
)

exit "$failed"
