/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "spicylyrics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace
{
const QByteArray s_defaultApiRoot = QByteArrayLiteral("https://api.spicylyrics.org/v1/lyrics/");

// Mirrors exist for networks where the official host is unreachable, and this
// also makes the client testable against a local server.
QByteArray apiRoot()
{
    static const QByteArray root = [] {
        const QByteArray fromEnvironment = qgetenv("SPICY_LYRICS_API_ROOT").trimmed();
        if (fromEnvironment.isEmpty()) {
            return s_defaultApiRoot;
        }
        QByteArray root = fromEnvironment;
        if (!root.endsWith('/')) {
            root.append('/');
        }
        return root;
    }();
    return root;
}
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.0 (Plasma 6 panel widget)");

// The terms require stored responses to be refetched or thrown away within this
// many days, so that a withdrawn sync actually disappears.
constexpr qint64 kCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
// A track without lyrics is remembered for a day, not a month: the community
// adds syncs all the time and we do not want to hide a new one for weeks.
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
// Written instead of a response when the API has nothing to show for a track.
// The reason is part of it so that the fallback can be explained later on,
// without asking the API again.
QByteArray missingMarker(MissingReason reason)
{
    const QByteArray name = reason == MissingReason::NotFound ? QByteArrayLiteral("notfound") : QByteArrayLiteral("unsynced");
    return QByteArrayLiteral("{\"missing\":true,\"reason\":\"") + name + QByteArrayLiteral("\"}");
}

MissingReason reasonFromMarker(const QByteArray &payload)
{
    return QJsonDocument::fromJson(payload).object().value(QStringLiteral("reason")).toString() == QLatin1String("notfound")
        ? MissingReason::NotFound
        : MissingReason::Unsynced;
}
constexpr int kRequestTimeoutMs = 15000;
// How many cache writes between sweeps for expired entries.
constexpr int kPruneEveryWrites = 64;

qint64 secondsToMs(const QJsonValue &value)
{
    return static_cast<qint64>(value.toDouble(-1) * 1000.0);
}

bool isWideScript(QChar c)
{
    return (c.unicode() >= 0x1100 && c.unicode() <= 0x11FF) // Hangul Jamo
        || (c.unicode() >= 0x2E80 && c.unicode() <= 0xA4CF) // CJK radicals .. Yi
        || (c.unicode() >= 0xAC00 && c.unicode() <= 0xD7A3) // Hangul syllables
        || (c.unicode() >= 0xF900 && c.unicode() <= 0xFAFF) // CJK compatibility
        || (c.unicode() >= 0xFF00 && c.unicode() <= 0xFFEF); // halfwidth/fullwidth
}

bool containsWideScript(const QString &text)
{
    for (const QChar c : text) {
        if (isWideScript(c)) {
            return true;
        }
    }
    return false;
}

bool startsWithSpace(const QString &text)
{
    return text.startsWith(QLatin1Char(' '));
}

/**
 * Whether a space has to be inserted between two consecutive syllables.
 *
 * The API hands out syllables with no whitespace in them at all, so word
 * boundaries are only marked by IsPartOfWord — and it marks the *first* piece of
 * a word that was split, not the piece that carries on from it. Real example
 * from the API: "o" flagged, then "k" plain gives "ok"; "har" flagged, then
 * "dest," gives "hardest,". Reading the flag the other way round glues the
 * previous word to this one and splits the word in two.
 *
 * Chinese, Japanese and Korean have no spaces at all, so they never get one.
 */
bool needsSpace(const QString &previous, const QString &current, bool previousStartsSplitWord)
{
    if (current.isEmpty()) {
        return false;
    }
    if (startsWithSpace(current)) {
        return false;
    }
    if (previous.endsWith(QLatin1Char(' ')) || previous.isEmpty()) {
        return false;
    }
    if (previousStartsSplitWord) {
        return false;
    }
    // CJK text is written without spaces.
    if (containsWideScript(previous) || containsWideScript(current)) {
        return false;
    }
    return true;
}

QString humanAttribution(const QString &source, const QJsonObject &uploadAttribution)
{
    const QString contributor = uploadAttribution.value(QStringLiteral("Maker")).toObject().value(QStringLiteral("username")).toString();
    const QString uploader = uploadAttribution.value(QStringLiteral("Uploader")).toObject().value(QStringLiteral("username")).toString();

    if (source == QLatin1String("spicy_lyrics")) {
        QString attribution = QStringLiteral("Spicy Lyrics");
        if (!contributor.isEmpty() || !uploader.isEmpty()) {
            attribution += QStringLiteral(" · ");
            const QString name = !contributor.isEmpty() ? contributor : uploader;
            attribution += QStringLiteral("made by @%1").arg(name);
            if (!contributor.isEmpty() && !uploader.isEmpty() && contributor != uploader) {
                attribution += QStringLiteral(" (@%1)").arg(uploader);
            }
        }
        return attribution;
    }

    if (source == QLatin1String("apple_music") || source == QLatin1String("applemusic") || source.contains(QLatin1String("apple"), Qt::CaseInsensitive)) {
        return QStringLiteral("Apple Music");
    }

    if (source.contains(QLatin1String("spotify"), Qt::CaseInsensitive)) {
        return QStringLiteral("Spotify");
    }

    // The terms ask for "unknown" rather than guessing a provider.
    return QStringLiteral("unknown source");
}

QString attributionUrl(const QString &source, const QJsonObject &uploadAttribution)
{
    if (source != QLatin1String("spicy_lyrics")) {
        return QString();
    }
    const QJsonObject maker = uploadAttribution.value(QStringLiteral("Maker")).toObject();
    if (!maker.value(QStringLiteral("url")).toString().isEmpty()) {
        return maker.value(QStringLiteral("url")).toString();
    }
    return uploadAttribution.value(QStringLiteral("Uploader")).toObject().value(QStringLiteral("url")).toString();
}

/**
 * Reads a {"Lead": {...}} or a bare {"Syllables": [...]} object into a line.
 */
bool readPart(const QJsonObject &part, LyricLine *line)
{
    const QJsonArray syllables = part.value(QStringLiteral("Syllables")).toArray();

    QString built;
    QString previousPiece;
    bool previousStartsSplit = false;
    QVector<bool> splitStarts;

    for (const QJsonValue &value : syllables) {
        const QJsonObject syllable = value.toObject();
        const QString text = syllable.value(QStringLiteral("Text")).toString();
        if (text.isEmpty()) {
            continue;
        }
        // The flag belongs to the first piece of a split word, so it says that
        // the *next* syllable continues this one, not that this one continues.
        const bool startsSplitWord = syllable.value(QStringLiteral("IsPartOfWord")).toBool();

        LyricWord word;
        word.text = text;
        word.startMs = secondsToMs(syllable.value(QStringLiteral("StartTime")));
        word.endMs = secondsToMs(syllable.value(QStringLiteral("EndTime")));

        const bool space = needsSpace(previousPiece, text, previousStartsSplit);
        if (space) {
            built.append(QLatin1Char(' '));
        }
        word.textStart = static_cast<int>(built.size());
        built.append(text);
        word.textEnd = static_cast<int>(built.size());
        line->words.append(word);
        splitStarts.append(startsSplitWord);

        previousPiece = text;
        previousStartsSplit = startsSplitWord;
    }

    // Line-synced responses may carry the whole line as a single string instead
    // of a list of syllables.
    if (line->words.isEmpty()) {
        const QString text = part.value(QStringLiteral("Text")).toString();
        if (text.isEmpty()) {
            return false;
        }
        LyricWord word;
        word.text = text;
        word.startMs = secondsToMs(part.value(QStringLiteral("StartTime")));
        word.endMs = secondsToMs(part.value(QStringLiteral("EndTime")));
        word.textEnd = static_cast<int>(text.size());
        built = text;
        line->words.append(word);
    }

    // Whitespace at the edges is noise: it would show up as a gap between the
    // lyric and the credit in the panel.
    built = built.trimmed();
    QVector<LyricWord> words;
    words.reserve(line->words.size());
    for (const LyricWord &word : line->words) {
        if (word.textStart >= built.size()) {
            continue;
        }
        LyricWord trimmed = word;
        trimmed.textEnd = qMin(trimmed.textEnd, static_cast<int>(built.size()));
        words.append(trimmed);
    }
    line->words = words;

    // A word that arrived in pieces ends at the first piece without the flag, so
    // "o" + "k" is one group while "think" + "ok" are two.
    int groupStart = 0;
    for (int i = 0; i < line->words.size(); ++i) {
        const bool endsGroup = i >= splitStarts.size() || !splitStarts.at(i);
        if (!endsGroup) {
            continue;
        }
        for (int j = groupStart; j <= i && j < line->words.size(); ++j) {
            line->words[j].groupStart = line->words.at(groupStart).textStart;
            line->words[j].groupEnd = line->words.at(i).textEnd;
        }
        groupStart = i + 1;
    }

    line->text = built;
    line->startMs = secondsToMs(part.value(QStringLiteral("StartTime")));
    if (line->startMs <= 0 && !line->words.isEmpty()) {
        line->startMs = line->words.first().startMs;
    }
    line->endMs = secondsToMs(part.value(QStringLiteral("EndTime")));
    if (line->endMs <= 0 && !line->words.isEmpty()) {
        line->endMs = line->words.last().endMs;
    }
    return true;
}
}

