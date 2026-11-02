/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "qqmusicprovider.h"
#include "candidatesearcherror.h"
#include "lrcparser.h"
#include "qrcdecoder.h"
#include "trackmatch.h"
#include "trackname.h"
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
constexpr int kMaxDurationDiffSec = 4;
} // namespace

QqMusicProvider::QqMusicProvider(QObject *parent, const QString &apiBase)
    : LyricsProvider(parent)
    , m_apiBase(apiBase)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &QqMusicProvider::onTimeout);
}

QqMusicProvider::~QqMusicProvider()
{
    cancel();
}

bool QqMusicProvider::canHandle(const TrackMetadata &meta) const
{
    return m_enabled && !meta.title.trimmed().isEmpty();
}

void QqMusicProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_meta = meta;
    m_currentGeneration = generation;
    m_step = Step::SearchSong;

    startSearch();
}

void QqMusicProvider::cancel()
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

void QqMusicProvider::onTimeout()
{
    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
}

void QqMusicProvider::onReplyFinished()
{
    m_watchdog.stop();
    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;

    if (!reply) {
        return;
    }
    reply->deleteLater();

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::SizeLimitExceeded, id());
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    const QByteArray data = reply->readAll();

    switch (m_step) {
    case Step::SearchSong:
        handleSearchReply(data);
        break;
    case Step::FetchQrc:
        handleQrcReply(data);
        break;
    case Step::FetchLrcFallback:
        handleLrcReply(data);
        break;
    default:
        break;
    }
}

