/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "amllprovider.h"
#include "trackmatch.h"
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
#include <QXmlStreamReader>

namespace
{
constexpr qint64 kAmllCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.8.5 (https://github.com/skippingclass/plasma-lrc)");
} // namespace

AmllProvider::AmllProvider(QObject *parent, const QString &apiBase)
    : LyricsProvider(parent)
    , m_apiBase(apiBase)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &AmllProvider::onTimeout);
}

AmllProvider::~AmllProvider()
{
    cancel();
}

QString AmllProvider::cacheDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return base + QStringLiteral("/plasma-lrc/amll");
}

QString AmllProvider::makeCacheKey(const TrackMetadata &meta)
{
    if (!meta.spotifyTrackId.trimmed().isEmpty()) {
        return meta.spotifyTrackId.trimmed();
    }
    const QString combined = QStringLiteral("%1\t%2\t%3")
        .arg(meta.title.trimmed().toLower(),
             meta.artist.trimmed().toLower(),
             QString::number(meta.durationMs > 0 ? meta.durationMs / 1000 : 0));
    return QString::fromLatin1(QCryptographicHash::hash(combined.toUtf8(), QCryptographicHash::Sha1).toHex());
}

bool AmllProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.spotifyTrackId.trimmed().isEmpty() || !meta.title.trimmed().isEmpty();
}

void AmllProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_currentGeneration = generation;
    m_diagGeneration = generation;
    m_diagReason.clear();
    m_diagCache = QStringLiteral("Miss");
    m_currentMeta = meta;
    m_currentCacheKey = makeCacheKey(meta);

    // Check negative cache
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_negativeCache.contains(m_currentCacheKey)) {
        if (now - m_negativeCache.value(m_currentCacheKey) < kNegativeCacheTtlMs) {
            const quint64 gen = m_currentGeneration;
            m_currentGeneration = 0;
            m_diagCache = QStringLiteral("Hit (negative cache)");
            m_diagReason = QStringLiteral("Track not found in AMLL database (cached)");
            Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
            return;
        }
        m_negativeCache.remove(m_currentCacheKey);
    }

    // Check disk cache
    Lyrics cachedLyrics;
    if (checkDiskCache(m_currentCacheKey, meta.durationMs, &cachedLyrics)) {
        const quint64 gen = m_currentGeneration;
        m_currentGeneration = 0;
        m_diagCache = QStringLiteral("Hit (disk)");
        m_diagReason.clear();
        Q_EMIT lyricsLoaded(gen, cachedLyrics, id());
        return;
    }

    if (!meta.spotifyTrackId.trimmed().isEmpty()) {
        startGetBySpotifyId();
    } else if (!meta.title.trimmed().isEmpty()) {
        startSearchByTitleArtist();
    } else {
        const quint64 gen = m_currentGeneration;
        m_currentGeneration = 0;
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
    }
}

void AmllProvider::cancel()
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
    m_currentStep = Step::Idle;
}

void AmllProvider::invalidateCache(const TrackMetadata &meta)
{
    const QString key = makeCacheKey(meta);
    if (key.isEmpty()) {
        return;
    }

    if (m_currentCacheKey == key) {
        cancel();
    }

    m_negativeCache.remove(key);

    const QString diskPath = cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".ttml");
    if (QFile::exists(diskPath)) {
        QFile::remove(diskPath);
    }

    if (!meta.spotifyTrackId.trimmed().isEmpty()) {
        m_negativeCache.remove(meta.spotifyTrackId.trimmed());
        const QString spotifyDiskPath = cacheDirectory() + QStringLiteral("/") + meta.spotifyTrackId.trimmed() + QStringLiteral(".ttml");
        if (QFile::exists(spotifyDiskPath)) {
            QFile::remove(spotifyDiskPath);
        }
    }
}

void AmllProvider::clearAllCaches()
{
    cancel();
    m_negativeCache.clear();
    const QString dir = cacheDirectory();
    if (QDir(dir).exists()) {
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
    }
    const QString legacyBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!legacyBase.isEmpty()) {
        const QString legacyPath = legacyBase + QStringLiteral("/amll");
        if (QDir(legacyPath).exists()) {
            QDir(legacyPath).removeRecursively();
        }
    }
}