int Lyrics::lineAt(qint64 positionMs, int hint) const
{
    // Lines are sorted by start time, and a good part of them overlap: the next
    // line often begins before the previous one ends, which is normal for words
    // that are sung across a line break. The line that started last wins, so that
    // a line shows up when it begins instead of when the previous one lets go of
    // the screen. A gap between lines means nobody is singing, and the caller
    // keeps showing whatever it had.
    //
    // @p hint is the line shown last time. Playback moves forward, so the search
    // starts there instead of at the beginning: this runs fifty times a second.
    int found = -1;
    for (int i = qMax(0, qMin(hint, static_cast<int>(lines.size()) - 1)); i < lines.size(); ++i) {
        const LyricLine &line = lines.at(i);
        if (positionMs < line.startMs) {
            break;
        }
        if (positionMs < line.endMs) {
            found = i;
        }
    }
    return found;
}

SpicyLyrics::SpicyLyrics(QObject *parent)
    : QObject(parent)
    , m_watchdog(new QTimer(this))
{
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        // Nothing was listening to the reply; make sure it is not left hanging.
        auto pending = m_network.findChildren<QNetworkReply *>();
        for (QNetworkReply *reply : pending) {
            if (reply->isRunning()) {
                reply->abort();
            }
        }
    });
}

