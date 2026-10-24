/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "spicylyrics.h"

#include <algorithm>
#include <limits>

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

const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.7.2 (Plasma 6 panel widget)");

bool isSuspensionCode(const QString &code)
{
    return code == QLatin1String("application_suspended")
        || code == QLatin1String("account_suspended")
        || code == QLatin1String("template_suspended");
}

// The terms require stored responses to be refetched or thrown away within this
// many days, so that a withdrawn sync actually disappears.
constexpr qint64 kCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
// Fallback line-level syncs (e.g. Apple Music) are refreshed every 2 days instead
// of a month so that newly added community syllable syncs are picked up quickly.
constexpr qint64 kLineSyncCacheTtlMs = 2LL * 24 * 60 * 60 * 1000;
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
/// How far back to look when the newest line has already ended.
constexpr int kLineLookBack = 8;
constexpr int kRequestTimeoutMs = 15000;

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
        word.startMs = qMax<qint64>(0, secondsToMs(syllable.value(QStringLiteral("StartTime"))));
        word.endMs = qMax<qint64>(word.startMs, secondsToMs(syllable.value(QStringLiteral("EndTime"))));

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
        word.startMs = qMax<qint64>(0, secondsToMs(part.value(QStringLiteral("StartTime"))));
        word.endMs = qMax<qint64>(word.startMs, secondsToMs(part.value(QStringLiteral("EndTime"))));
        word.textEnd = static_cast<int>(text.size());
        built = text;
        line->words.append(word);
    }

    // Whitespace at the edges is noise: it would show up as a gap between the
    // lyric and the credit in the panel.
    built = built.trimmed();
    QVector<LyricWord> words;
    QVector<bool> trimmedStarts;
    words.reserve(line->words.size());
    for (int i = 0; i < line->words.size(); ++i) {
        const LyricWord &word = line->words.at(i);
        if (word.textStart >= built.size()) {
            continue;
        }
        LyricWord trimmed = word;
        trimmed.textEnd = qMin(trimmed.textEnd, static_cast<int>(built.size()));
        words.append(trimmed);
        // The flags travel with the words, or the grouping below would read the
        // flag of a word that is no longer there.
        trimmedStarts.append(i < splitStarts.size() ? splitStarts.at(i) : false);
    }
    line->words = words;
    splitStarts = trimmedStarts;

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

    // The last piece of a line often carries the continuation flag with nothing
    // behind it: the flag says how the word continues, and there is no next
    // piece. Left alone, that word keeps empty bounds and can never light up,
    // which is exactly how the last word of a line went dark.
    if (groupStart < line->words.size()) {
        const int end = line->words.size() - 1;
        for (int j = groupStart; j <= end; ++j) {
            line->words[j].groupStart = line->words.at(groupStart).textStart;
            line->words[j].groupEnd = line->words.at(end).textEnd;
        }
    }

    line->text = built;
    const qint64 rawStart = secondsToMs(part.value(QStringLiteral("StartTime")));
    line->startMs = (rawStart >= 0) ? rawStart : (!line->words.isEmpty() ? line->words.first().startMs : 0);
    const qint64 rawEnd = secondsToMs(part.value(QStringLiteral("EndTime")));
    line->endMs = (rawEnd > line->startMs) ? rawEnd : (!line->words.isEmpty() ? line->words.last().endMs : line->startMs);
    if (line->endMs <= line->startMs) {
        line->endMs = line->startMs + 3000;
    }
    return true;
}
}

bool SpicyLyrics::isSafeHttpUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (!trimmed.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        && !trimmed.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) {
        return false;
    }
    for (const QChar c : trimmed) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('<')
            || c == QLatin1Char('>') || c.isSpace() || c.unicode() < 32) {
            return false;
        }
    }
    const QUrl parsed(trimmed);
    if (!parsed.isValid() || parsed.isEmpty() || parsed.host().isEmpty()) {
        return false;
    }
    const QString scheme = parsed.scheme().toLower();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https"));
}

