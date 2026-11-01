/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrclib.h"
#include "trackmatch.h"
#include "trackname.h"
#include "lyriclimits.h"

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
#include <QTimer>
#include <QUrlQuery>

namespace
{
constexpr qint64 kCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 30LL * 60 * 1000; // 30 minutes instead of 24 hours
constexpr int kRequestTimeoutMs = 5000;
constexpr int kSearchThrottleDelayMs = 250;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.9.0 (https://github.com/skippingclass/plasma-lrc)");
}

LrclibClient::LrclibClient(QObject *parent, const QUrl &apiBase)
    : QObject(parent)
    , m_apiBase(apiBase)
{
}

LrclibClient::~LrclibClient()
{
    cancel();
}

QByteArray LrclibClient::userAgent()
{
    return s_userAgent;
}

QString LrclibClient::cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/plasma-lrc/lrclib");
}

void LrclibClient::clearCache()
{
    m_memoryCache.clear();
    const QString dir = cacheDirectory();
    if (QDir(dir).exists()) {
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
    }
}

QString LrclibClient::makeCacheKey(const QString &title, const QString &artist, qint64 durationMs)
{
    const QString raw = QStringLiteral("%1|%2|%3").arg(title.trimmed().toLower(), artist.trimmed().toLower(), QString::number(durationMs / 1000));
    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex());
}

double LrclibClient::maxAllowedDurationDiffSec(qint64 targetDurationMs)
{
    if (targetDurationMs <= 0) {
        return -1.0;
    }
    const double targetSec = targetDurationMs / 1000.0;
    // Reasonable threshold: max(10s, 5% of target).
    // For standard tracks (~200s), 5% is 10s floor, accommodating silence trimming and single edits.
    // For long tracks (>200s), 5% accounts for extended intros/outros (e.g. 30s on a 10min track)
    // while definitively rejecting multi-song medleys or unrelated audio versions.
    return qMax(10.0, targetSec * 0.05);
}

bool LrclibClient::isDurationCompatible(double candDurationSec, qint64 targetDurationMs)
{
    if (targetDurationMs <= 0 || candDurationSec <= 0) {
        return true;
    }
    const double targetSec = targetDurationMs / 1000.0;
    const double diff = qAbs(candDurationSec - targetSec);
    const double maxDiff = maxAllowedDurationDiffSec(targetDurationMs);
    return diff <= maxDiff;
}

bool LrclibClient::validateCandidateIdentity(const QJsonObject &cand, const QString &expectedTitle, const QString &expectedArtist)
{
    const QString expTitle = expectedTitle.trimmed();
    const QString expArtist = expectedArtist.trimmed();
    if (expTitle.isEmpty() && expArtist.isEmpty()) {
        return true;
    }

    QString candTitle = cand.value(QStringLiteral("trackName")).toString();
    if (candTitle.isEmpty()) {
        candTitle = cand.value(QStringLiteral("name")).toString();
    }
    candTitle = candTitle.trimmed();

    QString candArtist = cand.value(QStringLiteral("artistName")).toString();
    if (candArtist.isEmpty()) {
        candArtist = cand.value(QStringLiteral("artist")).toString();
    }
    candArtist = candArtist.trimmed();

    // Strict identity validation: missing title or missing artist when expected fails
    if (!expTitle.isEmpty()) {
        if (candTitle.isEmpty() || !TrackMatch::matchTitles(candTitle, expTitle)) {
            return false;
        }
    }

    if (!expArtist.isEmpty()) {
        if (candArtist.isEmpty() || !TrackMatch::matchArtists(candArtist, expArtist)) {
            return false;
        }
    }

    return true;
}

