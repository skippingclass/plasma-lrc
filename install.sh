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
plugin_id="org.kde.plasma.lrc"

restart_plasmashell=1
for arg in "$@"; do
    case "$arg" in
        --no-restart) restart_plasmashell=0 ;;
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

if [[ $EUID -eq 0 ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

echo "==> Сборка"
cmake -S . -B "$builddir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$builddir" --parallel "$jobs"

# Старый .so нельзя перезаписывать на месте: у него уже замаплен plasmashell,
# и truncate+write на том же inode ломает запущенный процесс. Поэтому сначала
# удаляем, потом кладём новый — и перезапускаем plasmashell.
echo "==> Удаление предыдущей версии"
$SUDO rm -f "$prefix/lib/qt6/plugins/plasma/applets/$plugin_id.so"
$SUDO kpackagetool6 --remove "$plugin_id" --type Plasma/Applet --global >/dev/null 2>&1 || true

echo "==> Установка в $prefix"
$SUDO cmake --install "$builddir"

echo "==> Готово: $prefix/lib/qt6/plugins/plasma/applets/$plugin_id.so"

if [[ $restart_plasmashell -eq 1 ]]; then
    if systemctl --user is-active --quiet plasma-plasmashell.service; then
        echo "==> Перезапуск plasmashell (виджет с новым .so загрузится при старте)"
        systemctl --user restart plasma-plasmashell.service
    else
        echo
        echo "plasmashell не запущен как сервис — перелогинься, чтобы он подхватил новую версию."
    fi
fi

cat <<'EOF'

Если виджет уже был на панели, он останется на месте с прежними настройками.
Если хочешь начать с чистых значений: правый клик по виджету → «Настроить…».

Полное удаление:
    sudo kpackagetool6 --remove org.kde.plasma.lrc --type Plasma/Applet --global
    sudo rm -f /usr/lib/qt6/plugins/plasma/applets/org.kde.plasma.lrc.so
(без --type Plasma/Applet и --global kpackagetool6 ищет пакет другого типа
и ругается «Invalid metadata for package structure ""»)
EOF
