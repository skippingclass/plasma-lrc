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
#include "lyriclimits.h"

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
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.9.1 (https://github.com/skippingclass/plasma-lrc; contact: itzmeowzer@gmail.com)");
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
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return base + QStringLiteral("/plasma-lrc/lrcred");
}

QString LrcRedProvider::makeCacheKey(const QString &title, const QString &artist, qint64 durationMs, const QString &isrc,
                                     const QString &expectedTitle, const QString &expectedArtist)
{
    if (!isrc.isEmpty()) {
        return QStringLiteral("isrc_") + isrc.toLower().trimmed();
    }
    const QString expTitle = !expectedTitle.trimmed().isEmpty() ? expectedTitle.trimmed().toLower() : title.trimmed().toLower();
    const QString expArtist = !expectedArtist.trimmed().isEmpty() ? expectedArtist.trimmed().toLower() : artist.trimmed().toLower();
    const QString combined = QStringLiteral("%1\t%2\t%3\t%4\t%5")
        .arg(title.trimmed().toLower(),
             artist.trimmed().toLower(),
             QString::number(durationMs > 0 ? durationMs / 1000 : 0),
             expTitle,
             expArtist);
    return QString::fromLatin1(QCryptographicHash::hash(combined.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString LrcRedProvider::makeCacheKey(const TrackMetadata &meta)
{
    return makeCacheKey(meta.title, meta.artist, meta.durationMs, meta.isrc,
                        meta.effectiveExpectedTitle(), meta.effectiveExpectedArtist());
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
    m_currentCacheKey = makeCacheKey(meta);
    m_requestDeadline = QDeadlineTimer(kRequestTimeoutMs);
    m_candidateQueue.clear();
    m_currentCandidateIdx = 0;
    m_currentCandidateTriedTtml = false;
    m_currentCandidateTriedLrc = false;
    m_hadTransientError = false;
    m_hadUnsyncedCandidate = false;
    m_hadTruncatedCandidates = false;

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
    if (checkDiskCache(m_currentCacheKey, m_currentMeta, &cachedLyrics)) {
        const quint64 gen = m_currentGeneration;
        m_currentCacheKey.clear();
        m_diagCache = QStringLiteral("Hit (disk)");
        m_diagReason.clear();
        Q_EMIT lyricsLoaded(gen, cachedLyrics, id());
        return;
    }

    // Direct ISRC query
    if (!meta.isrc.isEmpty()) {
        CandidateItem candItem;
        candItem.isrc = meta.isrc;
        m_candidateQueue.append(candItem);
        m_currentCandidateIdx = 0;
        m_currentCandidateTriedTtml = false;
        m_currentCandidateTriedLrc = false;
        tryNextCandidateOrFormat();
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
    cancelCandidateSearch(0);
    cancelCandidateFetch(0);
    m_currentCacheKey.clear();
    m_currentGeneration = 0;
    m_currentMeta = TrackMetadata{};
    m_candidateQueue.clear();
    m_currentCandidateIdx = 0;
    m_currentCandidateTriedTtml = false;
    m_currentCandidateTriedLrc = false;
    m_hadTransientError = false;
    m_hadUnsyncedCandidate = false;
    m_hadTruncatedCandidates = false;
}

void LrcRedProvider::invalidateCache(const TrackMetadata &meta)
{
    const QVector<TrackMetadata> variants = generateSearchVariants(meta);
    QVector<TrackMetadata> allMetas = variants;
    allMetas.append(meta);

    for (const auto &v : allMetas) {
        QStringList keysToInvalidate;
        keysToInvalidate.append(makeCacheKey(v));
        keysToInvalidate.append(makeCacheKey(v.title, v.artist, v.durationMs, v.isrc));
        if (!v.isrc.isEmpty()) {
            keysToInvalidate.append(makeCacheKey(v.title, v.artist, v.durationMs, QString(),
                                                 v.effectiveExpectedTitle(), v.effectiveExpectedArtist()));
        }

        for (const QString &key : keysToInvalidate) {
            if (key.isEmpty()) {
                continue;
            }
            if (m_currentCacheKey == key) {
                cancel();
            }
            m_negativeCache.remove(key);
            QFile::remove(cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".ttml"));
            QFile::remove(cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".lrc"));
            QFile::remove(cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".meta"));
        }
    }
}

void LrcRedProvider::clearAllCaches()
{
    m_negativeCache.clear();
    const QString dir = cacheDirectory();
    if (QDir(dir).exists()) {
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
    }
    const QString legacyBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!legacyBase.isEmpty()) {
        const QString legacyPath = legacyBase + QStringLiteral("/lrcred");
        if (QDir(legacyPath).exists()) {
            QDir(legacyPath).removeRecursively();
        }
    }
}