SpicyLyrics::~SpicyLyrics()
{
    if (m_watchdog->isActive()) {
        m_watchdog->stop();
    }
}

void SpicyLyrics::setKey(const QString &key)
{
    m_key = key.trimmed();
}

QString SpicyLyrics::key() const
{
    return m_key;
}

void SpicyLyrics::setEnabled(bool enabled)
{
    m_enabled = enabled && !m_key.isEmpty();
}

bool SpicyLyrics::isEnabled() const
{
    return m_enabled;
}

QString SpicyLyrics::cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/plasma-lrc/spicy");
}

QString SpicyLyrics::trackIdFromMpris(const QVariant &mprisTrackId)
{
    const QString value = mprisTrackId.toString();
    if (value.isEmpty()) {
        return QString();
    }

    // "/com/spotify/track/<id>" or "spotify:track:<id>" or just "<id>".
    static const QRegularExpression re(QStringLiteral("([A-Za-z0-9]{22})(?:$|[/?#])"));
    const QRegularExpressionMatch match = re.match(value);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    static const QRegularExpression trailing(QStringLiteral("([A-Za-z0-9]{22})$"));
    const QRegularExpressionMatch tail = trailing.match(value);
    return tail.hasMatch() ? tail.captured(1) : QString();
}

void SpicyLyrics::request(const QString &trackId)
{
    // Without a key there is nothing to ask with, and nothing entitles us to the
    // cached answers either: the caller falls back to lrc_tty.
    if (trackId.isEmpty() || !isEnabled()) {
        return;
    }
    load(trackId);
}

void SpicyLyrics::emitMissing(const QString &trackId, MissingReason reason)
{
    Q_EMIT missing(trackId, reason);
}

void SpicyLyrics::load(const QString &trackId)
{
    const auto cached = m_memoryCache.constFind(trackId);
    if (cached != m_memoryCache.constEnd()) {
        QTimer::singleShot(0, this, [this, trackId, lyrics = cached.value()] {
            Q_EMIT loaded(trackId, lyrics);
        });
        return;
    }

    const QString directory = cacheDirectory();
    const QString path = directory + QLatin1Char('/') + trackId + QStringLiteral(".json");
    // QFile::exists() is static and answers about the empty name when called on
    // an instance, so the path has to be passed to it.
    if (QFile::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            fetch(trackId);
            return;
        }
        const QByteArray payload = file.readAll();
        file.close();

        const QDateTime modified = QFileInfo(path).lastModified();
        const qint64 age = modified.isValid() ? modified.msecsTo(QDateTime::currentDateTime()) : kCacheTtlMs + 1;

        if (age <= kNegativeCacheTtlMs && payload.contains(QByteArrayLiteral("\"missing\""))) {
            // We already asked about this track today and there was nothing.
            const MissingReason reason = reasonFromMarker(payload);
            QTimer::singleShot(0, this, [this, trackId, reason] {
                Q_EMIT missing(trackId, reason);
            });
            return;
        }

        if (age <= kCacheTtlMs) {
            Lyrics lyrics;
            if (parse(payload, &lyrics, nullptr) && lyrics.isUsable()) {
                m_memoryCache.insert(trackId, lyrics);
                QTimer::singleShot(0, this, [this, trackId, lyrics] {
                    Q_EMIT loaded(trackId, lyrics);
                });
                return;
            }
        }
        // Stale or unusable: forget it and go to the network.
        QFile::remove(path);
    }

    fetch(trackId);
}