int LrclibClient::scoreSearchMatch(const QJsonObject &cand, qint64 targetDurationMs, const QString &expectedTitle, const QString &expectedArtist)
{
    const QString synced = cand.value(QStringLiteral("syncedLyrics")).toString();
    const QString plain = cand.value(QStringLiteral("plainLyrics")).toString();
    if (synced.isEmpty() && plain.isEmpty()) {
        return -1;
    }

    if (!expectedTitle.trimmed().isEmpty() || !expectedArtist.trimmed().isEmpty()) {
        if (!validateCandidateIdentity(cand, expectedTitle, expectedArtist)) {
            return -1;
        }
    }

    const double candDurationSec = cand.value(QStringLiteral("duration")).toDouble(-1.0);
    if (targetDurationMs > 0 && candDurationSec > 0) {
        if (!isDurationCompatible(candDurationSec, targetDurationMs)) {
            return -1;
        }
    }

    int score = 0;
    if (!synced.isEmpty()) {
        score += 50;
    }
    if (!plain.isEmpty()) {
        score += 20;
    }

    if (targetDurationMs > 0 && candDurationSec > 0) {
        const double targetSec = targetDurationMs / 1000.0;
        const double diff = qAbs(candDurationSec - targetSec);
        if (diff <= 2.0) score += 35;
        else if (diff <= 5.0) score += 25;
        else if (diff <= 10.0) score += 15;
        else score += 5;
    }
    return score;
}

bool LrclibClient::parseCandidate(const QJsonObject &cand, Lyrics *lyrics, qint64 trackDurationMs, const QString &expectedTitle, const QString &expectedArtist)
{
    if (!lyrics) {
        return false;
    }

    if (!expectedTitle.trimmed().isEmpty() || !expectedArtist.trimmed().isEmpty()) {
        if (!validateCandidateIdentity(cand, expectedTitle, expectedArtist)) {
            return false;
        }
    }

    const double candDurationSec = cand.value(QStringLiteral("duration")).toDouble(-1.0);
    if (trackDurationMs > 0 && candDurationSec > 0) {
        if (!isDurationCompatible(candDurationSec, trackDurationMs)) {
            return false;
        }
    }
    const QString synced = cand.value(QStringLiteral("syncedLyrics")).toString();
    if (!synced.isEmpty()) {
        if (LrcParser::parseLrc(synced, lyrics, trackDurationMs) && lyrics->isUsable()) {
            lyrics->attributionData.provider = QStringLiteral("lrclib");
            lyrics->attributionData.providerUrl = QStringLiteral("https://lrclib.net");
            lyrics->attributionData.contributors.clear();
            lyrics->attribution = QStringLiteral("lrclib");
            lyrics->attributionUrl = QStringLiteral("https://lrclib.net");
            return true;
        }
    }
    return false;
}

void LrclibClient::cleanupAttemptState()
{
    if (m_activeReply) {
        m_activeReply->disconnect(this);
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }
    m_attempts.clear();
    m_attemptIndex = 0;
    m_currentDurationMs = 0;
    m_expectedTitle.clear();
    m_expectedArtist.clear();
    m_bestCandidate = QJsonObject();
    m_bestScore = -1;
    m_hadRateLimitOrNetworkError = false;
}

void LrclibClient::cancel()
{
    ++m_generation;
    cleanupAttemptState();
    m_currentKey.clear();
    cancelCandidateSearch(0);
    cancelCandidateFetch(0);
}

void LrclibClient::invalidate(const QString &title, const QString &artist, qint64 durationMs, bool fromWebPage)
{
    TrackMetadata meta;
    meta.title = title;
    meta.artist = artist;
    meta.durationMs = durationMs;
    meta.isWebSession = fromWebPage;
    invalidate(meta);
}

void LrclibClient::invalidate(const TrackMetadata &meta)
{
    TrackMetadata normMeta = meta;
    normalizeTrackMetadata(&normMeta);

    const QString key = makeCacheKey(normMeta.title, normMeta.artist, normMeta.durationMs);
    if (key.isEmpty()) {
        return;
    }

    if (m_currentKey == key) {
        cancel();
    }

    m_memoryCache.remove(key);

    const QString diskPath = cacheDirectory() + QLatin1Char('/') + key + QStringLiteral(".json");
    if (QFile::exists(diskPath)) {
        QFile::remove(diskPath);
    }
}

QString LrclibClient::request(const QString &title, const QString &artist, qint64 durationMs, bool fromWebPage)
{
    TrackMetadata meta;
    meta.title = title;
    meta.artist = artist;
    meta.durationMs = durationMs;
    meta.isWebSession = fromWebPage;
    return request(meta);
}