QUrl LrcRedProvider::resolveCandidateUrl(const QString &rawUrl) const
{
    QUrl u(rawUrl);
    if (u.isRelative()) {
        return QUrl(m_apiBase).resolved(u);
    }
    return u;
}

void LrcRedProvider::startSearch(const QString &query)
{
    const qint64 remaining = m_requestDeadline.remainingTime();
    if (remaining <= 0) {
        onTimeout();
        return;
    }

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
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    m_currentReply->setProperty("action", "search");
    connect(m_currentReply, &QNetworkReply::finished, this, &LrcRedProvider::onReplyFinished);

    m_watchdog.start(qBound(500LL, remaining, static_cast<qint64>(kRequestTimeoutMs)));
}

void LrcRedProvider::startFileDownload(const QUrl &url, bool isTtml, const QString &fallbackIsrc)
{
    const qint64 remaining = m_requestDeadline.remainingTime();
    if (remaining <= 0) {
        onTimeout();
        return;
    }

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    m_currentReply->setProperty("action", "download");
    m_currentReply->setProperty("isTtml", isTtml);
    if (!fallbackIsrc.isEmpty()) {
        m_currentReply->setProperty("fallbackIsrc", fallbackIsrc);
    }
    connect(m_currentReply, &QNetworkReply::finished, this, &LrcRedProvider::onReplyFinished);

    m_watchdog.start(qBound(500LL, remaining, static_cast<qint64>(kRequestTimeoutMs)));
}

