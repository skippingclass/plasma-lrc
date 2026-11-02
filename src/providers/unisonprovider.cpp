/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "unisonprovider.h"
#include "candidatesearcherror.h"
#include "lrcparser.h"
#include "trackmatch.h"
#include "trackname.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace
{
constexpr int kTimeoutMs = 7000;
} // namespace

UnisonProvider::UnisonProvider(QObject *parent, const QString &apiBase)
    : LyricsProvider(parent)
    , m_apiBase(apiBase)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &UnisonProvider::onTimeout);
}

UnisonProvider::~UnisonProvider()
{
    cancel();
}

bool UnisonProvider::canHandle(const TrackMetadata &meta) const
{
    return m_enabled && !meta.title.trimmed().isEmpty();
}

void UnisonProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_meta = meta;
    m_currentGeneration = generation;
    m_step = Step::DirectLookup;

    startDirectLookup();
}

void UnisonProvider::cancel()
{
    m_watchdog.stop();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    cancelCandidateSearch(0);
    cancelCandidateFetch(0);
    m_step = Step::Idle;
}

void UnisonProvider::onTimeout()
{
    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
}

void UnisonProvider::onReplyFinished()
{
    m_watchdog.stop();
    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;

    if (!reply) {
        return;
    }
    const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError err = reply->error();
    reply->deleteLater();

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::SizeLimitExceeded, id());
        return;
    }

    if (m_step == Step::DirectLookup) {
        if (err == QNetworkReply::NoError && httpCode == 200) {
            handleDirectReply(data, httpCode);
            return;
        }
        // Direct lookup failed/404 -> fallback to Search
        m_step = Step::Search;
        startSearch();
        return;
    }

    if (err != QNetworkReply::NoError) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    if (m_step == Step::Search) {
        handleSearchReply(data);
    } else if (m_step == Step::FetchDetails) {
        handleDetailsReply(data);
    }
}

void UnisonProvider::startDirectLookup()
{
    QUrl url(QStringLiteral("https://unison.boidu.dev/lyrics"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("song"), m_meta.title.trimmed());
    query.addQueryItem(QStringLiteral("artist"), m_meta.artist.trimmed());
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) Plasma-LRC/1.7"));

    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &UnisonProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void UnisonProvider::handleDirectReply(const QByteArray &data, int httpCode)
{
    Q_UNUSED(httpCode);
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonObject dataObj = doc.object().value(QStringLiteral("data")).toObject();

    // Validate direct exact-ID lookup metadata only if fields are present in response
    const QString respSong = dataObj.value(QStringLiteral("song")).toString();
    const QString respArtist = dataObj.value(QStringLiteral("artist")).toString();
    qint64 respDurMs = 0;
    if (dataObj.contains(QStringLiteral("duration"))) {
        const double d = dataObj.value(QStringLiteral("duration")).toDouble();
        if (d > 0) {
            respDurMs = d > 10000 ? static_cast<qint64>(d) : static_cast<qint64>(d * 1000);
        }
    } else if (dataObj.contains(QStringLiteral("durationMs"))) {
        respDurMs = dataObj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
    }

    if (!TrackMatch::validateExactLookupMetadata(m_meta, respSong, respArtist, respDurMs)) {
        m_step = Step::Search;
        startSearch();
        return;
    }

    Lyrics lyrics;
    if (parsePayload(dataObj, &lyrics)) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsLoaded(gen, lyrics, id());
        return;
    }

    m_step = Step::Search;
    startSearch();
}