QString LrclibClient::request(const TrackMetadata &meta)
{
    cancel();

    TrackMetadata normMeta = meta;
    normalizeTrackMetadata(&normMeta);

    // If title is empty, return stable key and queue missing asynchronously
    if (normMeta.title.isEmpty()) {
        const QString emptyKey = makeCacheKey(QString(), normMeta.artist, normMeta.durationMs);
        m_currentKey = emptyKey;
        const quint64 gen = m_generation;
        QTimer::singleShot(0, this, [this, gen, key = emptyKey] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                finishWithMissing(/*writeNegativeCache=*/false);
            }
        });
        return emptyKey;
    }

    m_currentKey = makeCacheKey(normMeta.title, normMeta.artist, normMeta.durationMs);
    m_currentDurationMs = normMeta.durationMs;
    m_expectedTitle = normMeta.effectiveExpectedTitle();
    m_expectedArtist = normMeta.effectiveExpectedArtist();
    const QString reqKey = m_currentKey;
    const quint64 gen = m_generation;

    // Check memory cache
    const auto memIt = m_memoryCache.constFind(reqKey);
    if (memIt != m_memoryCache.constEnd() && QDateTime::currentMSecsSinceEpoch() < memIt->expiresAtMs) {
        m_lastCacheStatus = QStringLiteral("Hit (memory)");
        m_lastRejectionReason.clear();
        QTimer::singleShot(0, this, [this, gen, key = reqKey, lyrics = memIt->lyrics] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                cleanupAttemptState();
                m_currentKey.clear();
                Q_EMIT loaded(key, lyrics);
            }
        });
        return reqKey;
    }

    // Check disk cache with strict candidate identity validation
    const QString diskPath = cacheDirectory() + QLatin1Char('/') + reqKey + QStringLiteral(".json");
    if (QFile::exists(diskPath)) {
        if (QFileInfo(diskPath).size() > LyricsLimits::kMaxInputSizeBytes) {
            QFile::remove(diskPath);
        } else {
            QFile file(diskPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray payload = file.readAll();
                file.close();

                const QDateTime modified = QFileInfo(diskPath).lastModified();
                const qint64 age = modified.isValid() ? modified.msecsTo(QDateTime::currentDateTime()) : kCacheTtlMs + 1;

                const QJsonObject diskObj = QJsonDocument::fromJson(payload).object();
                if (diskObj.value(QStringLiteral("missing")).toBool()) {
                    if (age <= kNegativeCacheTtlMs) {
                        m_lastCacheStatus = QStringLiteral("Hit (negative cache)");
                        m_lastRejectionReason = QStringLiteral("Track not found in database (cached)");
                        QTimer::singleShot(0, this, [this, gen, key = reqKey] {
                            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                                cleanupAttemptState();
                                m_currentKey.clear();
                                Q_EMIT missing(key);
                            }
                        });
                        return reqKey;
                    }
                } else if (age <= kCacheTtlMs) {
                    if (validateCandidateIdentity(diskObj, m_expectedTitle, m_expectedArtist)) {
                        Lyrics lyrics;
                        if (parseCandidate(diskObj, &lyrics, m_currentDurationMs, m_expectedTitle, m_expectedArtist)) {
                            m_lastCacheStatus = QStringLiteral("Hit (disk)");
                            m_lastRejectionReason.clear();
                            m_memoryCache.insert(reqKey, {lyrics, modified.toMSecsSinceEpoch() + kCacheTtlMs});
                            QTimer::singleShot(0, this, [this, gen, key = reqKey, lyrics] {
                                if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                                    cleanupAttemptState();
                                    m_currentKey.clear();
                                    Q_EMIT loaded(key, lyrics);
                                }
                            });
                            return reqKey;
                        }
                    } else {
                        // Cached candidate does not match expected identity; purge wrong entry
                        QFile::remove(diskPath);
                    }
                }
            }
        }
    }

    m_lastCacheStatus = QStringLiteral("Miss");
    m_lastRejectionReason.clear();

    // Build multi-tier search attempts using normalized metadata
    const QString cleanedTitle = normMeta.title;
    const QString cleanedArtist = normMeta.artist;
    const QString primaryArtist = extractPrimaryArtist(normMeta.artist);
    const qint64 durationSec = normMeta.durationMs > 0 ? qRound(normMeta.durationMs / 1000.0) : 0;

    // 1. Exact match (/api/get)
    QVector<QPair<QString, QString>> getQueries;
    if (!cleanedTitle.isEmpty() && !cleanedArtist.isEmpty()) {
        getQueries.append({cleanedTitle, cleanedArtist});
    }
    if (!cleanedTitle.isEmpty() && !primaryArtist.isEmpty() && primaryArtist != cleanedArtist) {
        getQueries.append({cleanedTitle, primaryArtist});
    }

    for (const auto &pair : getQueries) {
        QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("get"))));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("track_name"), pair.first);
        query.addQueryItem(QStringLiteral("artist_name"), pair.second);
        if (durationSec > 0) {
            query.addQueryItem(QStringLiteral("duration"), QString::number(durationSec));
        }
        url.setQuery(query);
        m_attempts.append({url, false});
    }

    // 2. Multi-tier search fallback (/api/search)
    if (!cleanedArtist.isEmpty()) {
        // Tier 1: clean title + clean artist
        if (!cleanedTitle.isEmpty() && !cleanedArtist.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("track_name"), cleanedTitle);
            query.addQueryItem(QStringLiteral("artist_name"), cleanedArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 2: clean title + primary artist
        if (!cleanedTitle.isEmpty() && !primaryArtist.isEmpty() && primaryArtist != cleanedArtist) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("track_name"), cleanedTitle);
            query.addQueryItem(QStringLiteral("artist_name"), primaryArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 3: Fuzzy query search ?q=cleanTitle primaryArtist
        if (!cleanedTitle.isEmpty() && !primaryArtist.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), cleanedTitle + QLatin1Char(' ') + primaryArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }
    } else {
        // Title-only search when artist is empty
        if (!cleanedTitle.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("track_name"), cleanedTitle);
            url.setQuery(query);
            m_attempts.append({url, true});

            QUrl urlFuzzy(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery queryFuzzy;
            queryFuzzy.addQueryItem(QStringLiteral("q"), cleanedTitle);
            urlFuzzy.setQuery(queryFuzzy);
            m_attempts.append({urlFuzzy, true});
        }
    }

    m_attemptIndex = 0;
    m_bestCandidate = QJsonObject();
    m_bestScore = -1;

    if (m_attempts.isEmpty()) {
        QTimer::singleShot(0, this, [this, gen, key = reqKey] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                finishWithMissing(/*writeNegativeCache=*/false);
            }
        });
        return reqKey;
    }

    startNextAttempt();
    return reqKey;
}

