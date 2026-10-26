/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcredprovider.h"
#include "lrcparser.h"
#include "lrclib.h"
#include "trackmatch.h"
#include "trackname.h"
#include "ttmlparser.h"

#include <cmath>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace
{
constexpr qint64 kLrcRedCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 12000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.8.2 (https://github.com/skippingclass/plasma-lrc; contact: skippingclass@github)");
} // namespace

LrcRedProvider::LrcRedProvider(QObject *parent, const QString &apiBase)
    : LyricsProvider(parent)
    , m_apiBase(apiBase)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &LrcRedProvider::onTimeout);
}

LrcRedProvider::~LrcRedProvider()
{
    cancel();
}

QString LrcRedProvider::cacheDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/plasma-lrc");
    }
    return base + QStringLiteral("/lrcred");
}

QString LrcRedProvider::makeCacheKey(const QString &title, const QString &artist, qint64 durationMs, const QString &isrc)
{
    if (!isrc.isEmpty()) {
        return QStringLiteral("isrc_") + isrc.toLower().trimmed();
    }
    const QString combined = QStringLiteral("%1\t%2\t%3")
        .arg(title.trimmed().toLower(),
             artist.trimmed().toLower(),
             QString::number(durationMs > 0 ? durationMs / 1000 : 0));
    return QString::fromLatin1(QCryptographicHash::hash(combined.toUtf8(), QCryptographicHash::Sha1).toHex());
}

bool LrcRedProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.title.isEmpty() || !meta.isrc.isEmpty();
}

void LrcRedProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_currentGeneration = generation;
    m_diagGeneration = generation;
    m_diagReason.clear();
    m_diagCache = QStringLiteral("Miss");
    m_currentMeta = meta;
    m_targetDurationMs = meta.durationMs;
    m_currentCacheKey = makeCacheKey(meta.title, meta.artist, meta.durationMs, meta.isrc);

    // Check negative cache
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_negativeCache.contains(m_currentCacheKey)) {
        if (now - m_negativeCache.value(m_currentCacheKey) < kNegativeCacheTtlMs) {
            const quint64 gen = m_currentGeneration;
            m_currentCacheKey.clear();
            m_diagCache = QStringLiteral("Hit (negative cache)");
            m_diagReason = QStringLiteral("Track not found in database (cached)");
            Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
            return;
        }
        m_negativeCache.remove(m_currentCacheKey);
    }

    // Check disk cache
    Lyrics cachedLyrics;
    if (checkDiskCache(m_currentCacheKey, m_targetDurationMs, &cachedLyrics)) {
        const quint64 gen = m_currentGeneration;
        m_currentCacheKey.clear();
        m_diagCache = QStringLiteral("Hit (disk)");
        m_diagReason.clear();
        Q_EMIT lyricsLoaded(gen, cachedLyrics, id());
        return;
    }

    // Direct ISRC query
    if (!meta.isrc.isEmpty()) {
        QString directUrl = m_apiBase;
        if (!directUrl.endsWith(QLatin1Char('/'))) {
            directUrl.append(QLatin1Char('/'));
        }
        directUrl += QStringLiteral("s/%1.ttml").arg(meta.isrc);
        startFileDownload(QUrl(directUrl), true, meta.isrc);
        return;
    }

    // Search query
    QString artist = meta.artist;
    QString title = meta.title;
    if (!meta.searchNormalized) {
        splitTrackArtistAndTitle(&artist, &title, meta.isWebSession);
    }
    const QString cleanedTitle = cleanTrackTitle(title);
    const QString primaryArtist = extractPrimaryArtist(cleanTrackArtist(artist));

    QString queryStr = cleanedTitle;
    if (!primaryArtist.isEmpty()) {
        queryStr += QLatin1Char(' ') + primaryArtist;
    }
    startSearch(queryStr);
}