void UnisonProvider::startSearch()
{
    QUrl url(QStringLiteral("https://unison.boidu.dev/lyrics/search"));
    QUrlQuery query;
    const QString kw = (m_meta.artist.trimmed() + QLatin1Char(' ') + m_meta.title.trimmed()).trimmed();
    query.addQueryItem(QStringLiteral("q"), kw);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) Plasma-LRC/1.7"));

    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &UnisonProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void UnisonProvider::handleSearchReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonArray results = doc.object().value(QStringLiteral("data")).toArray();
    if (results.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    QString matchedId;
    for (const QJsonValue &v : results) {
        const QJsonObject obj = v.toObject();
        const QString song = obj.value(QStringLiteral("song")).toString();
        const QString artist = obj.value(QStringLiteral("artist")).toString();
        const QString idStr = obj.value(QStringLiteral("id")).toString();

        if (idStr.isEmpty()) {
            continue;
        }

        qint64 durMs = 0;
        if (obj.contains(QStringLiteral("duration"))) {
            const double d = obj.value(QStringLiteral("duration")).toDouble();
            if (d > 0) {
                durMs = d > 10000 ? static_cast<qint64>(d) : static_cast<qint64>(d * 1000);
            }
        } else if (obj.contains(QStringLiteral("durationMs"))) {
            durMs = obj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
        }

        if (TrackMatch::matchCandidate(m_meta, song, artist, durMs)) {
            matchedId = idStr;
            break;
        }
    }

    // No blind fallback to results.first(): fail if no matching track
    if (matchedId.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    m_step = Step::FetchDetails;
    startFetchDetails(matchedId);
}

void UnisonProvider::startFetchDetails(const QString &lyricsId)
{
    QUrl url(QStringLiteral("https://unison.boidu.dev/lyrics/%1").arg(lyricsId));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) Plasma-LRC/1.7"));

    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &UnisonProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void UnisonProvider::handleDetailsReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonObject dataObj = doc.object().value(QStringLiteral("data")).toObject();

    const QString respSong = dataObj.value(QStringLiteral("song")).toString();
    const QString respArtist = dataObj.value(QStringLiteral("artist")).toString();
    qint64 respDurMs = 0;
    if (dataObj.contains(QStringLiteral("duration"))) {
        const double d = dataObj.value(QStringLiteral("duration")).toDouble();
        if (d > 0) {
            respDurMs = d > 10000 ? static_cast<qint64>(d) : static_cast<qint64>(d * 1000);
        }
    } else if (dataObj.contains(QStringLiteral("durationMs"))) {
        respDurMs = dataObj.value(QStringLiteral("durationMs")).toVariant().toLongLong();
    }

    if (!TrackMatch::validateExactLookupMetadata(m_meta, respSong, respArtist, respDurMs)) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    Lyrics lyrics;
    bool limitExceeded = false;
    if (!parsePayload(dataObj, &lyrics, &limitExceeded)) {
        const quint64 gen = m_currentGeneration;
        cancel();
        const MissingReason reason = limitExceeded ? MissingReason::SizeLimitExceeded : MissingReason::NotFound;
        Q_EMIT lyricsFailed(gen, reason, id());
        return;
    }

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

bool UnisonProvider::parsePayload(const QJsonObject &dataObj, Lyrics *outLyrics, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    if (!outLyrics) {
        return false;
    }

    const QString text = dataObj.value(QStringLiteral("lyrics")).toString();
    const QString format = dataObj.value(QStringLiteral("format")).toString().toLower();

    if (text.trimmed().isEmpty()) {
        return false;
    }

    bool ok = false;
    if (format == QLatin1String("ttml") || text.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
        ok = TtmlParser::parseTtml(text, outLyrics, m_meta.durationMs, outLimitExceeded);
    } else {
        ok = LrcParser::parseLrc(text, outLyrics, m_meta.durationMs, outLimitExceeded);
    }

    if (!ok || !outLyrics->isUsable()) {
        return false;
    }

    const QJsonObject submitter = dataObj.value(QStringLiteral("submitter")).toObject();
    const QString submitterName = submitter.value(QStringLiteral("displayName")).toString();
    const QString keyId = submitter.value(QStringLiteral("keyId")).toString();

    outLyrics->attributionData.provider = QStringLiteral("Unison");
    outLyrics->attributionData.providerUrl = QStringLiteral("https://unison.boidu.dev");

    if (!submitterName.isEmpty()) {
        LyricContributor c;
        c.role = QStringLiteral("submitter");
        c.name = submitterName;
        if (!keyId.isEmpty()) {
            c.url = QStringLiteral("https://unison.boidu.dev/curator/%1").arg(keyId);
        }
        outLyrics->attributionData.contributors.append(c);
        outLyrics->attribution = QStringLiteral("Unison (boidu.dev) via %1").arg(submitterName);
    } else {
        outLyrics->attribution = QStringLiteral("Unison (boidu.dev)");
    }

    return true;
}

void UnisonProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    cancelCandidateSearch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(base + QStringLiteral("/lyrics/search"));
    QUrlQuery query;
    const QString q = (meta.artist.trimmed() + QLatin1Char(' ') + meta.title.trimmed()).trimmed();
    query.addQueryItem(QStringLiteral("query"), q);
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

        if (doc.isArray()) {
            for (const QJsonValue &v : doc.array()) {
                if (result.size() >= 10) break;
                const QJsonObject obj = v.toObject();
                const QString song = obj.value(QStringLiteral("song")).toString();
                const QString artist = obj.value(QStringLiteral("artist")).toString();
                const QString idStr = obj.value(QStringLiteral("id")).toString();
                const qint64 duration = obj.value(QStringLiteral("duration")).toVariant().toLongLong();
                if (idStr.isEmpty()) continue;

                if (!TrackMatch::matchCandidate(meta, song, artist, duration, 4000)) {
                    continue;
                }

                LyricCandidate cand;
                cand.id = idStr;
                cand.providerId = id();
                cand.sourceName = displayName();
                cand.title = song;
                cand.artist = artist;
                cand.durationMs = duration;
                cand.syncType = CandidateSyncType::Unknown;
                result.append(cand);
            }
        }

        Q_EMIT candidatesFound(searchId, result, id());
    });
}

void UnisonProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    cancelCandidateFetch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(QStringLiteral("%1/lyrics/%2").arg(base, candidateId));
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

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject dataObj = doc.object();
        if (dataObj.contains(QStringLiteral("data"))) {
            dataObj = dataObj.value(QStringLiteral("data")).toObject();
        }
        Lyrics lyrics;
        bool limitExceeded = false;
        if (parsePayload(dataObj, &lyrics, &limitExceeded) && lyrics.isUsable()) {
            Q_EMIT candidateLyricsLoaded(fetchId, lyrics, id());
        } else {
            const MissingReason reason = limitExceeded ? MissingReason::SizeLimitExceeded : MissingReason::NotFound;
            Q_EMIT candidateLyricsFailed(fetchId, reason, id());
        }
    });
}

void UnisonProvider::cancelCandidateSearch(quint64 searchId)
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

void UnisonProvider::cancelCandidateFetch(quint64 fetchId)
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
