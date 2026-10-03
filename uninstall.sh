#!/usr/bin/env bash
#
# Полностью удаляет виджет: пакет, .so и его настройки.
#
#   ./uninstall.sh            спросит подтверждение
#   ./uninstall.sh --yes      без вопросов
#
# Переменные окружения:
#   prefix  префикс, откуда ставили (по умолчанию /usr)

set -euo pipefail

prefix="${prefix:-/usr}"
plugin_id="org.kde.plasma.lrc"
applets_rc="${XDG_CONFIG_HOME:-$HOME/.config}/plasma-org.kde.plasma.desktop-appletsrc"

assume_yes=0
restart_plasmashell=1
for arg in "$@"; do
    case "$arg" in
        --yes | -y) assume_yes=1 ;;
        --no-restart) restart_plasmashell=0 ;;
        -h | --help)
            sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Неизвестный аргумент: $arg" >&2
            exit 2
            ;;
    esac
done

if [[ $EUID -eq 0 ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

if [[ $assume_yes -eq 0 ]]; then
    echo "Удалить виджет $plugin_id (панель, настройки, всё)?"
    read -r -p "y/N: " answer
    [[ $answer == [yY] ]] || exit 0
fi

echo "==> Пакет"
$SUDO kpackagetool6 --remove "$plugin_id" --type Plasma/Applet --global || true

echo "==> Плагин"
$SUDO rm -f "$prefix/lib/qt6/plugins/plasma/applets/$plugin_id.so"

echo "==> Настройки"
# Плагин не переживает переустановку на месте: удалённые .so и пакет остаются в
# памяти plasmashell, поэтому его лучше перезапустить.
if [[ -f $applets_rc ]]; then
    backup="$applets_rc.bak-lrc-remove"
    cp "$applets_rc" "$backup"
    # Вырезаем только секции нашего виджета, остальное не трогаем.
    python3 - "$applets_rc" <<'PY'
import re
import sys

path = sys.argv[1]
plugin = "org.kde.plasma.lrc"
with open(path, encoding="utf-8") as handle:
    text = handle.read()

blocks = text.split("\n[")
out = []
for block in blocks:
    if block.startswith("Containments]") and re.search(r"^plugin=" + re.escape(plugin) + r"$", block, re.M):
        continue
    out.append(block)

result = "\n[".join(out)
# Мусор от старой версии: kcfg-записи без группы уезжали в корень файла.
result = re.sub(
    r"^\[No Group\]\n(?:(?:binaryPath|pollInterval|player|showTimestamp|showIcon|showTrackInfo|pauseWhenIdle|placeholderText|maxCharacters)=.*\n?)+",
    "",
    result,
    flags=re.M,
)
with open(path, "w", encoding="utf-8") as handle:
    handle.write(result)
print("    секции виджета вырезаны")
PY
    echo "    бэкап: $backup"
fi

echo "==> Перезапуск plasmashell"
if [[ $restart_plasmashell -eq 0 ]]; then
    echo "    пропущен (--no-restart), перезапусти сам: systemctl --user restart plasma-plasmashell"
elif systemctl --user is-active --quiet plasma-plasmashell.service; then
    systemctl --user restart plasma-plasmashell.service
else
    echo "    plasmashell не запущен как сервис — перелогинься."
fi

echo "Готово. Виджет удалён с панели; если хочешь вернуть — ./install.sh и «Добавить виджеты»."