void AmllProvider::startGetBySpotifyId()
{
    m_currentStep = Step::GetBySpotifyId;
    QString base = m_apiBase;
    if (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }

    QUrl url(base + QStringLiteral("/v1/lyrics/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("spotifyId"), m_currentMeta.spotifyTrackId.trimmed());
    url.setQuery(query);

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, &AmllProvider::onReplyFinished);
    m_watchdog.start(kRequestTimeoutMs);
}

void AmllProvider::startSearchByTitleArtist()
{
    m_currentStep = Step::SearchByTitleArtist;
    QString base = m_apiBase;
    if (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }

    QUrl url(base + QStringLiteral("/v1/lyrics/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("musicName"), m_currentMeta.title.trimmed());
    if (!m_currentMeta.artist.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("artistName"), m_currentMeta.artist.trimmed());
    }
    url.setQuery(query);

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, &AmllProvider::onReplyFinished);
    m_watchdog.start(kRequestTimeoutMs);
}

void AmllProvider::startGetById(qint64 idVal, const QString &filename)
{
    m_currentStep = Step::GetById;
    QString base = m_apiBase;
    if (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }

    QUrl url(base + QStringLiteral("/v1/lyrics/get"));
    QUrlQuery query;
    if (idVal > 0) {
        query.addQueryItem(QStringLiteral("id"), QString::number(idVal));
    } else {
        query.addQueryItem(QStringLiteral("filename"), filename.trimmed());
    }
    url.setQuery(query);

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    connect(m_currentReply, &QNetworkReply::finished, this, &AmllProvider::onReplyFinished);
    m_watchdog.start(kRequestTimeoutMs);
}

void AmllProvider::failCurrent(MissingReason reason, const QString &diagReason, bool is404)
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
    const quint64 gen = m_currentGeneration;
    const QString cacheKey = m_currentCacheKey;
    m_currentGeneration = 0;
    m_currentStep = Step::Idle;

    if (is404 && !cacheKey.isEmpty()) {
        m_negativeCache.insert(cacheKey, QDateTime::currentMSecsSinceEpoch());
    }

    m_diagReason = diagReason;
    Q_EMIT lyricsFailed(gen, reason, id());
}