void LrcRedProvider::tryNextCandidateOrFormat()
{
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    if (cacheKey.isEmpty() || gen == 0) {
        return;
    }

    if (m_requestDeadline.hasExpired()) {
        onTimeout();
        return;
    }

    while (m_currentCandidateIdx < m_candidateQueue.size()) {
        const CandidateItem &cand = m_candidateQueue.at(m_currentCandidateIdx);

        // 1. Try TTML if available and not yet tried
        if (!cand.ttmlUrl.isEmpty() && !m_currentCandidateTriedTtml) {
            m_currentCandidateTriedTtml = true;
            startFileDownload(resolveCandidateUrl(cand.ttmlUrl), true, cand.isrc);
            return;
        }

        // 2. Try LRC if available and not yet tried
        if (!cand.lrcUrl.isEmpty() && !m_currentCandidateTriedLrc) {
            m_currentCandidateTriedLrc = true;
            startFileDownload(resolveCandidateUrl(cand.lrcUrl), false, cand.isrc);
            return;
        }

        // 3. Candidate with isrc fallback
        if (!cand.isrc.isEmpty()) {
            if (!m_currentCandidateTriedTtml) {
                m_currentCandidateTriedTtml = true;
                QString directUrl = m_apiBase;
                if (!directUrl.endsWith(QLatin1Char('/'))) {
                    directUrl.append(QLatin1Char('/'));
                }
                directUrl += QStringLiteral("s/%1.ttml").arg(cand.isrc);
                startFileDownload(QUrl(directUrl), true, cand.isrc);
                return;
            }
            if (!m_currentCandidateTriedLrc) {
                m_currentCandidateTriedLrc = true;
                QString directUrl = m_apiBase;
                if (!directUrl.endsWith(QLatin1Char('/'))) {
                    directUrl.append(QLatin1Char('/'));
                }
                directUrl += QStringLiteral("s/%1.lrc").arg(cand.isrc);
                startFileDownload(QUrl(directUrl), false, QString());
                return;
            }
        }

        // Advance to next candidate
        ++m_currentCandidateIdx;
        m_currentCandidateTriedTtml = false;
        m_currentCandidateTriedLrc = false;
    }

    // Queue exhausted
    m_currentCacheKey.clear();
    const bool canNegativeCache = !m_hadTruncatedCandidates && !m_hadTransientError;
    if (m_hadUnsyncedCandidate && !m_hadTransientError) {
        m_diagReason = m_hadTruncatedCandidates
            ? QStringLiteral("Checked candidates are unsynchronized (remaining candidates not evaluated)")
            : QStringLiteral("Lyrics are unsynchronized (static text)");
        if (canNegativeCache) {
            m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        }
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
    } else if (m_hadTransientError) {
        m_diagReason = QStringLiteral("Transient network or parsing error");
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
    } else {
        m_diagReason = m_hadTruncatedCandidates
            ? QStringLiteral("Candidate limit reached without usable lyrics (remaining candidates not evaluated)")
            : QStringLiteral("Track not found in database");
        if (canNegativeCache) {
            m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
        }
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
    }
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

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        m_diagReason = QStringLiteral("Response size limit exceeded (max 8 MiB)");
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::SizeLimitExceeded, id());
        return;
    }

    const QString action = reply->property("action").toString();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
        if (action == QLatin1String("search")) {
            m_currentCacheKey.clear();
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

        // Action is download
        if (statusCode == 404) {
            tryNextCandidateOrFormat();
        } else {
            m_hadTransientError = true;
            tryNextCandidateOrFormat();
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

    bool hadTitleMismatch = false;
    bool hadArtistMismatch = false;
    bool hadDurationMismatch = false;
    bool hadMissingFiles = false;

    QVector<CandidateItem> matchingCandidates;

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

        const QString expTitle = target.effectiveExpectedTitle();
        const QString expArtist = target.effectiveExpectedArtist();
        const qint64 candDurMs = candDur > 0.0 ? static_cast<qint64>(std::round(candDur * 1000.0)) : 0;
        if (!TrackMatch::matchTitles(candTitle, expTitle)) {
            hadTitleMismatch = true;
            continue;
        }
        if (!expArtist.trimmed().isEmpty() && !TrackMatch::matchArtists(candArtist, expArtist)) {
            hadArtistMismatch = true;
            continue;
        }
        if (!TrackMatch::matchDurations(target.durationMs, candDurMs)) {
            hadDurationMismatch = true;
            continue;
        }

        int score = 0;
        if (candTitle.compare(expTitle, Qt::CaseInsensitive) == 0) {
            score += 50;
        } else {
            score += 30;
        }
        if (!expArtist.isEmpty() && candArtist.compare(expArtist, Qt::CaseInsensitive) == 0) {
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

        CandidateItem candItem;
        candItem.ttmlUrl = ttmlUrl;
        candItem.lrcUrl = lrcUrl;
        candItem.isrc = candIsrc;
        candItem.title = candTitle;
        candItem.artist = candArtist;
        candItem.durationMs = candDurMs;
        candItem.score = score;
        candItem.durationDiff = diff;
        matchingCandidates.append(candItem);
    }

    if (matchingCandidates.isEmpty()) {
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

    std::sort(matchingCandidates.begin(), matchingCandidates.end(), [](const CandidateItem &a, const CandidateItem &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.durationDiff < b.durationDiff;
    });

    m_candidateQueue.clear();
    const int count = qMin(3, matchingCandidates.size());
    m_hadTruncatedCandidates = (matchingCandidates.size() > count);
    for (int i = 0; i < count; ++i) {
        m_candidateQueue.append(matchingCandidates.at(i));
    }
    m_currentCandidateIdx = 0;
    m_currentCandidateTriedTtml = false;
    m_currentCandidateTriedLrc = false;

    tryNextCandidateOrFormat();
}

void LrcRedProvider::handleFileReply(const QByteArray &data, bool isTtml)
{
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;

    if (cacheKey.isEmpty() || gen == 0) {
        return;
    }

    const QString text = QString::fromUtf8(data);
    const QString trimmed = text.trimmed();

    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("<html"), Qt::CaseInsensitive)) {
        m_hadTransientError = true;
        tryNextCandidateOrFormat();
        return;
    }

    if (isTtml) {
        if (!trimmed.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
            m_hadTransientError = true;
            tryNextCandidateOrFormat();
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
            m_hadTransientError = true;
            tryNextCandidateOrFormat();
            return;
        }
    } else {
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (!jsonDoc.isNull() && (jsonDoc.isObject() || jsonDoc.isArray())) {
            m_hadTransientError = true;
            tryNextCandidateOrFormat();
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

    if (!ok && lyrics.type != LyricsType::Static) {
        m_hadTransientError = true;
        tryNextCandidateOrFormat();
        return;
    }

    if (!lyrics.isUsable() || lyrics.type == LyricsType::Static) {
        m_hadUnsyncedCandidate = true;
        tryNextCandidateOrFormat();
        return;
    }

    m_currentCacheKey.clear();
    lyrics.attributionData.provider = QStringLiteral("lrc.red");
    lyrics.attributionData.providerUrl = QStringLiteral("https://lrc.red");
    lyrics.attribution = QStringLiteral("lrc.red");
    lyrics.attributionUrl = lyrics.attributionData.providerUrl;

    QString candTitle;
    QString candArtist;
    qint64 candDurMs = 0;
    if (m_currentCandidateIdx >= 0 && m_currentCandidateIdx < m_candidateQueue.size()) {
        candTitle = m_candidateQueue.at(m_currentCandidateIdx).title;
        candArtist = m_candidateQueue.at(m_currentCandidateIdx).artist;
        candDurMs = m_candidateQueue.at(m_currentCandidateIdx).durationMs;
    }
    writeDiskCache(cacheKey, data, isTtml, candTitle, candArtist, candDurMs);
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void LrcRedProvider::onTimeout()
{
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        if (m_currentReply->isRunning()) {
            m_currentReply->abort();
        }
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    m_candidateQueue.clear();
    m_currentCandidateIdx = 0;
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    m_currentCacheKey.clear();
    if (!cacheKey.isEmpty() && gen != 0) {
        m_diagReason = QStringLiteral("Request timed out");
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
    }
}

bool LrcRedProvider::checkDiskCache(const QString &cacheKey, const TrackMetadata &meta, Lyrics *outLyrics)
{
    if (!outLyrics || cacheKey.isEmpty()) {
        return false;
    }

    const QString metaPath = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".meta");
    if (QFile::exists(metaPath)) {
        QFile metaFile(metaPath);
        if (metaFile.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(metaFile.readAll()).object();
            metaFile.close();
            const QString candTitle = obj.value(QStringLiteral("title")).toString();
            const QString candArtist = obj.value(QStringLiteral("artist")).toString();
            const qint64 candDurMs = obj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
            if (!TrackMatch::matchCandidate(meta, candTitle, candArtist, candDurMs, 4000)) {
                QFile::remove(metaPath);
                QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml"));
                QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".lrc"));
                return false;
            }
        }
    } else {
        if (!meta.effectiveExpectedArtist().trimmed().isEmpty() && meta.isrc.trimmed().isEmpty()) {
            QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml"));
            QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".lrc"));
            return false;
        }
    }

    const QString ttmlPath = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
    QFileInfo ttmlInfo(ttmlPath);
    if (ttmlInfo.exists() && ttmlInfo.isFile()) {
        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - ttmlInfo.lastModified().toMSecsSinceEpoch();
        if (ageMs <= kLrcRedCacheTtlMs && ttmlInfo.size() <= LyricsLimits::kMaxInputSizeBytes) {
            QFile file(ttmlPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QString content = QString::fromUtf8(file.readAll());
                file.close();
                if (TtmlParser::parseTtml(content, outLyrics, meta.durationMs) && outLyrics->isUsable()) {
                    outLyrics->attributionData.provider = QStringLiteral("lrc.red");
                    outLyrics->attributionData.providerUrl = QStringLiteral("https://lrc.red");
                    outLyrics->attribution = QStringLiteral("lrc.red");
                    outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
                    return true;
                }
                QFile::remove(ttmlPath);
            }
        } else {
            QFile::remove(ttmlPath);
        }
    }

    const QString lrcPath = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".lrc");
    QFileInfo lrcInfo(lrcPath);
    if (lrcInfo.exists() && lrcInfo.isFile()) {
        const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - lrcInfo.lastModified().toMSecsSinceEpoch();
        if (ageMs <= kLrcRedCacheTtlMs && lrcInfo.size() <= LyricsLimits::kMaxInputSizeBytes) {
            QFile file(lrcPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QString content = QString::fromUtf8(file.readAll());
                file.close();
                if (LrcParser::parseLrc(content, outLyrics, meta.durationMs) && outLyrics->isUsable()) {
                    outLyrics->attributionData.provider = QStringLiteral("lrc.red");
                    outLyrics->attributionData.providerUrl = QStringLiteral("https://lrc.red");
                    outLyrics->attribution = QStringLiteral("lrc.red");
                    outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
                    return true;
                }
                QFile::remove(lrcPath);
            }
        } else {
            QFile::remove(lrcPath);
        }
    }

    return false;
}