void LrcRedProvider::cancel()
{
    m_watchdog.stop();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        if (m_currentReply->isRunning()) {
            m_currentReply->abort();
        }
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    m_currentCacheKey.clear();
    m_currentGeneration = 0;
    m_currentMeta = TrackMetadata{};
}

void LrcRedProvider::invalidateCache(const TrackMetadata &meta)
{
    QStringList keysToInvalidate;
    const QString mainKey = makeCacheKey(meta.title, meta.artist, meta.durationMs, meta.isrc);
    if (!mainKey.isEmpty()) {
        keysToInvalidate.append(mainKey);
    }
    if (!meta.isrc.isEmpty() && !meta.title.isEmpty()) {
        const QString fallbackKey = makeCacheKey(meta.title, meta.artist, meta.durationMs, QString());
        if (!fallbackKey.isEmpty() && fallbackKey != mainKey) {
            keysToInvalidate.append(fallbackKey);
        }
    }

    for (const QString &key : keysToInvalidate) {
        if (m_currentCacheKey == key) {
            cancel();
        }
        m_negativeCache.remove(key);
        const QString ttmlPath = cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".ttml");
        if (QFile::exists(ttmlPath)) {
            QFile::remove(ttmlPath);
        }
        const QString lrcPath = cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".lrc");
        if (QFile::exists(lrcPath)) {
            QFile::remove(lrcPath);
        }
    }
}

void LrcRedProvider::startSearch(const QString &query)
{
    QString searchUrl = m_apiBase;
    if (!searchUrl.endsWith(QLatin1Char('/'))) {
        searchUrl.append(QLatin1Char('/'));
    }
    searchUrl += QStringLiteral("search.json");

    QUrl url(searchUrl);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    url.setQuery(q);

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    m_currentReply->setProperty("action", "search");
    connect(m_currentReply, &QNetworkReply::finished, this, &LrcRedProvider::onReplyFinished);

    m_watchdog.start(kRequestTimeoutMs);
}

void LrcRedProvider::startFileDownload(const QUrl &url, bool isTtml, const QString &fallbackIsrc)
{
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    m_currentReply->setProperty("action", "download");
    m_currentReply->setProperty("isTtml", isTtml);
    if (!fallbackIsrc.isEmpty()) {
        m_currentReply->setProperty("fallbackIsrc", fallbackIsrc);
    }
    connect(m_currentReply, &QNetworkReply::finished, this, &LrcRedProvider::onReplyFinished);

    m_watchdog.start(kRequestTimeoutMs);
}

void LrcRedProvider::onReplyFinished()
{
    m_watchdog.stop();
    if (!m_currentReply) {
        return;
    }

    QPointer<QNetworkReply> reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    if (cacheKey.isEmpty() || gen == 0) {
        return;
    }

    const QString action = reply->property("action").toString();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
        const QString fallbackIsrc = reply->property("fallbackIsrc").toString();
        const bool isTtml = reply->property("isTtml").toBool();
        if (action == QLatin1String("download") && isTtml && !fallbackIsrc.isEmpty()) {
            QString directUrl = m_apiBase;
            if (!directUrl.endsWith(QLatin1Char('/'))) {
                directUrl.append(QLatin1Char('/'));
            }
            directUrl += QStringLiteral("s/%1.lrc").arg(fallbackIsrc);
            startFileDownload(QUrl(directUrl), false, QString());
            return;
        }

        m_currentCacheKey.clear();
        // Negative cache ONLY on confirmed 404 Not Found!
        // Never on transient network errors, 429, 5xx, or status 0.
        if (statusCode == 404) {
            m_diagReason = QStringLiteral("Track not found in database (HTTP 404)");
            m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
            Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        } else if (statusCode == 429) {
            m_diagReason = QStringLiteral("Rate limit reached (HTTP 429)");
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        } else if (statusCode >= 500) {
            m_diagReason = QStringLiteral("HTTP server error (%1)").arg(statusCode);
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        } else if (reply->error() != QNetworkReply::NoError) {
            m_diagReason = QStringLiteral("Network error (%1)").arg(reply->errorString());
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        } else {
            m_diagReason = QStringLiteral("HTTP error (%1)").arg(statusCode);
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        }
        return;
    }

    const QByteArray data = reply->readAll();
    if (action == QLatin1String("search")) {
        handleSearchReply(data);
    } else {
        const bool isTtml = reply->property("isTtml").toBool();
        handleFileReply(data, isTtml);
    }
}