void SpicyLyrics::write(const QString &trackId, const QByteArray &payload)
{
    QDir().mkpath(cacheDirectory());
    QFile file(cacheDirectory() + QLatin1Char('/') + trackId + QStringLiteral(".json"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(payload);
    }

    // Entries are only ever read, never deleted, so without this the cache grows
    // for as long as the widget exists. Every so often, drop what has expired.
    if (++m_writesSincePrune >= kPruneEveryWrites) {
        m_writesSincePrune = 0;
        pruneCache();
    }
}

void SpicyLyrics::pruneCache()
{
    QDir directory(cacheDirectory());
    const QDateTime now = QDateTime::currentDateTime();
    const QFileInfoList entries = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo &entry : entries) {
        const QDateTime modified = entry.lastModified();
        if (!modified.isValid() || modified.msecsTo(now) > kCacheTtlMs) {
            QFile::remove(entry.absoluteFilePath());
        }
    }
}

void SpicyLyrics::fetch(const QString &trackId)
{
    QNetworkRequest request(QUrl(QString::fromUtf8(apiRoot()) + trackId));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", s_userAgent);
    request.setRawHeader("Authorization", "Bearer " + m_key.toUtf8());
    request.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, trackId] {
        reply->deleteLater();
        m_watchdog->stop();

        if (reply->error() != QNetworkReply::NoError) {
            // 404 means the API does not know the track, which is worth
            // remembering for a day. Anything else is a problem on our side or
            // theirs (no key, rate limit, no network), and retrying on the next
            // track is the right thing to do.
            const bool notFound = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
            if (notFound) {
                write(trackId, missingMarker(MissingReason::NotFound));
            }
            emitMissing(trackId, notFound ? MissingReason::NotFound : MissingReason::Unreachable);
            return;
        }

        const QByteArray payload = reply->readAll();
        Lyrics lyrics;
        QString parseError;
        if (!parse(payload, &lyrics, &parseError) || !lyrics.isUsable()) {
            // The reply was fine but there is nothing to time: plain text with no
            // timings, which is what Apple Music and Spotify answer for a track
            // nobody has made a sync for. Remembering it keeps us from asking
            // again on every repeat.
            write(trackId, missingMarker(MissingReason::Unsynced));
            emitMissing(trackId, MissingReason::Unsynced);
            return;
        }

        write(trackId, payload);
        m_memoryCache.insert(trackId, lyrics);
        Q_EMIT loaded(trackId, lyrics);
    });

    m_watchdog->start(kRequestTimeoutMs + 5000);
}

bool SpicyLyrics::parse(const QByteArray &json, Lyrics *lyrics, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(parseError.errorString());
    }
    if (!document.isObject()) {
        return fail(QStringLiteral("response is not an object"));
    }

    const QJsonObject body = document.object().value(QStringLiteral("Body")).toObject();
    if (body.isEmpty()) {
        return fail(QStringLiteral("no Body in response"));
    }

    lyrics->type = LyricsType::None;
    lyrics->lines.clear();

    const QString typeName = body.value(QStringLiteral("Type")).toString();
    if (typeName == QLatin1String("Syllable")) {
        lyrics->type = LyricsType::Syllable;
    } else if (typeName == QLatin1String("Line")) {
        lyrics->type = LyricsType::Line;
    } else if (typeName == QLatin1String("Static")) {
        lyrics->type = LyricsType::Static;
    }

    const QString source = body.value(QStringLiteral("source")).toString();
    const QJsonObject uploadAttribution = body.value(QStringLiteral("UploadAttribution")).toObject();
    lyrics->attribution = humanAttribution(source, uploadAttribution);
    lyrics->attributionUrl = attributionUrl(source, uploadAttribution);

    const QJsonArray content = body.value(QStringLiteral("Content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject entry = value.toObject();

        // Community syncs nest the syllables under Lead. Apple Music and Spotify
        // put the whole line on the entry itself, with no Lead at all, so both
        // shapes have to be read or the lyrics look missing.
        QJsonObject part = entry.value(QStringLiteral("Lead")).toObject();
        if (part.isEmpty()) {
            part = entry;
        }

        // A line can carry background vocals in a separate part; the panel is
        // one line of text, so only the lead is used.
        LyricLine line;
        if (!readPart(part, &line)) {
            continue;
        }

        lyrics->lines.append(line);
    }

    if (lyrics->lines.isEmpty()) {
        return fail(QStringLiteral("no usable lines"));
    }

    std::sort(lyrics->lines.begin(), lyrics->lines.end(), [](const LyricLine &lhs, const LyricLine &rhs) {
        return lhs.startMs < rhs.startMs;
    });

    return true;
}