void LrcRedProvider::writeDiskCache(const QString &cacheKey, const QByteArray &data, bool isTtml,
                                    const QString &candTitle, const QString &candArtist, qint64 candDurationMs)
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
    if (!candTitle.isEmpty() || !candArtist.isEmpty()) {
        QFile metaFile(dirPath + QStringLiteral("/") + cacheKey + QStringLiteral(".meta"));
        if (metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QJsonObject obj;
            obj[QStringLiteral("title")] = candTitle;
            obj[QStringLiteral("artist")] = candArtist;
            obj[QStringLiteral("durationMs")] = candDurationMs;
            metaFile.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }
    }
}

void LrcRedProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    cancelCandidateSearch(0);

    QString searchUrl = m_apiBase;
    if (!searchUrl.endsWith(QLatin1Char('/'))) {
        searchUrl.append(QLatin1Char('/'));
    }
    searchUrl += QStringLiteral("search.json");

    const QString cleanedTitle = cleanTrackTitle(meta.title);
    const QString primaryArtist = extractPrimaryArtist(cleanTrackArtist(meta.artist));
    QString queryStr = cleanedTitle;
    if (!primaryArtist.isEmpty()) {
        queryStr += QLatin1Char(' ') + primaryArtist;
    }

    QUrl url(searchUrl);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), queryStr);
    url.setQuery(q);

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_candidateSearchId = searchId;
    m_candidateSearchReply = m_network.get(request);
    LyricsLimits::attachSizeLimitGuard(m_candidateSearchReply);

    connect(m_candidateSearchReply, &QNetworkReply::finished, this, [this, reply = m_candidateSearchReply, meta, searchId] {
        if (!reply) return;
        reply->deleteLater();
        if (reply == m_candidateSearchReply) {
            m_candidateSearchReply = nullptr;
        }
        if (searchId != m_candidateSearchId) {
            return;
        }
        if (LyricsLimits::isSizeLimitExceeded(reply) || reply->error() != QNetworkReply::NoError) {
            Q_EMIT candidatesFound(searchId, {}, id());
            return;
        }

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        QList<LyricCandidate> result;

        QJsonArray hits;
        if (doc.isArray()) {
            hits = doc.array();
        } else if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("hits")).isArray()) {
                hits = obj.value(QStringLiteral("hits")).toArray();
            } else if (obj.value(QStringLiteral("results")).isArray()) {
                hits = obj.value(QStringLiteral("results")).toArray();
            }
        }

        for (const QJsonValue &val : hits) {
            if (result.size() >= 10) {
                break;
            }
            const QJsonObject cand = val.toObject();
            QString candTitle = cand.value(QStringLiteral("title")).toString();
            if (candTitle.isEmpty()) candTitle = cand.value(QStringLiteral("name")).toString();
            if (candTitle.isEmpty()) candTitle = cand.value(QStringLiteral("trackName")).toString();
            candTitle = candTitle.trimmed();

            QString candArtist = cand.value(QStringLiteral("artist")).toString();
            if (candArtist.isEmpty()) candArtist = cand.value(QStringLiteral("artistName")).toString();
            if (candArtist.isEmpty()) candArtist = cand.value(QStringLiteral("artist_name")).toString();
            candArtist = candArtist.trimmed();

            const double candDur = cand.value(QStringLiteral("duration")).toDouble(0.0);
            const QString candIsrc = cand.value(QStringLiteral("isrc")).toString().trimmed();

            const QJsonObject files = cand.value(QStringLiteral("files")).toObject();
            const QString ttmlUrl = files.value(QStringLiteral("ttml")).toString().trimmed();
            const QString lrcUrl = files.value(QStringLiteral("lrc")).toString().trimmed();

            if (!TrackMatch::matchCandidate(meta, candTitle, candArtist, qRound(candDur * 1000.0), 4000)) {
                continue;
            }

            LyricCandidate item;
            item.providerId = id();
            item.sourceName = displayName();
            item.title = candTitle;
            item.artist = candArtist;
            item.durationMs = qRound(candDur * 1000.0);
            item.syncType = CandidateSyncType::Unknown;

            if (!ttmlUrl.isEmpty()) {
                item.id = ttmlUrl;
            } else if (!lrcUrl.isEmpty()) {
                item.id = lrcUrl;
            } else if (!candIsrc.isEmpty()) {
                item.id = QStringLiteral("isrc:") + candIsrc;
            } else {
                continue;
            }

            result.append(item);
        }

        Q_EMIT candidatesFound(searchId, result, id());
    });
}

void LrcRedProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    cancelCandidateFetch(0);

    QUrl fetchUrl;
    bool isTtml = true;

    if (candidateId.startsWith(QLatin1String("isrc:"))) {
        const QString isrc = candidateId.mid(5);
        QString base = m_apiBase;
        if (!base.endsWith(QLatin1Char('/'))) base.append(QLatin1Char('/'));
        fetchUrl = QUrl(base + isrc + QStringLiteral(".ttml"));
        isTtml = true;
    } else {
        fetchUrl = QUrl(candidateId);
        if (fetchUrl.isRelative()) {
            fetchUrl = QUrl(m_apiBase).resolved(fetchUrl);
        }
        isTtml = fetchUrl.path().endsWith(QLatin1String(".ttml"), Qt::CaseInsensitive);
    }

    QNetworkRequest request{fetchUrl};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_candidateFetchId = fetchId;
    m_candidateFetchReply = m_network.get(request);
    LyricsLimits::attachSizeLimitGuard(m_candidateFetchReply);

    connect(m_candidateFetchReply, &QNetworkReply::finished, this, [this, reply = m_candidateFetchReply, isTtml, fetchId] {
        if (!reply) return;
        reply->deleteLater();
        if (reply == m_candidateFetchReply) {
            m_candidateFetchReply = nullptr;
        }
        if (fetchId != m_candidateFetchId) {
            return;
        }
        if (LyricsLimits::isSizeLimitExceeded(reply)) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            return;
        }

        const QByteArray payload = reply->readAll();
        Lyrics lyrics;
        bool limitExceeded = false;
        bool ok = false;
        if (isTtml) {
            ok = TtmlParser::parseTtml(QString::fromUtf8(payload), &lyrics, 0, &limitExceeded);
        } else {
            ok = LrcParser::parseLrc(QString::fromUtf8(payload), &lyrics, 0, &limitExceeded);
        }

        if (limitExceeded) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
            return;
        }

        if (ok && lyrics.isUsable()) {
            lyrics.attributionData.provider = QStringLiteral("lrc.red");
            lyrics.attributionData.providerUrl = QStringLiteral("https://lrc.red");
            lyrics.attribution = QStringLiteral("lrc.red");
            lyrics.attributionUrl = lyrics.attributionData.providerUrl;
            Q_EMIT candidateLyricsLoaded(fetchId, lyrics, id());
        } else {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
        }
    });
}

void LrcRedProvider::cancelCandidateSearch(quint64 searchId)
{
    if (m_candidateSearchReply && (searchId == 0 || searchId == m_candidateSearchId)) {
        m_candidateSearchId = 0;
        QPointer<QNetworkReply> reply = m_candidateSearchReply;
        m_candidateSearchReply = nullptr;
        if (reply) {
            reply->abort();
            if (reply) {
                reply->deleteLater();
            }
        }
    }
}

void LrcRedProvider::cancelCandidateFetch(quint64 fetchId)
{
    if (m_candidateFetchReply && (fetchId == 0 || fetchId == m_candidateFetchId)) {
        m_candidateFetchId = 0;
        QPointer<QNetworkReply> reply = m_candidateFetchReply;
        m_candidateFetchReply = nullptr;
        if (reply) {
            reply->abort();
            if (reply) {
                reply->deleteLater();
            }
        }
    }
}