void QqMusicProvider::startSearch()
{
    const QString kw = (m_meta.artist.trimmed() + QLatin1Char(' ') + m_meta.title.trimmed()).trimmed();

    QJsonObject param;
    param[QStringLiteral("num_per_page")] = 8;
    param[QStringLiteral("page_num")] = 1;
    param[QStringLiteral("query")] = kw;
    param[QStringLiteral("search_type")] = 0;

    QJsonObject req1;
    req1[QStringLiteral("module")] = QStringLiteral("music.search.SearchCgiService");
    req1[QStringLiteral("method")] = QStringLiteral("DoSearchForQQMusicDesktop");
    req1[QStringLiteral("param")] = param;

    QJsonObject root;
    root[QStringLiteral("req_1")] = req1;

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QNetworkRequest req(QUrl(QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic"));
    req.setRawHeader("Referer", "https://y.qq.com/");

    m_currentReply = m_network.post(req, payload);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &QqMusicProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void QqMusicProvider::handleSearchReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonArray songs = doc.object().value(QStringLiteral("req_1")).toObject()
                                 .value(QStringLiteral("data")).toObject()
                                 .value(QStringLiteral("body")).toObject()
                                 .value(QStringLiteral("song")).toObject()
                                 .value(QStringLiteral("list")).toArray();

    if (songs.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    QString matchedMid;

    for (const QJsonValue &v : songs) {
        const QJsonObject obj = v.toObject();
        QString name = obj.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            name = obj.value(QStringLiteral("title")).toString();
        }
        const qint64 interval = obj.value(QStringLiteral("interval")).toInt();
        const QString mid = obj.value(QStringLiteral("mid")).toString();

        if (mid.isEmpty()) {
            continue;
        }

        // Parse singers from QQ Music in-source fields: 'singer' array of objects with 'name' / 'title'
        QStringList singerList;
        const QJsonArray singerArray = obj.value(QStringLiteral("singer")).toArray();
        for (const QJsonValue &s : singerArray) {
            QString sName = s.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (sName.isEmpty()) {
                sName = s.toObject().value(QStringLiteral("title")).toString().trimmed();
            }
            if (!sName.isEmpty()) {
                singerList.append(sName);
            }
        }
        if (singerList.isEmpty() && obj.contains(QStringLiteral("singer_name"))) {
            const QString sName = obj.value(QStringLiteral("singer_name")).toString().trimmed();
            if (!sName.isEmpty()) {
                singerList.append(sName);
            }
        }

        const qint64 durMs = interval > 0 ? interval * 1000 : 0;

        if (TrackMatch::matchCandidate(m_meta, name, singerList, durMs, kMaxDurationDiffSec * 1000)) {
            matchedMid = mid;
            break;
        }
    }

    if (matchedMid.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    m_currentMid = matchedMid;
    m_step = Step::FetchQrc;
    startFetchQrc(m_currentMid);
}

void QqMusicProvider::startFetchQrc(const QString &songMid)
{
    QJsonObject comm;
    comm[QStringLiteral("ct")] = QStringLiteral("19");
    comm[QStringLiteral("cv")] = QStringLiteral("1873");
    comm[QStringLiteral("uin")] = QStringLiteral("0");

    QJsonObject param;
    param[QStringLiteral("songMID")] = songMid;
    param[QStringLiteral("qrc")] = 1;
    param[QStringLiteral("qrc_t")] = 0;
    param[QStringLiteral("trans")] = 1;
    param[QStringLiteral("roma")] = 1;
    param[QStringLiteral("crypt")] = 1;

    QJsonObject reqObj;
    reqObj[QStringLiteral("module")] = QStringLiteral("music.musichallSong.PlayLyricInfo");
    reqObj[QStringLiteral("method")] = QStringLiteral("GetPlayLyricInfo");
    reqObj[QStringLiteral("param")] = param;

    QJsonObject root;
    root[QStringLiteral("comm")] = comm;
    root[QStringLiteral("req")] = reqObj;

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QNetworkRequest req(QUrl(QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic"));
    req.setRawHeader("Referer", "https://y.qq.com/");

    m_currentReply = m_network.post(req, payload);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &QqMusicProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void QqMusicProvider::handleQrcReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QString lyric = doc.object().value(QStringLiteral("req")).toObject()
                              .value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("lyric")).toString();

    Lyrics lyrics;
    if (!lyric.isEmpty() && QrcDecoder::parse(lyric, &lyrics, m_meta.durationMs) && lyrics.isUsable()) {
        lyrics.attribution = QStringLiteral("QQ Music (QQ音乐)");
        lyrics.attributionData.provider = QStringLiteral("QQ Music");
        lyrics.attributionData.providerUrl = QStringLiteral("https://y.qq.com");

        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsLoaded(gen, lyrics, id());
        return;
    }

    // Fallback to LRC
    m_step = Step::FetchLrcFallback;
    startFetchLrcFallback(m_currentMid);
}

void QqMusicProvider::startFetchLrcFallback(const QString &songMid)
{
    QUrl url(QStringLiteral("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("songmid"), songMid);
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("nobase64"), QStringLiteral("1"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic"));
    req.setRawHeader("Referer", "https://y.qq.com/");

    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &QqMusicProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void QqMusicProvider::handleLrcReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    QString lyric = doc.object().value(QStringLiteral("lyric")).toString();

    if (lyric.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    if (!lyric.trimmed().startsWith(QLatin1Char('['))) {
        const QByteArray decoded = QByteArray::fromBase64(lyric.toUtf8());
        if (!decoded.isEmpty()) {
            lyric = QString::fromUtf8(decoded);
        }
    }

    Lyrics lyrics;
    bool limitExceeded = false;
    if (!LrcParser::parseLrc(lyric, &lyrics, m_meta.durationMs, &limitExceeded) || !lyrics.isUsable()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        const MissingReason reason = limitExceeded ? MissingReason::SizeLimitExceeded : MissingReason::Unsynced;
        Q_EMIT lyricsFailed(gen, reason, id());
        return;
    }

    lyrics.attribution = QStringLiteral("QQ Music (QQ音乐)");
    lyrics.attributionData.provider = QStringLiteral("QQ Music");
    lyrics.attributionData.providerUrl = QStringLiteral("https://y.qq.com");

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void QqMusicProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    cancelCandidateSearch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(base.isEmpty()
        ? QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg")
        : (base + QStringLiteral("/cgi-bin/musicu.fcg")));

    QJsonObject comm;
    comm[QStringLiteral("ct")] = 19;
    comm[QStringLiteral("cv")] = 1873;

    QJsonObject param;
    param[QStringLiteral("search_type")] = 0;
    param[QStringLiteral("query")] = (meta.artist.trimmed() + QLatin1Char(' ') + meta.title.trimmed()).trimmed();
    param[QStringLiteral("page_num")] = 1;
    param[QStringLiteral("num_per_page")] = 10;

    QJsonObject req1;
    req1[QStringLiteral("method")] = QStringLiteral("DoSearchForQQMusicDesktop");
    req1[QStringLiteral("module")] = QStringLiteral("music.search.SearchCgiService");
    req1[QStringLiteral("param")] = param;

    QJsonObject root;
    root[QStringLiteral("comm")] = comm;
    root[QStringLiteral("req_1")] = req1;

    const QByteArray body = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json;charset=utf-8"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic/1873"));
    req.setRawHeader("Referer", "https://y.qq.com");

    m_candidateSearchId = searchId;
    m_candidateSearchReply = m_network.post(req, body);
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

        if (doc.isObject()) {
            const QJsonArray songs = doc.object().value(QStringLiteral("req_1")).toObject()
                                         .value(QStringLiteral("data")).toObject()
                                         .value(QStringLiteral("body")).toObject()
                                         .value(QStringLiteral("song")).toObject()
                                         .value(QStringLiteral("list")).toArray();

            for (const QJsonValue &v : songs) {
                if (result.size() >= 10) break;
                const QJsonObject obj = v.toObject();
                QString name = obj.value(QStringLiteral("name")).toString();
                if (name.isEmpty()) name = obj.value(QStringLiteral("title")).toString();
                const qint64 interval = obj.value(QStringLiteral("interval")).toInt();
                const QString mid = obj.value(QStringLiteral("mid")).toString();
                if (mid.isEmpty()) continue;

                QStringList singerList;
                const QJsonArray singerArray = obj.value(QStringLiteral("singer")).toArray();
                for (const QJsonValue &s : singerArray) {
                    QString sName = s.toObject().value(QStringLiteral("name")).toString().trimmed();
                    if (sName.isEmpty()) sName = s.toObject().value(QStringLiteral("title")).toString().trimmed();
                    if (!sName.isEmpty()) singerList.append(sName);
                }

                const qint64 durMs = interval > 0 ? interval * 1000 : 0;
                if (!TrackMatch::matchCandidate(meta, name, singerList, durMs, 4000)) {
                    continue;
                }

                LyricCandidate cand;
                cand.id = mid;
                cand.providerId = id();
                cand.sourceName = displayName();
                cand.title = name;
                cand.artist = singerList.join(QStringLiteral(", "));
                cand.durationMs = durMs;
                cand.syncType = CandidateSyncType::Unknown;
                result.append(cand);
            }
        }

        Q_EMIT candidatesFound(searchId, result, id());
    });
}

void QqMusicProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    cancelCandidateFetch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(base.isEmpty()
        ? QStringLiteral("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg")
        : (base + QStringLiteral("/lyric/fcgi-bin/fcg_query_lyric_new.fcg")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("songmid"), candidateId);
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("nobase64"), QStringLiteral("0"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) QQMusic/1873"));
    req.setRawHeader("Referer", "https://y.qq.com");

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
        if (!doc.isObject()) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            return;
        }

        const QString lyric = doc.object().value(QStringLiteral("lyric")).toString();
        const QString qrc = doc.object().value(QStringLiteral("qrc")).toString();

        Lyrics lyrics;
        bool limitExceeded = false;
        bool ok = false;
        if (!qrc.isEmpty()) {
            const QByteArray decodedQrc = QByteArray::fromBase64(qrc.toUtf8());
            ok = QrcDecoder::parse(decodedQrc, &lyrics, 0, &limitExceeded);
        }
        if (!ok && !limitExceeded) {
            if (!lyric.isEmpty()) {
                const QByteArray decodedLrc = QByteArray::fromBase64(lyric.toUtf8());
                ok = LrcParser::parseLrc(QString::fromUtf8(decodedLrc), &lyrics, 0, &limitExceeded);
            }
        }

        if (limitExceeded) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
            return;
        }

        if (ok && lyrics.isUsable()) {
            lyrics.attribution = QStringLiteral("QQ Music (QQ音乐)");
            lyrics.attributionData.provider = QStringLiteral("QQ Music");
            lyrics.attributionData.providerUrl = QStringLiteral("https://y.qq.com");
            Q_EMIT candidateLyricsLoaded(fetchId, lyrics, id());
        } else {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
        }
    });
}

void QqMusicProvider::cancelCandidateSearch(quint64 searchId)
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

void QqMusicProvider::cancelCandidateFetch(quint64 fetchId)
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
