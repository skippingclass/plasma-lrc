# plasma-lrc

Виджет для панели Plasma 6, который показывает текст текущей строки песни.
Всю работу по получению текста делает [lrc_tty](https://github.com/larsgrah/lrc_tty):
виджет периодически выполняет `lrc_tty --lines 1 --raw` и показывает то, что тот напечатал.

## Возможности

- Текст текущей строки прямо на панели, обрезается по длине (или по числу символов).
- Автоопределение MPRIS-плеера: если `lrc_tty` не нашёл текст у текущего плеера,
  виджет пробует следующий доступный.
- Плеер не играет — виджет не запускает процессы (положение берётся из MPRIS).
- Трек (исполнитель + название) в подсказке.
- Клик по виджету открывает окно с полной строкой.
- Настройки: интервал опроса, путь к `lrc_tty`, плеер, timestamps, иконка,
  длина текста, текст «нет слов».

## Требования

- Plasma 6 (`libPlasma`), Qt 6.5+, KDE Frameworks 6 (`CoreAddons`, `Config`, `I18n`)
- CMake 3.20+, компилятор с C++17
- `lrc_tty` в `$PATH` (см. его репозиторий)

## Сборка

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --install build            # требует прав на /usr/lib и /usr/share
```

Виджет ставится в два места, как это делают все applet-плагины Plasma:

- `/usr/lib/qt6/plugins/plasma/applets/org.kde.plasma.lrc.so`
- `/usr/share/plasma/plasmoids/org.kde.plasma.lrc/`

Чтобы установить в систему, нужен `sudo`. Пользовательская установка в
`~/.local` не сработает без дополнительных телодвижений: Plasma ищет
applet-плагины только в `QCoreApplication::libraryPaths()`, а туда `~/.local/lib`
не входит.

Arch: `makepkg -si` из приложенного `PKGBUILD`.

После установки: «Добавить виджеты» → **LRC Lyrics**.

## Проверка без панели

```sh
plasmawindowed org.kde.plasma.lrc
```

Либо без единого окна на экране:

```sh
QT_QPA_PLATFORM=offscreen plasmawindowed org.kde.plasma.lrc
```

Список плееров, которые виджет вообще может опросить:

```sh
lrc_tty --list-players
```

## Устройство

```
lrcapplet.{h,cpp}     C++-часть: Plasma::Applet, который держит QProcess и MPRIS
contents/ui/          QML: панельное представление, окно, страницы настроек
contents/config/      main.xml (kcfg, значения по умолчанию) + config.qml
metadata.json         метаданные виджета
```

C++ отдаёт в QML только состояние (`Plasmoid.text`, `Plasmoid.trackInfo`,
`Plasmoid.active`, …) и зеркалит настройки, чтобы QML не работал напрямую с
`KConfigPropertyMap`. Всю отрисовку делает `contents/ui/main.qml`.

Опрос: таймер дёргает `poll()`, который за `QProcess` запускает `lrc_tty`. Если
предыдущий процесс ещё жив (например, лезет в сеть за новым треком), просто
пропускаем тик. Сторожевой таймер убивает процесс, который завис дольше 20 с.
Отсутствие бинаря не приводит к спаму: интервал временно растёт до минуты.

## Лицензия

MIT (см. `LICENSE`). Сам `lrc_tty` — GPL-3.0; виджет только запускает его как
внешнюю программу и ничего не линкует.