QString SpicyLyrics::escapeHtml(const QString &text)
{
    QString escaped;
    escaped.reserve(static_cast<int>(text.size() * 1.2));
    for (const QChar c : text) {
        if (c == QLatin1Char('&')) {
            escaped.append(QStringLiteral("&amp;"));
        } else if (c == QLatin1Char('<')) {
            escaped.append(QStringLiteral("&lt;"));
        } else if (c == QLatin1Char('>')) {
            escaped.append(QStringLiteral("&gt;"));
        } else if (c == QLatin1Char('"')) {
            escaped.append(QStringLiteral("&quot;"));
        } else if (c == QLatin1Char('\'')) {
            escaped.append(QStringLiteral("&#39;"));
        } else {
            escaped.append(c);
        }
    }
    return escaped;
}

LyricAttribution SpicyLyrics::parseAttribution(const QString &source, const QJsonObject &uploadAttribution)
{
    LyricAttribution attr;

    if (source == QLatin1String("spicy_lyrics")) {
        attr.provider = QStringLiteral("Spicy Lyrics");

        const QJsonObject makerObj = uploadAttribution.value(QStringLiteral("Maker")).toObject();
        const QJsonObject uploaderObj = uploadAttribution.value(QStringLiteral("Uploader")).toObject();

        const QString makerName = makerObj.value(QStringLiteral("username")).toString().trimmed();
        const QString rawMakerUrl = makerObj.value(QStringLiteral("url")).toString().trimmed();
        const QString makerUrl = isSafeHttpUrl(rawMakerUrl) ? rawMakerUrl : QString();

        const QString uploaderName = uploaderObj.value(QStringLiteral("username")).toString().trimmed();
        const QString rawUploaderUrl = uploaderObj.value(QStringLiteral("url")).toString().trimmed();
        const QString uploaderUrl = isSafeHttpUrl(rawUploaderUrl) ? rawUploaderUrl : QString();

        const bool hasMaker = !makerName.isEmpty();
        const bool hasUploader = !uploaderName.isEmpty();

        if (hasMaker && hasUploader && makerName.compare(uploaderName, Qt::CaseInsensitive) == 0 && makerUrl == uploaderUrl) {
            LyricContributor c;
            c.role = QStringLiteral("maker & uploader");
            c.name = makerName;
            c.url = makerUrl;
            attr.contributors.append(c);
        } else {
            if (hasMaker) {
                LyricContributor c;
                c.role = QStringLiteral("maker");
                c.name = makerName;
                c.url = makerUrl;
                attr.contributors.append(c);
            }
            if (hasUploader) {
                LyricContributor c;
                c.role = QStringLiteral("uploader");
                c.name = uploaderName;
                c.url = uploaderUrl;
                attr.contributors.append(c);
            }
        }
        return attr;
    }

    if (source == QLatin1String("apple_music") || source == QLatin1String("applemusic")) {
        attr.provider = QStringLiteral("Apple Music");
        return attr;
    }

    if (source == QLatin1String("spotify")) {
        attr.provider = QStringLiteral("Spotify");
        return attr;
    }

    // The terms ask for "unknown" rather than guessing a provider.
    attr.provider = QStringLiteral("unknown source");
    return attr;
}

QString SpicyLyrics::buildAttributionText(const LyricAttribution &attr)
{
    if (attr.provider.isEmpty() && attr.contributors.isEmpty()) {
        return QString();
    }
    QString text = attr.provider;
    for (const auto &c : attr.contributors) {
        if (!text.isEmpty()) {
            text += QStringLiteral(" · ");
        }
        text += QStringLiteral("%1: %2").arg(c.role, c.name);
    }
    return text;
}