void LrclibClient::startNextAttempt()
{
    const quint64 gen = m_generation;
    if (m_attemptIndex >= m_attempts.size()) {
        if (!m_bestCandidate.isEmpty() && m_bestScore > 0) {
            finishWithCandidate(m_bestCandidate);
        } else {
            finishWithMissing(!m_hadRateLimitOrNetworkError);
        }
        return;
    }

    const SearchAttempt attempt = m_attempts.at(m_attemptIndex++);
    if (m_attemptIndex == 1) {
        executeAttempt(attempt);
    } else {
        QTimer::singleShot(kSearchThrottleDelayMs, this, [this, attempt, gen, key = m_currentKey] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                executeAttempt(attempt);
            }
        });
    }
}

void LrclibClient::executeAttempt(const SearchAttempt &attempt)
{
    const quint64 gen = m_generation;
    QNetworkRequest request(attempt.url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", s_userAgent);
    request.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply *reply = m_network.get(request);
    LyricsLimits::attachSizeLimitGuard(reply);
    m_activeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt, gen] {
        if (gen != m_generation) {
            reply->deleteLater();
            return;
        }
        if (m_activeReply == reply) {
            m_activeReply.clear();
        }
        handleReply(reply, attempt);
    });
}

void LrclibClient::handleReply(QNetworkReply *reply, const SearchAttempt &attempt)
{
    reply->deleteLater();

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        m_hadRateLimitOrNetworkError = true;
        m_lastRejectionReason = QStringLiteral("Response size limit exceeded (max 8 MiB)");
        finishWithMissing(/*writeNegativeCache=*/false, MissingReason::SizeLimitExceeded);
        return;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429) {
        m_hadRateLimitOrNetworkError = true;
        m_lastRejectionReason = QStringLiteral("Rate limit reached (HTTP 429)");
        finishWithMissing(/*writeNegativeCache=*/false);
        return;
    }
    if (statusCode >= 500) {
        m_hadRateLimitOrNetworkError = true;
        m_lastRejectionReason = QStringLiteral("HTTP server error (%1)").arg(statusCode);
        finishWithMissing(/*writeNegativeCache=*/false);
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::ContentNotFoundError) {
            m_hadRateLimitOrNetworkError = true;
            m_lastRejectionReason = QStringLiteral("Network error (%1)").arg(reply->errorString());
        }
        startNextAttempt();
        return;
    }

    const QByteArray data = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);

    if (!attempt.isSearch) {
        // /api/get: single candidate
        if (doc.isObject()) {
            const QJsonObject cand = doc.object();
            // Defensively validate candidate identity even for /get
            if (validateCandidateIdentity(cand, m_expectedTitle, m_expectedArtist)) {
                Lyrics lyrics;
                if (parseCandidate(cand, &lyrics, m_currentDurationMs, m_expectedTitle, m_expectedArtist)) {
                    finishWithCandidate(cand);
                    return;
                }
            }
        }
    } else {
        // /api/search: list of candidates
        if (doc.isArray()) {
            const QJsonArray list = doc.array();
            for (const QJsonValue &val : list) {
                if (!val.isObject()) continue;
                const QJsonObject cand = val.toObject();

                // Strict candidate identity verification: never match solely on duration
                if (!validateCandidateIdentity(cand, m_expectedTitle, m_expectedArtist)) {
                    continue;
                }

                Lyrics lyrics;
                if (!parseCandidate(cand, &lyrics, m_currentDurationMs, m_expectedTitle, m_expectedArtist)) {
                    continue;
                }

                const int score = scoreSearchMatch(cand, m_currentDurationMs, m_expectedTitle, m_expectedArtist);
                if (score < 0) {
                    continue;
                }

                if (score > m_bestScore) {
                    m_bestScore = score;
                    m_bestCandidate = cand;

                    // If synced lyrics with duration match within 2s and identity matches, virtually perfect match
                    const double candDur = cand.value(QStringLiteral("duration")).toDouble(-1.0);
                    if (m_currentDurationMs > 0 && candDur > 0 && qAbs(candDur - (m_currentDurationMs / 1000.0)) <= 2.0) {
                        finishWithCandidate(m_bestCandidate);
                        return;
                    }
                }
            }
            if (m_bestScore >= 70) {
                finishWithCandidate(m_bestCandidate);
                return;
            }
        }
    }

    startNextAttempt();
}