void AmllProvider::handlePayload(const QByteArray &data, quint64 gen)
{
    QString ttmlContent;

    // Check if response is JSON (e.g. from api.amll.dev)
    QJsonParseError jsonError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
    if (jsonError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.contains(QLatin1String("data"))) {
            const QJsonValue dataVal = root.value(QLatin1String("data"));
            if (dataVal.isObject()) {
                const QJsonObject dataObj = dataVal.toObject();
                ttmlContent = dataObj.value(QLatin1String("lyrics")).toString();
            }
        }
    } else {
        // Fallback: check if raw TTML was returned
        const QString text = QString::fromUtf8(data).trimmed();
        if (text.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
            ttmlContent = text;
        }
    }

    const QString trimmed = ttmlContent.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("<html"), Qt::CaseInsensitive)
        || !trimmed.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
        failCurrent(MissingReason::Unreachable, QStringLiteral("Malformed TTML response"));
        return;
    }

    // Check XML well-formedness
    QXmlStreamReader xmlReader(trimmed);
    while (!xmlReader.atEnd()) {
        xmlReader.readNext();
    }
    if (xmlReader.hasError()) {
        failCurrent(MissingReason::Unreachable, QStringLiteral("Invalid TTML XML syntax"));
        return;
    }

    Lyrics lyrics;
    if (!TtmlParser::parseTtml(trimmed, &lyrics, m_currentMeta.durationMs) || !lyrics.isUsable()) {
        failCurrent(MissingReason::Unsynced, QStringLiteral("No synchronized lyrics in TTML"), true);
        return;
    }

    lyrics.attributionData.provider = QStringLiteral("AMLL TTML DB");
    lyrics.attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
    lyrics.attribution = QStringLiteral("AMLL TTML DB");
    lyrics.attributionUrl = lyrics.attributionData.providerUrl;

    writeDiskCache(m_currentCacheKey, trimmed.toUtf8());

    m_currentGeneration = 0;
    m_currentStep = Step::Idle;
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void AmllProvider::onReplyFinished()
{
    m_watchdog.stop();
    if (!m_currentReply) {
        return;
    }

    QPointer<QNetworkReply> reply = m_currentReply;
    m_currentReply = nullptr;
    reply->deleteLater();

    const quint64 gen = m_currentGeneration;
    if (m_currentCacheKey.isEmpty() || gen == 0) {
        return;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError err = reply->error();
    const Step step = m_currentStep;

    if (step == Step::GetBySpotifyId) {
        if (err == QNetworkReply::NoError && statusCode == 200) {
            handlePayload(reply->readAll(), gen);
            return;
        }
        // If 404 or 400, try fallback to search by title & artist if title is available!
        if (statusCode == 404 || statusCode == 400) {
            if (!m_currentMeta.title.trimmed().isEmpty()) {
                startSearchByTitleArtist();
                return;
            }
            failCurrent(MissingReason::NotFound, QStringLiteral("Track not found in AMLL database (HTTP 404)"), true);
            return;
        }
        if (statusCode == 429) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Rate limit reached (HTTP 429)"));
        } else if (statusCode >= 500) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP server error (%1)").arg(statusCode));
        } else if (err != QNetworkReply::NoError) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Network error (%1)").arg(reply->errorString()));
        } else {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP error (%1)").arg(statusCode));
        }
        return;
    }

    if (step == Step::SearchByTitleArtist) {
        if (err == QNetworkReply::NoError && statusCode == 200) {
            const QByteArray data = reply->readAll();
            QJsonParseError jsonError;
            const QJsonDocument doc = QJsonDocument::fromJson(data, &jsonError);
            if (jsonError.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject root = doc.object();
                const QJsonObject dataObj = root.value(QLatin1String("data")).toObject();
                const QJsonArray items = dataObj.value(QLatin1String("items")).toArray();
                bool hadCandidatesWithMeta = false;
                for (const QJsonValue &val : items) {
                    if (!val.isObject()) {
                        continue;
                    }
                    const QJsonObject item = val.toObject();
                    const qint64 itemId = item.value(QLatin1String("id")).toVariant().toLongLong();
                    const QString filename = item.value(QLatin1String("filename")).toString();
                    if (itemId <= 0 && filename.trimmed().isEmpty()) {
                        continue;
                    }

                    QString candTitle = item.value(QLatin1String("title")).toString().trimmed();
                    if (candTitle.isEmpty()) {
                        candTitle = item.value(QLatin1String("name")).toString().trimmed();
                    }
                    if (candTitle.isEmpty()) {
                        candTitle = item.value(QLatin1String("musicName")).toString().trimmed();
                    }

                    QStringList candArtists;
                    const QJsonValue artistsVal = item.value(QLatin1String("artists"));
                    if (artistsVal.isArray()) {
                        for (const QJsonValue &aVal : artistsVal.toArray()) {
                            if (aVal.isString()) {
                                candArtists.append(aVal.toString());
                            } else if (aVal.isObject()) {
                                candArtists.append(aVal.toObject().value(QLatin1String("name")).toString());
                            }
                        }
                    } else if (artistsVal.isString() && !artistsVal.toString().trimmed().isEmpty()) {
                        candArtists.append(artistsVal.toString());
                    }
                    if (candArtists.isEmpty()) {
                        const QString singleArtist = item.value(QLatin1String("artist")).toString().trimmed();
                        if (!singleArtist.isEmpty()) {
                            candArtists.append(singleArtist);
                        } else {
                            const QString artistName = item.value(QLatin1String("artistName")).toString().trimmed();
                            if (!artistName.isEmpty()) {
                                candArtists.append(artistName);
                            }
                        }
                    }

                    const qint64 candDurationMs = item.value(QLatin1String("duration")).toVariant().toLongLong();

                    if (!candTitle.isEmpty()) {
                        hadCandidatesWithMeta = true;
                        if (!TrackMatch::matchCandidate(m_currentMeta, candTitle, candArtists, candDurationMs)) {
                            continue;
                        }
                    }

                    startGetById(itemId, filename);
                    return;
                }
                if (hadCandidatesWithMeta) {
                    failCurrent(MissingReason::NotFound, QStringLiteral("Candidate metadata mismatch in AMLL search"), true);
                    return;
                }
            }
            failCurrent(MissingReason::NotFound, QStringLiteral("Track not found in AMLL database (0 results)"), true);
            return;
        }
        if (statusCode == 404 || statusCode == 400) {
            failCurrent(MissingReason::NotFound, QStringLiteral("Track not found in AMLL database (HTTP 404)"), true);
            return;
        }
        if (statusCode == 429) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Rate limit reached (HTTP 429)"));
        } else if (statusCode >= 500) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP server error (%1)").arg(statusCode));
        } else if (err != QNetworkReply::NoError) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Network error (%1)").arg(reply->errorString()));
        } else {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP error (%1)").arg(statusCode));
        }
        return;
    }

    if (step == Step::GetById) {
        if (err == QNetworkReply::NoError && statusCode == 200) {
            handlePayload(reply->readAll(), gen);
            return;
        }
        if (statusCode == 404) {
            failCurrent(MissingReason::NotFound, QStringLiteral("Track not found in AMLL database (HTTP 404)"), true);
            return;
        }
        if (statusCode == 429) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Rate limit reached (HTTP 429)"));
        } else if (statusCode >= 500) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP server error (%1)").arg(statusCode));
        } else if (err != QNetworkReply::NoError) {
            failCurrent(MissingReason::Unreachable, QStringLiteral("Network error (%1)").arg(reply->errorString()));
        } else {
            failCurrent(MissingReason::Unreachable, QStringLiteral("HTTP error (%1)").arg(statusCode));
        }
        return;
    }
}

void AmllProvider::onTimeout()
{
    failCurrent(MissingReason::Unreachable, QStringLiteral("Request timed out"));
}

bool AmllProvider::checkDiskCache(const QString &cacheKey, qint64 durationMs, Lyrics *outLyrics)
{
    if (!outLyrics || cacheKey.isEmpty()) {
        return false;
    }

    const QString path = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        return false;
    }

    const qint64 ageMs = fi.lastModified().msecsTo(QDateTime::currentDateTime());
    if (ageMs > kAmllCacheTtlMs) {
        QFile::remove(path);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QString text = QString::fromUtf8(file.readAll());
    if (TtmlParser::parseTtml(text, outLyrics, durationMs) && outLyrics->isUsable()) {
        outLyrics->attributionData.provider = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
        outLyrics->attribution = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
        return true;
    }

    return false;
}

void AmllProvider::writeDiskCache(const QString &cacheKey, const QByteArray &data)
{
    if (cacheKey.isEmpty() || data.isEmpty()) {
        return;
    }

    const QString dirPath = cacheDirectory();
    QDir dir;
    if (!dir.exists(dirPath)) {
        dir.mkpath(dirPath);
    }

    const QString filePath = dirPath + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(data);
    }
}