QString SpicyLyrics::buildAttributionHtml(const LyricAttribution &attr)
{
    if (attr.provider.isEmpty() && attr.contributors.isEmpty()) {
        return QString();
    }
    QString html;
    if (!attr.provider.isEmpty()) {
        if (!attr.providerUrl.isEmpty() && isSafeHttpUrl(attr.providerUrl)) {
            html += QStringLiteral("<a href=\"%1\">%2</a>").arg(escapeHtml(attr.providerUrl), escapeHtml(attr.provider));
        } else {
            html += escapeHtml(attr.provider);
        }
    }
    for (const auto &c : attr.contributors) {
        if (!html.isEmpty()) {
            html += QStringLiteral(" · ");
        }
        const QString roleEscaped = escapeHtml(c.role);
        const QString nameEscaped = escapeHtml(c.name);
        if (!c.url.isEmpty() && isSafeHttpUrl(c.url)) {
            html += QStringLiteral("%1: <a href=\"%2\">%3</a>").arg(roleEscaped, escapeHtml(c.url), nameEscaped);
        } else {
            html += QStringLiteral("%1: %2").arg(roleEscaped, nameEscaped);
        }
    }
    return html;
}

QString SpicyLyrics::primaryAttributionUrl(const LyricAttribution &attr)
{
    if (!attr.providerUrl.isEmpty() && isSafeHttpUrl(attr.providerUrl)) {
        return attr.providerUrl;
    }
    for (const auto &c : attr.contributors) {
        if (!c.url.isEmpty() && isSafeHttpUrl(c.url)) {
            return c.url;
        }
    }
    return QString();
}


SpicyLyrics::SpicyLyrics(QObject *parent)
    : QObject(parent)
    , m_watchdog(new QTimer(this))
    , m_cleanupTimer(new QTimer(this))
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

    m_cleanupTimer->setSingleShot(true);
    connect(m_cleanupTimer, &QTimer::timeout, this, [this] {
        pruneCache();
    });

    pruneCache();
}

void SpicyLyrics::cancel()
{
    ++m_generation;
    m_currentTrackId.clear();
    m_watchdog->stop();
    if (m_activeReply) {
        m_activeReply->disconnect(this);
        if (m_activeReply->isRunning()) {
            m_activeReply->abort();
        }
        m_activeReply->deleteLater();
        m_activeReply.clear();
    }
}

SpicyLyrics::~SpicyLyrics()
{
    cancel();
}

void SpicyLyrics::setKey(const QString &key)
{
    const QString trimmed = key.trimmed();
    if (m_key != trimmed) {
        cancel();
        m_retryClock.invalidate();
        m_lastHttpStatus = 0;
        m_lastErrorCode.clear();
        m_key = trimmed;
    }
}

