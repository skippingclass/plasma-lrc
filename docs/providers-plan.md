# План архитектурной переработки источников текстов (Multi-Source Lyrics Pipeline) для Plasma LRC

## 1. Контекст и цели
В текущей реализации `plasma_lrc` логика получения текстов жестко привязана к связке **Spicy Lyrics (если трек из Spotify) -> LRCLIB -> lrc_tty (локальный fallback)**.
Это создает ряд критических проблем:
1. Зависимость от драконовских правил Spicy Lyrics (требования к обязательной атрибуции в одну строчку UI, блокировки API-ключей, необходимость регистрации шаблонов приложений).
2. Отсутствие гибкости: пользователь не может выбрать порядок опроса источников, включить или выключить конкретный провайдер.
3. Отсутствие богатых альтернативных баз (AMLL TTML DB, Apple Music, lrc.red, KuGou, Unison и т.д.).

**Цель:** Реализовать модульную архитектуру провайдеров текстов (Source Provider Pipeline) с поддержкой:
- Конфигурируемого списка источников: включение/выключение каждого.
- Настраиваемого приоритета (drag-and-drop / стрелки вверх-вниз в UI настроек).
- Цепочки фоллбеков (Chain of Responsibility): если источник вернул 404, таймаут или пустой результат, опрашивается следующий по приоритету.
- Поддержки как строчной синхронизации (LRC), так и пословной (Word-level sync / TTML / karaoke).

---

## 2. Анализ и список источников (Sources)

| Источник | Формат | Идентификация трека | Нужен ли токен/ключ? | Особенности и требования |
| :--- | :--- | :--- | :--- | :--- |
| **LRCLIB** | LRC / Plain | Title + Artist + Album + Duration | Нет | Публичный, свободный, очень надежный для базового LRC |
| **lrc.red** | TTML / LRC / YAML | ISRC, либо Title + Artist (через search) | Нет (нужен только валидный `User-Agent`) | Богатая база пословных таймингов Apple Music; никаких токенов и жестких требований к 30px UI |
| **AMLL TTML DB** (GitHub/CDN) | TTML | Apple Music ID / ISRC / Имя файла в репозитории | Нет (статический CDN / jsDelivr / raw GitHub) | Громадная открытая база готовых Apple Music TTML файлов сообщества AMLL |
| **Apple Music** | TTML | ISRC или поиск по каталогу | Требуется Developer Token (JWT) или публичный web-token | Прямой источник Apple Music; для работы обычно парсится web-токен из web player или используется прокси |
| **Spicy Lyrics** | TTML / LRC | Spotify Track ID | Требуется API Key (`sl_pk_...` / `sl_sk_...`) | Опциональный провайдер со строгими правилами атрибуции и сроком кэша 30 дней |
| **Spotify / Musixmatch** | RichSync / Line | Spotify ID / Canvas / Musixmatch Token | Spotify sp_dc cookie или User Token | Напрямую из мобильного/веб API Spotify (через внутренний Musixmatch токен) |
| **KuGou (酷狗)** | KRC (слоговый) / LRC | Title + Artist + Duration + Hash | Нет (публичные эндпоинты API KuGou) | Огромная китайская база с послоговой синхронизацией (формат KRC конвертируется в слоговые тайминги) |
| **Unison** | TTML / LRC | Title + Artist / ISRC | Зависит от инстанса | Открытый агрегатор синхронизированных текстов |
| **Local / lrc_tty** | LRC | Файлы рядом с треком / локальный кэш | Нет | Локальные `.lrc` файлы или сторонний бинарник |

---

## 3. Архитектура решения в C++ / Qt6

