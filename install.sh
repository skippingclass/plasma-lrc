#!/usr/bin/env bash
#
# Собирает и ставит виджет (или обновляет уже установленный).
#
#   ./install.sh                 собрать, поставить, перезапустить plasmashell
#   ./install.sh --no-restart    не трогать plasmashell
#
# Переменные окружения:
#   builddir  каталог сборки (по умолчанию build)
#   prefix    префикс установки (по умолчанию /usr)
#   jobs      parallelism (по умолчанию nproc)

set -euo pipefail

builddir="${builddir:-build}"
prefix="${prefix:-/usr}"
jobs="${jobs:-$(nproc)}"
for arg in "$@"; do
    case "$arg" in
        --no-restart) ;;
        -h | --help)
            sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Неизвестный аргумент: $arg" >&2
            exit 2
            ;;
    esac
done

srcdir="$(cd "$(dirname "$0")" && pwd)"
cd "$srcdir"

echo "==> Сборка"
cmake -S . -B "$builddir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$builddir" --parallel "$jobs"

echo "==> Подготовка файлов"
stage_dir=$(mktemp -d)
trap 'rm -rf -- "$stage_dir"' EXIT
DESTDIR="$stage_dir" cmake --install "$builddir" --prefix /usr
install -m755 scripts/install-release.sh "$stage_dir/install.sh"
install -m644 LICENSE "$stage_dir/LICENSE"
prefix="$prefix" "$stage_dir/install.sh" "$@"

cat <<'EOF'

Если виджет уже был на панели, он останется на месте с прежними настройками.
Если хочешь начать с чистых значений: правый клик по виджету → «Настроить…».

Полное удаление:
    sudo kpackagetool6 --remove org.kde.plasma.lrc --type Plasma/Applet --global
    sudo rm -f /usr/lib/qt6/plugins/plasma/applets/org.kde.plasma.lrc.so
(без --type Plasma/Applet и --global kpackagetool6 ищет пакет другого типа
и ругается «Invalid metadata for package structure ""»)
EOF