void SpicyLyrics::invalidate(const QString &trackId)
{
    cancel();
    m_memoryCache.remove(trackId);
    if (QRegularExpression(QStringLiteral("^[A-Za-z0-9]{22}$")).match(trackId).hasMatch()) {
        QFile::remove(cacheDirectory() + QLatin1Char('/') + trackId + QStringLiteral(".json"));
    }
    m_retryClock.invalidate();
    pruneCache();
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

QString SpicyLyrics::trackIdFromMpris(const QString &trackId)
{
    if (trackId.isEmpty()) {
        return QString();
    }

    // Match "/com/spotify/track/<id>", "spotify:track:<id>",
    // "https://open.spotify.com/track/<id>", or an exact 22-character "<id>".
    // Never match browser object paths like "/org/chromium/MediaPlayer2/TrackList/Track<32hex>".
    static const QRegularExpression re(
        QStringLiteral("(?:^|/com/spotify/track/|spotify:track:|open\\.spotify\\.com/track/)([A-Za-z0-9]{22})(?:$|[/?#])"));
    const QRegularExpressionMatch match = re.match(trackId);
    return match.hasMatch() ? match.captured(1) : QString();
}

void SpicyLyrics::request(const QString &trackId)
{
    cancel();

    if (trackId.isEmpty() || !isEnabled()) {
        return;
    }
    m_currentTrackId = trackId;
    load(trackId);
}

void SpicyLyrics::emitMissing(const QString &trackId, MissingReason reason)
{
    if (trackId != m_currentTrackId) {
        return;
    }
    Q_EMIT missing(trackId, reason);
}

void SpicyLyrics::load(const QString &trackId)
{
    if (m_suspended) {
        fetch(trackId);
        return;
    }

    const auto cached = m_memoryCache.constFind(trackId);
    if (cached != m_memoryCache.constEnd()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs >= cached->expiresAtMs) {
            m_memoryCache.erase(cached);
        } else {
            const Lyrics lyrics = cached->lyrics;
            const qint64 deadline = cached->expiresAtMs;
            QTimer::singleShot(0, this, [this, trackId, lyrics, deadline, generation = m_generation] {
                if (generation == m_generation && trackId == m_currentTrackId) {
                    if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() >= deadline) {
                        m_memoryCache.remove(trackId);
                        const QString p = cacheDirectory() + QLatin1Char('/') + trackId + QStringLiteral(".json");
                        QFile::remove(p);
                        load(trackId);
                        return;
                    }
                    Q_EMIT loaded(trackId, lyrics);
                }
            });
            return;
        }
    }

    const QString directory = cacheDirectory();
    const QString path = directory + QLatin1Char('/') + trackId + QStringLiteral(".json");
    if (QFile::exists(path)) {
        const QDateTime now = QDateTime::currentDateTime();
        const QDateTime modified = QFileInfo(path).lastModified();
        if (!modified.isValid() || modified > now) {
            QFile::remove(path);
            fetch(trackId);
            return;
        }

        const qint64 age = modified.msecsTo(now);
        if (age > kCacheTtlMs) {
            QFile::remove(path);
            fetch(trackId);
            return;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QFile::remove(path);
            fetch(trackId);
            return;
        }
        const QByteArray payload = file.readAll();
        file.close();

        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            QFile::remove(path);
            fetch(trackId);
            return;
        }
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("missing")).toBool()) {
            if (age > kNegativeCacheTtlMs) {
                QFile::remove(path);
                fetch(trackId);
                return;
            }
            const MissingReason reason = reasonFromMarker(payload);
            const qint64 deadline = modified.toMSecsSinceEpoch() + kNegativeCacheTtlMs;
            QTimer::singleShot(0, this, [this, trackId, reason, deadline, generation = m_generation] {
                if (generation == m_generation && trackId == m_currentTrackId) {
                    if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() >= deadline) {
                        const QString p = cacheDirectory() + QLatin1Char('/') + trackId + QStringLiteral(".json");
                        QFile::remove(p);
                        load(trackId);
                        return;
                    }
                    Q_EMIT missing(trackId, reason);
                }
            });
            return;
        }

        Lyrics lyrics;
        if (parse(payload, &lyrics, nullptr) && lyrics.isUsable()) {
            const qint64 ttl = (lyrics.type == LyricsType::Syllable) ? kCacheTtlMs : kLineSyncCacheTtlMs;
            const qint64 originalAcquiredMs = modified.toMSecsSinceEpoch();
            const qint64 validUntilMs = originalAcquiredMs + ttl;
            if (now.toMSecsSinceEpoch() >= validUntilMs || age > ttl) {
                QFile::remove(path);
                fetch(trackId);
                return;
            }

            lyrics.validUntilMs = validUntilMs;
            m_memoryCache.insert(trackId, {lyrics, validUntilMs});

            if (m_cleanupTimer) {
                const qint64 remainingMs = validUntilMs - now.toMSecsSinceEpoch();
                if (!m_cleanupTimer->isActive() || remainingMs < m_cleanupTimer->remainingTime()) {
                    const int clampedDelay = static_cast<int>(qBound(0LL, remainingMs, static_cast<qint64>(std::numeric_limits<int>::max())));
                    m_cleanupTimer->start(clampedDelay);
                }
            }

            const qint64 deadline = validUntilMs;
            QTimer::singleShot(0, this, [this, trackId, lyrics, deadline, generation = m_generation] {
                if (generation == m_generation && trackId == m_currentTrackId) {
                    if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() >= deadline) {
                        m_memoryCache.remove(trackId);
                        const QString p = cacheDirectory() + QLatin1Char('/') + trackId + QStringLiteral(".json");
                        QFile::remove(p);
                        load(trackId);
                        return;
                    }
                    Q_EMIT loaded(trackId, lyrics);
                }
            });
            return;
        }

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
        file.close();
    }

    pruneCache();
}