### 3.1. Интерфейс базового провайдера: `LyricsProvider`
Создается абстрактный базовый класс `LyricsProvider` (наследник `QObject`):
```cpp
class LyricsProvider : public QObject {
    Q_OBJECT
public:
    enum class Capability {
        LineSync = 1 << 0,
        WordSync = 1 << 1,
        SearchByTitle = 1 << 2,
        SearchByIsrc = 1 << 3,
        SearchBySpotifyId = 1 << 4
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    virtual QString id() const = 0;             // "lrcred", "lrclib", "amll", etc.
    virtual QString displayName() const = 0;    // "lrc.red", "LRCLIB"
    virtual Capabilities capabilities() const = 0;
    virtual bool isConfigured() const { return true; }
    virtual void requestLyrics(const TrackMetadata &track, int generation) = 0;
    virtual void abort() = 0;

Q_SIGNALS:
    void lyricsLoaded(int generation, const Lyrics &lyrics, const ProviderInfo &info);
    void lyricsFailed(int generation, ProviderError error);
};
```

### 3.2. Менеджер цепочки запросов: `LyricsPipelineManager`
Вместо кучи разрозненных флагов в `LrcApplet` (`m_fetchingFromSpicy`, `m_fetchingLrc`, `m_lrclibKey`), запросами управляет пайплайн:
1. Получает метаданные трека от MPRIS (`TrackMetadata`: title, artist, album, duration, isrc, spotifyId).
2. Формирует упорядоченный список активных провайдеров согласно настройкам пользователя (приоритетам).
3. Фильтрует провайдеры по доступности данных (например, если нет Spotify ID, провайдеры, требующие Spotify ID, пропускаются или переключаются на поиск по названию).
4. Последовательно (или по превентивной схеме) опрашивает источники.
5. При успешном ответе: передает `Lyrics` в `LrcApplet` с метаданными об источнике (для отображения в подсказке/настройках).
6. При неудаче: мгновенно пробует следующий провайдер в цепочке.

---

## 4. Пользовательский интерфейс настроек (KCM QML)

Обновление страницы `contents/ui/configSource.qml`:
1. **Список источников с чекбоксами (Вкл / Выкл).**
2. **Кнопки перемещения вверх / вниз (Приоритет опроса):**
   - Пользователь может поставить `lrc.red` на 1-е место, `AMLL TTML` на 2-е, `LRCLIB` на 3-е, а `Spicy Lyrics` выключить вообще или отправить в самый конец.
3. **Панель настроек выбранного источника:**
   - Для Spicy Lyrics: ввод API Key.
   - Для Apple Music / Spotify: поля для токенов (если требуются).
   - Для lrc.red / LRCLIB: информация о статусе и ссылка на проект.
4. **Конфигурация в `main.xml`:**
   - Сохранение порядка и активности: например, строковый список идентификаторов `enabledProviders` (`"lrcred,amll,lrclib,kugou"`) или структурированный JSON/QStringList.

---

## 5. План поэтапной реализации

### Фаза 1: Рефакторинг ядра и абстракция провайдеров
- Выделение общего интерфейса `LyricsProvider` и структуры `TrackMetadata`.
- Перенос `LrclibClient` и `SpicyLyrics` на новый интерфейс.
- Создание `LyricsPipelineManager` и интеграция его в `LrcApplet`.

### Фаза 2: Добавление свободных провайдеров без токенов (Highest Priority)
- **`LrcRedProvider`:** интеграция `https://lrc.red` (поиск по `/search.json` и прямой запрос TTML/LRC по `/s/{isrc}.{ttml,lrc}`).
- **`AmllProvider`:** интеграция базы AMLL TTML DB (индексация и скачивание TTML).
- Парсинг формата TTML с пословными таймингами (расширение существующего парсера синхронизации).

### Фаза 3: Дополнительные провайдеры
- **`KuGouProvider`:** парсинг китайской базы KuGou и декодирование KRC/LRC таймингов.
- **`UnisonProvider`:** подключение API Unison.
- **`SpotifyProvider` / `AppleMusicProvider`:** модули для работы с токенами/web-эндпоинтами.

### Фаза 4: UI переключателей и приоритетов в KDE Plasma
- Переработка `configSource.qml` в полноценный список управления источниками с сортировкой.
- Обновление отображения атрибуции в виджете (корректная ссылка на активный источник в тултипе/попапе).
- Тестирование фоллбеков и переключения треков.
