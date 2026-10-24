/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcredprovider.h"
#include "lrcparser.h"
#include "lrclib.h"
#include "trackname.h"
#include "ttmlparser.h"

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

namespace
{
constexpr qint64 kLrcRedCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 12000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.7.1 (https://github.com/skippingclass/plasma-lrc; contact: skippingclass@github)");
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
    m_targetDurationMs = meta.durationMs;
    m_currentCacheKey = makeCacheKey(meta.title, meta.artist, meta.durationMs, meta.isrc);

    // Check negative cache
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_negativeCache.contains(m_currentCacheKey)) {
        if (now - m_negativeCache.value(m_currentCacheKey) < kNegativeCacheTtlMs) {
            const quint64 gen = m_currentGeneration;
            m_currentCacheKey.clear();
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
    splitTrackArtistAndTitle(&artist, &title, meta.isWebSession);
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

        const quint64 gen = m_currentGeneration;
        m_negativeCache.insert(m_currentCacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        const MissingReason reason = (statusCode == 404) ? MissingReason::NotFound : MissingReason::Unreachable;
        Q_EMIT lyricsFailed(gen, reason, id());
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
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QJsonArray hits = root.value(QStringLiteral("hits")).toArray();

    if (hits.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        m_negativeCache.insert(m_currentCacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    // Pick best hit matching duration (if known) or first hit with word sync
    QJsonObject bestHit;
    bool found = false;
    double minDurDiff = 999999.0;

    for (const auto &val : hits) {
        const QJsonObject obj = val.toObject();
        const double candDur = obj.value(QStringLiteral("duration")).toDouble(0.0);
        if (m_targetDurationMs > 0) {
            if (candDur > 0.0) {
                if (LrclibClient::isDurationCompatible(candDur, m_targetDurationMs)) {
                    const double diff = qAbs(candDur - (m_targetDurationMs / 1000.0));
                    if (diff < minDurDiff) {
                        minDurDiff = diff;
                        bestHit = obj;
                        found = true;
                    }
                }
            } else if (!found && bestHit.isEmpty()) {
                // Keep candidate with unknown duration as fallback only if no compatible candidate found
                bestHit = obj;
            }
        } else if (!found) {
            bestHit = obj;
            found = true;
        }
    }

    if (!found) {
        if (m_targetDurationMs > 0 && !bestHit.isEmpty() && bestHit.value(QStringLiteral("duration")).toDouble(0.0) <= 0.0) {
            found = true;
        }
    }

    if (!found) {
        const quint64 gen = m_currentGeneration;
        m_negativeCache.insert(m_currentCacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonObject files = bestHit.value(QStringLiteral("files")).toObject();
    const QString ttmlUrl = files.value(QStringLiteral("ttml")).toString();
    const QString lrcUrl = files.value(QStringLiteral("lrc")).toString();
    const QString isrc = bestHit.value(QStringLiteral("isrc")).toString();

    if (!ttmlUrl.isEmpty()) {
        startFileDownload(QUrl(ttmlUrl), true, isrc);
    } else if (!lrcUrl.isEmpty()) {
        startFileDownload(QUrl(lrcUrl), false, isrc);
    } else if (!isrc.isEmpty()) {
        QString directUrl = m_apiBase;
        if (!directUrl.endsWith(QLatin1Char('/'))) {
            directUrl.append(QLatin1Char('/'));
        }
        directUrl += QStringLiteral("s/%1.ttml").arg(isrc);
        startFileDownload(QUrl(directUrl), true, isrc);
    } else {
        const quint64 gen = m_currentGeneration;
        m_negativeCache.insert(m_currentCacheKey, QDateTime::currentMSecsSinceEpoch());
        m_currentCacheKey.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
    }
}

void LrcRedProvider::handleFileReply(const QByteArray &data, bool isTtml)
{
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    m_currentCacheKey.clear();

    Lyrics lyrics;
    bool ok = false;
    const QString text = QString::fromUtf8(data);

    if (isTtml) {
        ok = TtmlParser::parseTtml(text, &lyrics, m_targetDurationMs);
    }
    if (!ok) {
        // Fall back to LRC parsing
        ok = LrcParser::parseLrc(text, &lyrics, m_targetDurationMs);
    }

    if (!ok || !lyrics.isUsable()) {
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