void SpicyLyrics::pruneCache()
{
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 nowMs = now.toMSecsSinceEpoch();
    qint64 earliestExpiryMs = std::numeric_limits<qint64>::max();

    for (auto it = m_memoryCache.begin(); it != m_memoryCache.end(); ) {
        if (nowMs >= it.value().expiresAtMs) {
            it = m_memoryCache.erase(it);
        } else {
            earliestExpiryMs = std::min(earliestExpiryMs, it.value().expiresAtMs);
            ++it;
        }
    }

    const QDir directory(cacheDirectory());
    if (directory.exists()) {
        const QFileInfoList entries = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files);
        for (const QFileInfo &entry : entries) {
            const QDateTime modified = entry.lastModified();
            if (!modified.isValid() || modified > now) {
                QFile::remove(entry.absoluteFilePath());
                continue;
            }

            const qint64 age = modified.msecsTo(now);
            if (age > kCacheTtlMs) {
                QFile::remove(entry.absoluteFilePath());
                continue;
            }

            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                QFile::remove(entry.absoluteFilePath());
                continue;
            }
            const QByteArray payload = file.readAll();
            file.close();

            qint64 ttl = kCacheTtlMs;
            const QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (!doc.isObject()) {
                QFile::remove(entry.absoluteFilePath());
                continue;
            }
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("missing")).toBool()) {
                ttl = kNegativeCacheTtlMs;
            } else {
                const QString type = obj.value(QStringLiteral("Body")).toObject().value(QStringLiteral("Type")).toString();
                if (type == QLatin1String("Line") || type == QLatin1String("Static")) {
                    ttl = kLineSyncCacheTtlMs;
                } else if (type == QLatin1String("Syllable")) {
                    ttl = kCacheTtlMs;
                } else {
                    QFile::remove(entry.absoluteFilePath());
                    continue;
                }
            }

            const qint64 expiryMs = modified.toMSecsSinceEpoch() + ttl;
            if (nowMs >= expiryMs || age > ttl) {
                QFile::remove(entry.absoluteFilePath());
            } else {
                earliestExpiryMs = std::min(earliestExpiryMs, expiryMs);
            }
        }
    }

    if (m_cleanupTimer) {
        m_cleanupTimer->stop();
        if (earliestExpiryMs != std::numeric_limits<qint64>::max()) {
            const qint64 delayMs = earliestExpiryMs - nowMs;
            const int clampedDelay = static_cast<int>(qBound(0LL, delayMs, static_cast<qint64>(std::numeric_limits<int>::max())));
            m_cleanupTimer->start(clampedDelay);
        }
    }
}

