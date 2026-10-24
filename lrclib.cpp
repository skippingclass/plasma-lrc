/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrclib.h"
#include "trackname.h"

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
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.7.0 (https://github.com/skippingclass/plasma-lrc)");
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

int LrclibClient::scoreSearchMatch(const QJsonObject &cand, qint64 targetDurationMs)
{
    const QString synced = cand.value(QStringLiteral("syncedLyrics")).toString();
    const QString plain = cand.value(QStringLiteral("plainLyrics")).toString();
    if (synced.isEmpty() && plain.isEmpty()) {
        return -1;
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

bool LrclibClient::parseCandidate(const QJsonObject &cand, Lyrics *lyrics, qint64 trackDurationMs)
{
    if (!lyrics) {
        return false;
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
    m_bestCandidate = QJsonObject();
    m_bestScore = -1;
    m_hadRateLimitOrNetworkError = false;
}

void LrclibClient::cancel()
{
    ++m_generation;
    cleanupAttemptState();
    m_currentKey.clear();
}

QString LrclibClient::request(const QString &title, const QString &artist, qint64 durationMs, bool fromWebPage)
{
    cancel();

    QString baseTitle = title.trimmed();
    QString baseArtist = artist.trimmed();

    splitTrackArtistAndTitle(&baseArtist, &baseTitle, fromWebPage);

    const QString cleanedTitle = cleanTrackTitle(baseTitle);
    const QString cleanedArtist = cleanTrackArtist(baseArtist);
    const QString primaryArtist = extractPrimaryArtist(baseArtist);

    // If title is empty, return stable key and queue missing asynchronously
    if (baseTitle.isEmpty()) {
        const QString emptyKey = makeCacheKey(QString(), baseArtist, durationMs);
        m_currentKey = emptyKey;
        const quint64 gen = m_generation;
        QTimer::singleShot(0, this, [this, gen, key = emptyKey] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                finishWithMissing(/*writeNegativeCache=*/false);
            }
        });
        return emptyKey;
    }

    m_currentKey = makeCacheKey(baseTitle, baseArtist, durationMs);
    m_currentDurationMs = durationMs;
    const QString reqKey = m_currentKey;
    const quint64 gen = m_generation;

    // Check memory cache
    const auto memIt = m_memoryCache.constFind(reqKey);
    if (memIt != m_memoryCache.constEnd() && QDateTime::currentMSecsSinceEpoch() < memIt->expiresAtMs) {
        QTimer::singleShot(0, this, [this, gen, key = reqKey, lyrics = memIt->lyrics] {
            if (gen == m_generation && key == m_currentKey && !m_currentKey.isEmpty()) {
                cleanupAttemptState();
                m_currentKey.clear();
                Q_EMIT loaded(key, lyrics);
            }
        });
        return reqKey;
    }

    // Check disk cache
    const QString diskPath = cacheDirectory() + QLatin1Char('/') + reqKey + QStringLiteral(".json");
    if (QFile::exists(diskPath)) {
        QFile file(diskPath);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray payload = file.readAll();
            file.close();

            const QDateTime modified = QFileInfo(diskPath).lastModified();
            const qint64 age = modified.isValid() ? modified.msecsTo(QDateTime::currentDateTime()) : kCacheTtlMs + 1;

            if (QJsonDocument::fromJson(payload).object().value(QStringLiteral("missing")).toBool()) {
                if (age <= kNegativeCacheTtlMs) {
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
                const QJsonObject cand = QJsonDocument::fromJson(payload).object();
                Lyrics lyrics;
                if (parseCandidate(cand, &lyrics, durationMs)) {
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
            }
        }
    }

    // Build multi-tier search attempts following /home/pirkov/self/server/src/lyrics.js
    const qint64 durationSec = durationMs > 0 ? qRound(durationMs / 1000.0) : 0;

    // 1. Exact match (/api/get)
    QVector<QPair<QString, QString>> getQueries;
    if (!cleanedTitle.isEmpty() && !cleanedArtist.isEmpty()) {
        getQueries.append({cleanedTitle, cleanedArtist});
    }
    if (!cleanedTitle.isEmpty() && !primaryArtist.isEmpty() && primaryArtist != cleanedArtist) {
        getQueries.append({cleanedTitle, primaryArtist});
    }
    if (baseTitle != cleanedTitle || baseArtist != cleanedArtist) {
        if (!baseTitle.isEmpty() && !baseArtist.isEmpty()) {
            getQueries.append({baseTitle, baseArtist});
        }
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
    if (!cleanedArtist.isEmpty() || !baseArtist.isEmpty()) {
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

        // Tier 3: clean title + raw artist
        if (!cleanedTitle.isEmpty() && !baseArtist.isEmpty() && baseArtist != cleanedArtist && baseArtist != primaryArtist) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("track_name"), cleanedTitle);
            query.addQueryItem(QStringLiteral("artist_name"), baseArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 4: Fuzzy query search ?q=cleanTitle primaryArtist
        if (!cleanedTitle.isEmpty() && !primaryArtist.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), cleanedTitle + QLatin1Char(' ') + primaryArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 5: Raw fallback query search ?q=baseTitle baseArtist
        if (!baseTitle.isEmpty() && !baseArtist.isEmpty() && (baseTitle != cleanedTitle || baseArtist != primaryArtist)) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), baseTitle + QLatin1Char(' ') + baseArtist);
            url.setQuery(query);
            m_attempts.append({url, true});
        }
    } else {
        // Title-only search when artist is empty
        // Tier 1: Search by clean track_name
        if (!cleanedTitle.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("track_name"), cleanedTitle);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 2: Fuzzy search by clean title
        if (!cleanedTitle.isEmpty()) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), cleanedTitle);
            url.setQuery(query);
            m_attempts.append({url, true});
        }

        // Tier 3: Search by base title if different
        if (!baseTitle.isEmpty() && baseTitle != cleanedTitle) {
            QUrl url(m_apiBase.resolved(QUrl(QStringLiteral("search"))));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), baseTitle);
            url.setQuery(query);
            m_attempts.append({url, true});
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
        // First attempt (exact match or primary search): send immediately
        executeAttempt(attempt);
    } else {
        // Subsequent attempts: throttle with 250ms delay to respect LRCLIB rate limits
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

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429 || statusCode == 503) {
        // Rate limited or server busy: mark error and do NOT record negative cache
        m_hadRateLimitOrNetworkError = true;
        finishWithMissing(/*writeNegativeCache=*/false);
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::ContentNotFoundError) {
            // Network failure, timeout, or connection reset
            m_hadRateLimitOrNetworkError = true;
        }
        // Continue to next attempt
        startNextAttempt();
        return;
    }

    const QByteArray data = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);

    if (!attempt.isSearch) {
        // /api/get: single candidate
        if (doc.isObject()) {
            const QJsonObject cand = doc.object();
            Lyrics lyrics;
            if (parseCandidate(cand, &lyrics, m_currentDurationMs)) {
                finishWithCandidate(cand);
                return;
            }
        }
    } else {
        // /api/search: list of candidates
        if (doc.isArray()) {
            const QJsonArray list = doc.array();
            for (const QJsonValue &val : list) {
                if (!val.isObject()) continue;
                const QJsonObject cand = val.toObject();
                Lyrics lyrics;
                if (!parseCandidate(cand, &lyrics, m_currentDurationMs)) {
                    continue;
                }
                const int score = scoreSearchMatch(cand, m_currentDurationMs);
                if (score < 0) {
                    continue;
                }
                if (score > m_bestScore) {
                    m_bestScore = score;
                    m_bestCandidate = cand;

                    // If synced lyrics with duration match within 2s, virtually perfect match
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
    if (parseCandidate(cand, &lyrics, m_currentDurationMs)) {
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

void LrclibClient::finishWithMissing(bool writeNegativeCache)
{
    const QString key = m_currentKey;

    if (writeNegativeCache && !key.isEmpty()) {
        // Write negative cache only on clean non-matches
        QDir().mkpath(cacheDirectory());
        QFile file(cacheDirectory() + QLatin1Char('/') + key + QStringLiteral(".json"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("{\"missing\":true}");
        }
    }

    cleanupAttemptState();
    m_currentKey.clear();
    Q_EMIT missing(key);
}