void LrcRedProvider::handleSearchReply(const QByteArray &data)
{
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;

    if (cacheKey.isEmpty() || gen == 0) {
        return;
    }

    if (data.trimmed().isEmpty()) {
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        // Malformed transient response (e.g. truncated JSON or HTML page with 200 status)
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray hits = root.value(QStringLiteral("hits")).toArray();

    if (hits.isEmpty()) {
        m_diagReason = QStringLiteral("Track not found in database (empty search hits)");
        m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    TrackMetadata target = m_currentMeta;
    target.durationMs = m_targetDurationMs;
    if (!m_currentMeta.searchNormalized) {
        splitTrackArtistAndTitle(&target.artist, &target.title, m_currentMeta.isWebSession);
    }
    target.title = target.title.trimmed();
    target.artist = target.artist.trimmed();

    QJsonObject bestHit;
    int bestScore = -1;
    double bestDurationDiff = 999999.0;
    bool hadTitleMismatch = false;
    bool hadArtistMismatch = false;
    bool hadDurationMismatch = false;
    bool hadMissingFiles = false;

    for (const auto &val : hits) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject cand = val.toObject();

        QString candTitle = cand.value(QStringLiteral("title")).toString();
        if (candTitle.isEmpty()) {
            candTitle = cand.value(QStringLiteral("name")).toString();
        }
        if (candTitle.isEmpty()) {
            candTitle = cand.value(QStringLiteral("trackName")).toString();
        }
        candTitle = candTitle.trimmed();

        QString candArtist = cand.value(QStringLiteral("artist")).toString();
        if (candArtist.isEmpty()) {
            candArtist = cand.value(QStringLiteral("artistName")).toString();
        }
        if (candArtist.isEmpty()) {
            candArtist = cand.value(QStringLiteral("artist_name")).toString();
        }
        if (candArtist.isEmpty()) {
            const QJsonValue artsVal = cand.value(QStringLiteral("artists"));
            if (artsVal.isArray()) {
                QStringList arts;
                for (const auto &a : artsVal.toArray()) {
                    if (a.isString()) arts.append(a.toString());
                    else if (a.isObject()) arts.append(a.toObject().value(QStringLiteral("name")).toString());
                }
                candArtist = arts.join(QLatin1String(", "));
            }
        }
        candArtist = candArtist.trimmed();

        const double candDur = cand.value(QStringLiteral("duration")).toDouble(0.0);
        const QString candIsrc = cand.value(QStringLiteral("isrc")).toString().trimmed();

        const QJsonObject files = cand.value(QStringLiteral("files")).toObject();
        const QString ttmlUrl = files.value(QStringLiteral("ttml")).toString();
        const QString lrcUrl = files.value(QStringLiteral("lrc")).toString();
        if (ttmlUrl.isEmpty() && lrcUrl.isEmpty() && candIsrc.isEmpty()) {
            hadMissingFiles = true;
            continue;
        }

        const qint64 candDurMs = candDur > 0.0 ? static_cast<qint64>(std::round(candDur * 1000.0)) : 0;
        if (!TrackMatch::matchTitles(candTitle, target.title)) {
            hadTitleMismatch = true;
            continue;
        }
        if (!target.artist.trimmed().isEmpty() && !TrackMatch::matchArtists(candArtist, target.artist)) {
            hadArtistMismatch = true;
            continue;
        }
        if (!TrackMatch::matchDurations(target.durationMs, candDurMs)) {
            hadDurationMismatch = true;
            continue;
        }

        int score = 0;
        if (candTitle.compare(target.title, Qt::CaseInsensitive) == 0) {
            score += 50;
        } else {
            score += 30;
        }
        if (candArtist.compare(target.artist, Qt::CaseInsensitive) == 0) {
            score += 30;
        } else {
            score += 15;
        }
        double diff = 999999.0;
        if (m_targetDurationMs > 0 && candDur > 0.0) {
            diff = qAbs(candDur - (m_targetDurationMs / 1000.0));
            if (diff <= 1.0) score += 30;
            else if (diff <= 2.0) score += 20;
            else if (diff <= 5.0) score += 10;
            else score += 5;
        }
        if (!candIsrc.isEmpty() && !m_currentMeta.isrc.isEmpty() && candIsrc.compare(m_currentMeta.isrc, Qt::CaseInsensitive) == 0) {
            score += 40;
        }
        if (!ttmlUrl.isEmpty()) {
            score += 10;
        }

        if (score > bestScore || (score == bestScore && diff < bestDurationDiff)) {
            bestScore = score;
            bestDurationDiff = diff;
            bestHit = cand;
        }
    }

    if (bestHit.isEmpty()) {
        if (hadTitleMismatch && !hadArtistMismatch && !hadDurationMismatch && !hadMissingFiles) {
            m_diagReason = QStringLiteral("Candidate title mismatch");
        } else if (hadArtistMismatch && !hadTitleMismatch && !hadDurationMismatch && !hadMissingFiles) {
            m_diagReason = QStringLiteral("Candidate artist mismatch");
        } else if (hadDurationMismatch && !hadTitleMismatch && !hadArtistMismatch && !hadMissingFiles) {
            m_diagReason = QStringLiteral("Candidate duration mismatch (>4s)");
        } else if (hadMissingFiles && !hadTitleMismatch && !hadArtistMismatch && !hadDurationMismatch) {
            m_diagReason = QStringLiteral("No synchronized lyrics available");
        } else {
            m_diagReason = QStringLiteral("Candidate metadata mismatch (title, artist, or duration)");
        }
        m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonObject files = bestHit.value(QStringLiteral("files")).toObject();
    const QString ttmlUrl = files.value(QStringLiteral("ttml")).toString();
    const QString lrcUrl = files.value(QStringLiteral("lrc")).toString();
    const QString isrc = bestHit.value(QStringLiteral("isrc")).toString();

    auto resolveUrl = [this](const QString &rawUrl) -> QUrl {
        QUrl u(rawUrl);
        if (u.isRelative()) {
            return QUrl(m_apiBase).resolved(u);
        }
        return u;
    };

    if (!ttmlUrl.isEmpty()) {
        startFileDownload(resolveUrl(ttmlUrl), true, isrc);
    } else if (!lrcUrl.isEmpty()) {
        startFileDownload(resolveUrl(lrcUrl), false, isrc);
    } else if (!isrc.isEmpty()) {
        QString directUrl = m_apiBase;
        if (!directUrl.endsWith(QLatin1Char('/'))) {
            directUrl.append(QLatin1Char('/'));
        }
        directUrl += QStringLiteral("s/%1.ttml").arg(isrc);
        startFileDownload(QUrl(directUrl), true, isrc);
    } else {
        m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
    }
}

void LrcRedProvider::handleFileReply(const QByteArray &data, bool isTtml)
{
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    m_currentCacheKey.clear();

    if (cacheKey.isEmpty() || gen == 0) {
        return;
    }

    const QString text = QString::fromUtf8(data);
    const QString trimmed = text.trimmed();

    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("<html"), Qt::CaseInsensitive)) {
        // Malformed transient response (HTML error page, empty data)
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    if (isTtml) {
        if (!trimmed.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
            return;
        }
        QXmlStreamReader xmlReader(trimmed);
        bool hasTt = false;
        while (!xmlReader.atEnd()) {
            xmlReader.readNext();
            if (xmlReader.isStartElement() && xmlReader.name().toString().compare(QLatin1String("tt"), Qt::CaseInsensitive) == 0) {
                hasTt = true;
            }
        }
        if (xmlReader.hasError() || !hasTt) {
            // Malformed / truncated XML transient response
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
            return;
        }
    } else {
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isNull() && (jsonDoc.isObject() || jsonDoc.isArray())) {
            // Malformed / JSON response instead of LRC
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
            return;
        }
    }

    Lyrics lyrics;
    bool ok = false;

    if (isTtml) {
        ok = TtmlParser::parseTtml(trimmed, &lyrics, m_targetDurationMs);
    } else {
        ok = LrcParser::parseLrc(trimmed, &lyrics, m_targetDurationMs);
    }

    if (!ok) {
        // Parsing error in payload -> transient Unreachable, NO negative cache
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    if (!lyrics.isUsable()) {
        // Valid text/TTML, but confirmed unsynced
        m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attributionData.provider = QStringLiteral("lrc.red");
    lyrics.attributionData.providerUrl = QStringLiteral("https://lrc.red");
    lyrics.attribution = QStringLiteral("lrc.red");
    lyrics.attributionUrl = lyrics.attributionData.providerUrl;

    writeDiskCache(cacheKey, data, isTtml);
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void LrcRedProvider::onTimeout()
{
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
    }
}

bool LrcRedProvider::checkDiskCache(const QString &cacheKey, qint64 durationMs, Lyrics *outLyrics)
{
    if (!outLyrics || cacheKey.isEmpty()) {
        return false;
    }

    const QString ttmlPath = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
    QFileInfo ttmlInfo(ttmlPath);
    if (ttmlInfo.exists() && ttmlInfo.isFile()) {
        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - ttmlInfo.lastModified().toMSecsSinceEpoch();
        if (ageMs <= kLrcRedCacheTtlMs) {
            QFile file(ttmlPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QString content = QString::fromUtf8(file.readAll());
                if (TtmlParser::parseTtml(content, outLyrics, durationMs)) {
                    outLyrics->attributionData.provider = QStringLiteral("lrc.red");
                    outLyrics->attributionData.providerUrl = QStringLiteral("https://lrc.red");
                    outLyrics->attribution = QStringLiteral("lrc.red");
                    outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
                    return true;
                }
            }
        } else {
            QFile::remove(ttmlPath);
        }
    }

    const QString lrcPath = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".lrc");
    QFileInfo lrcInfo(lrcPath);
    if (lrcInfo.exists() && lrcInfo.isFile()) {
        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - lrcInfo.lastModified().toMSecsSinceEpoch();
        if (ageMs <= kLrcRedCacheTtlMs) {
            QFile file(lrcPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QString content = QString::fromUtf8(file.readAll());
                if (LrcParser::parseLrc(content, outLyrics, durationMs)) {
                    outLyrics->attributionData.provider = QStringLiteral("lrc.red");
                    outLyrics->attributionData.providerUrl = QStringLiteral("https://lrc.red");
                    outLyrics->attribution = QStringLiteral("lrc.red");
                    outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
                    return true;
                }
            }
        } else {
            QFile::remove(lrcPath);
        }
    }

    return false;
}

void LrcRedProvider::writeDiskCache(const QString &cacheKey, const QByteArray &data, bool isTtml)
{
    if (cacheKey.isEmpty() || data.isEmpty()) {
        return;
    }
    const QString dirPath = cacheDirectory();
    QDir().mkpath(dirPath);
    const QString ext = isTtml ? QStringLiteral(".ttml") : QStringLiteral(".lrc");
    QFile file(dirPath + QStringLiteral("/") + cacheKey + ext);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(data);
    }
}