void SpicyLyrics::fetch(const QString &trackId)
{
    cancel();
    m_currentTrackId = trackId;

    // A suspended app or a rate-limited key will not recover on the next song.
    // Keep using valid cached lyrics, but avoid hammering the API for new ones.
    if (m_retryClock.isValid() && m_retryClock.elapsed() < m_retryDelayMs) {
        QTimer::singleShot(0, this, [this, trackId, generation = m_generation] {
            if (generation == m_generation) {
                emitMissing(trackId, MissingReason::Unreachable);
            }
        });
        return;
    }
    m_lastHttpStatus = 0;
    m_lastErrorCode.clear();

    QNetworkRequest request(QUrl(QString::fromUtf8(apiRoot()) + trackId));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", s_userAgent);
    request.setRawHeader("Authorization", "Bearer " + m_key.toUtf8());
    request.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply *reply = m_network.get(request);
    m_activeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, trackId, generation = m_generation] {
        if (generation != m_generation || trackId != m_currentTrackId) {
            reply->deleteLater();
            return;
        }
        if (m_activeReply == reply) {
            m_activeReply.clear();
        }
        reply->deleteLater();
        m_watchdog->stop();
        m_lastHttpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            // 404 means the API does not know the track, which is worth
            // remembering for a day. Anything else is a problem on our side or
            // theirs (no key, rate limit, no network), and retrying on the next
            // track is the right thing to do.
            const QString code = QJsonDocument::fromJson(payload).object()
                .value(QStringLiteral("Body")).toObject().value(QStringLiteral("error")).toString();
            // Only retain the machine-readable code, never response text or credentials.
            if (QRegularExpression(QStringLiteral("^[a-z0-9_]{1,64}$")).match(code).hasMatch()) {
                m_lastErrorCode = code;
            }
            if (isSuspensionCode(code)) {
                m_suspended = true;
            }
            if (m_lastHttpStatus == 401 || m_lastHttpStatus == 403 || m_lastHttpStatus == 429) {
                const qint64 retrySeconds = reply->rawHeader("Retry-After").toLongLong();
                m_retryDelayMs = m_lastHttpStatus == 429
                    ? qBound(1LL, retrySeconds > 0 ? retrySeconds : 60LL, 600LL) * 1000
                    : 600000;
                m_retryClock.start();
            }
            const bool notFound = m_lastHttpStatus == 404;
            if (notFound) {
                write(trackId, missingMarker(MissingReason::NotFound));
            }
            emitMissing(trackId, notFound ? MissingReason::NotFound : MissingReason::Unreachable);
            return;
        }

        Lyrics lyrics;
        QString parseError;
        if (!parse(payload, &lyrics, &parseError) || !lyrics.isUsable()) {
            // The reply was fine but there is nothing to time: plain text with no
            // timings, which is what Apple Music and Spotify answer for a track
            // nobody has made a sync for. Remembering it keeps us from asking
            // again on every repeat.
            const QJsonObject body = QJsonDocument::fromJson(payload).object()
                .value(QStringLiteral("Body")).toObject();
            const QString code = body.value(QStringLiteral("error")).toString();
            const bool untimed = body.value(QStringLiteral("Type")).toString() == QLatin1String("Static");
            if (untimed) {
                m_suspended = false;
                write(trackId, missingMarker(MissingReason::Unsynced));
            } else if (isSuspensionCode(code)) {
                m_suspended = true;
                m_lastErrorCode = code;
            } else {
                m_lastErrorCode = QStringLiteral("invalid_response");
            }
            emitMissing(trackId, untimed ? MissingReason::Unsynced : MissingReason::Unreachable);
            return;
        }

        const qint64 acquiredTimeMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 ttl = (lyrics.type == LyricsType::Syllable) ? kCacheTtlMs : kLineSyncCacheTtlMs;
        lyrics.validUntilMs = acquiredTimeMs + ttl;

        m_suspended = false;
        m_retryClock.invalidate();
        m_memoryCache.insert(trackId, {lyrics, lyrics.validUntilMs});
        write(trackId, payload);

        const qint64 deadline = lyrics.validUntilMs;
        if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() >= deadline) {
            emitMissing(trackId, MissingReason::Unreachable);
            return;
        }
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

    if (!lyrics) {
        return fail(QStringLiteral("null lyrics output pointer"));
    }

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
    lyrics->attributionData = parseAttribution(source, uploadAttribution);
    lyrics->attribution = buildAttributionText(lyrics->attributionData);
    lyrics->attributionUrl = primaryAttributionUrl(lyrics->attributionData);

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
