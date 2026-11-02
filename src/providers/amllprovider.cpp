/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "amllprovider.h"
#include "candidatesearcherror.h"
#include "trackmatch.h"
#include "ttmlparser.h"
#include "lrcparser.h"
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
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace
{
constexpr qint64 kAmllCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.9.2 (https://github.com/skippingclass/plasma-lrc)");
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
    const QString expTitle = meta.effectiveExpectedTitle().trimmed().toLower();
    const QString expArtist = meta.effectiveExpectedArtist().trimmed().toLower();
    const QString combined = QStringLiteral("%1\t%2\t%3\t%4\t%5")
        .arg(meta.title.trimmed().toLower(),
             meta.artist.trimmed().toLower(),
             QString::number(meta.durationMs > 0 ? meta.durationMs / 1000 : 0),
             expTitle,
             expArtist);
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
    m_selectedCandTitle.clear();
    m_selectedCandArtists.clear();
    m_selectedCandDurationMs = 0;

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
    if (checkDiskCache(m_currentCacheKey, meta, &cachedLyrics)) {
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
    cancelCandidateSearch(0);
    cancelCandidateFetch(0);
    m_currentCacheKey.clear();
    m_currentGeneration = 0;
    m_currentStep = Step::Idle;
}

void AmllProvider::invalidateCache(const TrackMetadata &meta)
{
    const QVector<TrackMetadata> variants = generateSearchVariants(meta);
    QVector<TrackMetadata> allMetas = variants;
    allMetas.append(meta);

    for (const auto &v : allMetas) {
        QStringList keys;
        keys.append(makeCacheKey(v));
        if (!v.spotifyTrackId.trimmed().isEmpty()) {
            keys.append(v.spotifyTrackId.trimmed());
        }
        const QString oldCombined = QStringLiteral("%1\t%2\t%3")
            .arg(v.title.trimmed().toLower(),
                 v.artist.trimmed().toLower(),
                 QString::number(v.durationMs > 0 ? v.durationMs / 1000 : 0));
        keys.append(QString::fromLatin1(QCryptographicHash::hash(oldCombined.toUtf8(), QCryptographicHash::Sha1).toHex()));

        for (const QString &k : keys) {
            if (k.isEmpty()) {
                continue;
            }
            if (m_currentCacheKey == k) {
                cancel();
            }
            m_negativeCache.remove(k);
            QFile::remove(cacheDirectory() + QStringLiteral("/") + k + QStringLiteral(".ttml"));
            QFile::remove(cacheDirectory() + QStringLiteral("/") + k + QStringLiteral(".meta"));
        }
    }
}

void AmllProvider::clearAllCaches()
{
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
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
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
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
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
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
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
    if (!LyricsLimits::checkInputSize(data.size())) {
        failCurrent(MissingReason::SizeLimitExceeded, QStringLiteral("Response size limit exceeded (max 8 MiB)"));
        return;
    }

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
    bool limitExceeded = false;
    if (!TtmlParser::parseTtml(trimmed, &lyrics, m_currentMeta.durationMs, &limitExceeded) || !lyrics.isUsable()) {
        if (limitExceeded) {
            failCurrent(MissingReason::SizeLimitExceeded, QStringLiteral("Lyrics exceed line or word limit"), /*is404=*/false);
            return;
        }
        failCurrent(MissingReason::Unsynced, QStringLiteral("No synchronized lyrics in TTML"), true);
        return;
    }

    lyrics.attributionData.provider = QStringLiteral("AMLL TTML DB");
    lyrics.attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
    lyrics.attribution = QStringLiteral("AMLL TTML DB");
    lyrics.attributionUrl = lyrics.attributionData.providerUrl;

    writeDiskCache(m_currentCacheKey, trimmed.toUtf8(), m_selectedCandTitle, m_selectedCandArtists, m_selectedCandDurationMs);

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

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        failCurrent(MissingReason::SizeLimitExceeded, QStringLiteral("Response size limit exceeded (max 8 MiB)"));
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
            if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
                failCurrent(MissingReason::Unreachable,
                            QStringLiteral("Malformed JSON in AMLL search response: %1").arg(jsonError.errorString()),
                            false);
                return;
            }

            const QJsonObject root = doc.object();
            if (!root.contains(QLatin1String("data")) || !root.value(QLatin1String("data")).isObject()) {
                failCurrent(MissingReason::Unreachable,
                            QStringLiteral("Invalid response structure in AMLL search (missing data object)"),
                            false);
                return;
            }

            const QJsonObject dataObj = root.value(QLatin1String("data")).toObject();
            if (!dataObj.contains(QLatin1String("items")) || !dataObj.value(QLatin1String("items")).isArray()) {
                failCurrent(MissingReason::Unreachable,
                            QStringLiteral("Invalid response structure in AMLL search (missing items array)"),
                            false);
                return;
            }

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

                m_selectedCandTitle = candTitle;
                m_selectedCandArtists = candArtists;
                m_selectedCandDurationMs = candDurationMs;
                startGetById(itemId, filename);
                return;
            }

            if (hadCandidatesWithMeta) {
                failCurrent(MissingReason::NotFound, QStringLiteral("Candidate metadata mismatch in AMLL search"), true);
                return;
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

bool AmllProvider::checkDiskCache(const QString &cacheKey, const TrackMetadata &meta, Lyrics *outLyrics)
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
            QStringList candArtists;
            const QJsonArray artsArr = obj.value(QStringLiteral("artists")).toArray();
            for (const auto &a : artsArr) {
                candArtists.append(a.toString());
            }
            const qint64 candDurMs = obj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
            if (!TrackMatch::matchCandidate(meta, candTitle, candArtists, candDurMs, 4000)) {
                QFile::remove(metaPath);
                QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml"));
                return false;
            }
        }
    } else {
        if (!meta.effectiveExpectedArtist().trimmed().isEmpty() && meta.spotifyTrackId.trimmed().isEmpty()) {
            QFile::remove(cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml"));
            return false;
        }
    }

    const QString path = cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        return false;
    }

    const qint64 ageMs = fi.lastModified().msecsTo(QDateTime::currentDateTime());
    if (ageMs > kAmllCacheTtlMs || fi.size() > LyricsLimits::kMaxInputSizeBytes) {
        QFile::remove(path);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QString text = QString::fromUtf8(file.readAll());
    if (TtmlParser::parseTtml(text, outLyrics, meta.durationMs) && outLyrics->isUsable()) {
        outLyrics->attributionData.provider = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
        outLyrics->attribution = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
        return true;
    }

    return false;
}