void LrclibClient::finishWithCandidate(const QJsonObject &cand)
{
    const QString key = m_currentKey;
    Lyrics lyrics;
    if (parseCandidate(cand, &lyrics, m_currentDurationMs, m_expectedTitle, m_expectedArtist)) {
        m_memoryCache.insert(key, {lyrics, QDateTime::currentMSecsSinceEpoch() + kCacheTtlMs});

        // Write disk cache
        QDir().mkpath(cacheDirectory());
        QFile file(cacheDirectory() + QLatin1Char('/') + key + QStringLiteral(".json"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(cand).toJson(QJsonDocument::Compact));
        }

        cleanupAttemptState();
        m_currentKey.clear();
        Q_EMIT loaded(key, lyrics);
    } else {
        finishWithMissing(!m_hadRateLimitOrNetworkError);
    }
}

void LrclibClient::finishWithMissing(bool writeNegativeCache, MissingReason reason)
{
    const QString key = m_currentKey;

    if (m_lastRejectionReason.isEmpty()) {
        m_lastRejectionReason = (reason == MissingReason::SizeLimitExceeded)
            ? QStringLiteral("Response size limit exceeded (max 8 MiB)")
            : QStringLiteral("Track not found in database");
    }

    if (writeNegativeCache && !key.isEmpty()) {
        QDir().mkpath(cacheDirectory());
        QFile file(cacheDirectory() + QLatin1Char('/') + key + QStringLiteral(".json"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("{\"missing\":true}");
        }
    }

    cleanupAttemptState();
    m_currentKey.clear();
    Q_EMIT missing(key, reason);
}

void LrclibClient::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    QUrl url = m_apiBase.resolved(QUrl(QStringLiteral("search")));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("track_name"), meta.title);
    if (!meta.artist.isEmpty()) {
        q.addQueryItem(QStringLiteral("artist_name"), meta.artist);
    }
    url.setQuery(q);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", s_userAgent);
    request.setTransferTimeout(kRequestTimeoutMs);

    if (m_candidateSearchReply) {
        m_candidateSearchReply->abort();
        m_candidateSearchReply->deleteLater();
        m_candidateSearchReply = nullptr;
    }

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
            Q_EMIT candidatesFound(searchId, {});
            return;
        }

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        QList<LyricCandidate> result;

        const QString expTitle = meta.effectiveExpectedTitle();
        const QString expArtist = meta.effectiveExpectedArtist();

        if (doc.isArray()) {
            const QJsonArray arr = doc.array();
            for (const QJsonValue &v : arr) {
                if (result.size() >= 10) {
                    break;
                }
                const QJsonObject obj = v.toObject();
                if (!validateCandidateIdentity(obj, expTitle, expArtist)) {
                    continue;
                }
                const double durSec = obj.value(QStringLiteral("duration")).toDouble(0.0);
                if (!isDurationCompatible(durSec, meta.durationMs)) {
                    continue;
                }

                LyricCandidate cand;
                cand.id = QString::number(obj.value(QStringLiteral("id")).toVariant().toLongLong());
                cand.providerId = QStringLiteral("lrclib");
                cand.sourceName = QStringLiteral("LRCLIB");
                cand.title = obj.value(QStringLiteral("trackName")).toString();
                cand.artist = obj.value(QStringLiteral("artistName")).toString();
                cand.durationMs = qRound(durSec * 1000.0);
                const bool hasSynced = !obj.value(QStringLiteral("syncedLyrics")).toString().trimmed().isEmpty();
                const bool hasPlain = !obj.value(QStringLiteral("plainLyrics")).toString().trimmed().isEmpty();
                cand.syncType = hasSynced ? CandidateSyncType::Line : (hasPlain ? CandidateSyncType::None : CandidateSyncType::Unknown);
                result.append(cand);
            }
        }
        Q_EMIT candidatesFound(searchId, result);
    });
}