void AmllProvider::writeDiskCache(const QString &cacheKey, const QByteArray &data,
                                  const QString &candTitle, const QStringList &candArtists, qint64 candDurationMs)
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

    if (!candTitle.isEmpty() || !candArtists.isEmpty()) {
        QFile metaFile(dirPath + QStringLiteral("/") + cacheKey + QStringLiteral(".meta"));
        if (metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QJsonObject obj;
            obj[QStringLiteral("title")] = candTitle;
            QJsonArray arts;
            for (const QString &a : candArtists) {
                arts.append(a);
            }
            obj[QStringLiteral("artists")] = arts;
            obj[QStringLiteral("durationMs")] = candDurationMs;
            metaFile.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }
    }
}

void AmllProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    cancelCandidateSearch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    QUrl url(base + QStringLiteral("/v1/lyrics/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("musicName"), meta.title);
    query.addQueryItem(QStringLiteral("title"), meta.title);
    if (!meta.artist.isEmpty()) {
        query.addQueryItem(QStringLiteral("artistName"), meta.artist);
        query.addQueryItem(QStringLiteral("artist"), meta.artist);
    }
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("plasma-lrc/1.9.2 (candidate-search)"));

    m_candidateSearchId = searchId;
    m_candidateSearchReply = m_network.get(req);
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
            Q_EMIT candidateSearchFailed(searchId, CandidateSearchError::reason(reply), id(), CandidateSearchError::retryAfterSeconds(reply));
            return;
        }

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull()) {
            Q_EMIT candidateSearchFailed(searchId, MissingReason::Unreachable, id(), 0);
            return;
        }
        QList<LyricCandidate> result;

        QJsonArray items;
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root.contains(QLatin1String("data"))) {
                const QJsonObject dataObj = root.value(QLatin1String("data")).toObject();
                items = dataObj.value(QLatin1String("items")).toArray();
            } else if (root.contains(QLatin1String("items"))) {
                items = root.value(QLatin1String("items")).toArray();
            }
        } else if (doc.isArray()) {
            items = doc.array();
        }

        for (const QJsonValue &val : items) {
            if (result.size() >= 10) {
                break;
            }
            if (!val.isObject()) continue;
            const QJsonObject item = val.toObject();

            const qint64 itemId = item.value(QLatin1String("id")).toVariant().toLongLong();
            const QString filename = item.value(QLatin1String("filename")).toString();
            if (itemId <= 0 && filename.trimmed().isEmpty()) continue;

            QString candTitle = item.value(QLatin1String("title")).toString().trimmed();
            if (candTitle.isEmpty()) candTitle = item.value(QLatin1String("name")).toString().trimmed();
            if (candTitle.isEmpty()) candTitle = item.value(QLatin1String("musicName")).toString().trimmed();

            QStringList candArtists;
            const QJsonValue artistsVal = item.value(QLatin1String("artists"));
            if (artistsVal.isArray()) {
                for (const QJsonValue &aVal : artistsVal.toArray()) {
                    if (aVal.isString()) candArtists.append(aVal.toString());
                    else if (aVal.isObject()) candArtists.append(aVal.toObject().value(QLatin1String("name")).toString());
                }
            } else if (artistsVal.isString()) {
                candArtists.append(artistsVal.toString());
            }
            if (candArtists.isEmpty()) {
                const QString singleArtist = item.value(QLatin1String("artist")).toString().trimmed();
                if (!singleArtist.isEmpty()) candArtists.append(singleArtist);
            }

            const qint64 candDurationMs = item.value(QLatin1String("duration")).toVariant().toLongLong();
            if (!TrackMatch::matchCandidate(meta, candTitle, candArtists, candDurationMs)) {
                continue;
            }

            LyricCandidate cand;
            cand.id = itemId > 0 ? QString::number(itemId) : filename;
            cand.providerId = id();
            cand.sourceName = displayName();
            cand.title = candTitle;
            cand.artist = candArtists.join(QStringLiteral(", "));
            cand.durationMs = candDurationMs;
            cand.syncType = CandidateSyncType::Unknown;
            result.append(cand);
        }

        Q_EMIT candidatesFound(searchId, result, id());
    });
}

void AmllProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    cancelCandidateFetch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    QUrl url(base + QStringLiteral("/v1/lyrics/get"));
    QUrlQuery query;
    bool isNumeric = false;
    const qint64 numericId = candidateId.toLongLong(&isNumeric);
    if (isNumeric && numericId > 0) {
        query.addQueryItem(QStringLiteral("id"), QString::number(numericId));
    } else {
        query.addQueryItem(QStringLiteral("filename"), candidateId);
    }
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("plasma-lrc/1.9.2 (candidate-fetch)"));

    m_candidateFetchId = fetchId;
    m_candidateFetchReply = m_network.get(req);
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
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            return;
        }

        const QByteArray payload = reply->readAll();
        QString ttmlContent;

        QJsonParseError jsonErr;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &jsonErr);
        if (jsonErr.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root.contains(QLatin1String("data"))) {
                const QJsonValue dataVal = root.value(QLatin1String("data"));
                if (dataVal.isObject()) {
                    ttmlContent = dataVal.toObject().value(QLatin1String("lyrics")).toString();
                } else if (dataVal.isString()) {
                    ttmlContent = dataVal.toString();
                }
            } else if (root.contains(QLatin1String("lyrics"))) {
                ttmlContent = root.value(QLatin1String("lyrics")).toString();
            }
        }
        if (ttmlContent.isEmpty()) {
            ttmlContent = QString::fromUtf8(payload);
        }

        Lyrics lyrics;
        bool limitExceeded = false;
        bool ok = TtmlParser::parseTtml(ttmlContent, &lyrics, 0, &limitExceeded);
        if (!ok && !limitExceeded) {
            ok = LrcParser::parseLrc(ttmlContent, &lyrics, 0, &limitExceeded);
        }

        if (limitExceeded) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
            return;
        }

        if (ok && lyrics.isUsable()) {
            lyrics.attributionData.provider = QStringLiteral("AMLL");
            lyrics.attributionData.providerUrl = QStringLiteral("https://amll.top");
            Q_EMIT candidateLyricsLoaded(fetchId, lyrics, id());
        } else {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
        }
    });
}

void AmllProvider::cancelCandidateSearch(quint64 searchId)
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

void AmllProvider::cancelCandidateFetch(quint64 fetchId)
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