void LrclibClient::fetchCandidate(const QString &candidateId, quint64 fetchId)
{
    if (m_candidateFetchReply) {
        m_candidateFetchReply->abort();
        m_candidateFetchReply->deleteLater();
        m_candidateFetchReply = nullptr;
    }

    QUrl url = m_apiBase.resolved(QUrl(QStringLiteral("get/") + candidateId));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", s_userAgent);
    request.setTransferTimeout(kRequestTimeoutMs);

    m_candidateFetchId = fetchId;
    m_candidateFetchReply = m_network.get(request);
    LyricsLimits::attachSizeLimitGuard(m_candidateFetchReply);

    connect(m_candidateFetchReply, &QNetworkReply::finished, this, [this, reply = m_candidateFetchReply, fetchId] {
        if (!reply) return;
        reply->deleteLater();
        if (reply == m_candidateFetchReply) {
            m_candidateFetchReply = nullptr;
        }
        if (fetchId != m_candidateFetchId) {
            return;
        }
        if (LyricsLimits::isSizeLimitExceeded(reply)) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound);
            return;
        }

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound);
            return;
        }

        Lyrics lyrics;
        if (parseCandidate(doc.object(), &lyrics)) {
            Q_EMIT candidateLyricsLoaded(fetchId, lyrics);
        } else {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound);
        }
    });
}

void LrclibClient::cancelCandidateSearch(quint64 searchId)
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

void LrclibClient::cancelCandidateFetch(quint64 fetchId)
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
