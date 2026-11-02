/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "kugouprovider.h"
#include "candidatesearcherror.h"
#include "krcparser.h"
#include "lrcparser.h"
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

KugouProvider::KugouProvider(QObject *parent, const QString &apiBase)
    : LyricsProvider(parent)
    , m_apiBase(apiBase)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &KugouProvider::onTimeout);
}

KugouProvider::~KugouProvider()
{
    cancel();
}

bool KugouProvider::canHandle(const TrackMetadata &meta) const
{
    return m_enabled && !meta.title.trimmed().isEmpty();
}

void KugouProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_meta = meta;
    m_currentGeneration = generation;
    m_step = Step::SearchSong;

    startSongSearch();
}

void KugouProvider::cancel()
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

void KugouProvider::onTimeout()
{
    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
}

void KugouProvider::onReplyFinished()
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
        handleSearchSongReply(data);
        break;
    case Step::SearchCandidates:
        handleCandidatesReply(data);
        break;
    case Step::DownloadKrc:
        handleDownloadReply(data, true);
        break;
    case Step::DownloadLrcFallback:
        handleDownloadReply(data, false);
        break;
    default:
        break;
    }
}

void KugouProvider::startSongSearch()
{
    QUrl url(QStringLiteral("https://mobileservice.kugou.com/api/v3/search/song"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    const QString kw = (m_meta.artist.trimmed() + QLatin1Char(' ') + m_meta.title.trimmed()).trimmed();
    query.addQueryItem(QStringLiteral("keyword"), kw);
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pagesize"), QStringLiteral("8"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Linux; Android 10) Kugou"));

    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &KugouProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void KugouProvider::handleSearchSongReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonArray songs = doc.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("info")).toArray();
    if (songs.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const qint64 targetDurSec = m_meta.durationMs > 0 ? (m_meta.durationMs + 500) / 1000 : 0;
    QString matchedHash;
    qint64 songDurationSec = 0;

    for (const QJsonValue &v : songs) {
        const QJsonObject obj = v.toObject();
        const QString songname = obj.value(QStringLiteral("songname")).toString();
        const QString singername = obj.value(QStringLiteral("singername")).toString();
        const qint64 dur = obj.value(QStringLiteral("duration")).toInt();
        const QString hash = obj.value(QStringLiteral("hash")).toString();

        if (hash.isEmpty()) {
            continue;
        }

        const qint64 durMs = dur > 0 ? dur * 1000 : 0;

        if (TrackMatch::matchCandidate(m_meta, songname, singername, durMs, kMaxDurationDiffSec * 1000)) {
            matchedHash = hash;
            songDurationSec = dur > 0 ? dur : targetDurSec;
            break;
        }
    }

    if (matchedHash.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    m_step = Step::SearchCandidates;
    startCandidateSearch(matchedHash, songDurationSec);
}

void KugouProvider::startCandidateSearch(const QString &hash, qint64 songDurationSec)
{
    QUrl url(QStringLiteral("https://krcs.kugou.com/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("man"), QStringLiteral("yes"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("mobi"));
    const QString kw = (m_meta.artist.trimmed() + QLatin1Char(' ') + m_meta.title.trimmed()).trimmed();
    query.addQueryItem(QStringLiteral("keyword"), kw);
    query.addQueryItem(QStringLiteral("duration"), QString::number(songDurationSec * 1000));
    query.addQueryItem(QStringLiteral("hash"), hash);
    url.setQuery(query);

    QNetworkRequest req(url);
    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &KugouProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void KugouProvider::handleCandidatesReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonArray candidates = doc.object().value(QStringLiteral("candidates")).toArray();
    if (candidates.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonObject first = candidates.first().toObject();
    m_currentId = first.value(QStringLiteral("id")).toString();
    m_currentAccessKey = first.value(QStringLiteral("accesskey")).toString();

    if (m_currentId.isEmpty() || m_currentAccessKey.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    m_step = Step::DownloadKrc;
    startDownload(m_currentId, m_currentAccessKey, true);
}

void KugouProvider::startDownload(const QString &idStr, const QString &accesskey, bool isKrc)
{
    QUrl url(QStringLiteral("https://lyrics.kugou.com/download"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("id"), idStr);
    query.addQueryItem(QStringLiteral("accesskey"), accesskey);
    query.addQueryItem(QStringLiteral("fmt"), isKrc ? QStringLiteral("krc") : QStringLiteral("lrc"));
    query.addQueryItem(QStringLiteral("charset"), QStringLiteral("utf8"));
    url.setQuery(query);

    QNetworkRequest req(url);
    m_currentReply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
    connect(m_currentReply, &QNetworkReply::finished, this, &KugouProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void KugouProvider::handleDownloadReply(const QByteArray &data, bool isKrc)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QString content = doc.object().value(QStringLiteral("content")).toString();

    if (content.isEmpty()) {
        if (isKrc) {
            // Try fallback to fmt=lrc
            m_step = Step::DownloadLrcFallback;
            startDownload(m_currentId, m_currentAccessKey, false);
            return;
        }
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    Lyrics lyrics;
    bool limitExceeded = false;
    bool parsed = false;

    if (isKrc) {
        parsed = KrcParser::parse(content.toUtf8(), &lyrics, m_meta.durationMs, &limitExceeded);
    } else {
        const QByteArray lrcBytes = QByteArray::fromBase64(content.toUtf8());
        parsed = LrcParser::parseLrc(QString::fromUtf8(lrcBytes), &lyrics, m_meta.durationMs, &limitExceeded);
    }

    if (limitExceeded) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::SizeLimitExceeded, id());
        return;
    }

    if (!parsed || !lyrics.isUsable()) {
        if (isKrc) {
            m_step = Step::DownloadLrcFallback;
            startDownload(m_currentId, m_currentAccessKey, false);
            return;
        }
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attribution = QStringLiteral("KuGou (酷狗音乐)");
    lyrics.attributionData.provider = QStringLiteral("KuGou");
    lyrics.attributionData.providerUrl = QStringLiteral("https://www.kugou.com");

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void KugouProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    cancelCandidateSearch(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(base.isEmpty()
        ? QStringLiteral("https://mobileservice.kugou.com/api/v3/search/song")
        : (base + QStringLiteral("/api/v3/search/song")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("version"), QStringLiteral("9108"));
    query.addQueryItem(QStringLiteral("plat"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("pagesize"), QStringLiteral("10"));
    query.addQueryItem(QStringLiteral("area_code"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("keyword"), (meta.artist.trimmed() + QLatin1Char(' ') + meta.title.trimmed()).trimmed());
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KuGouMusic/9108 (Android)"));

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

        if (doc.isObject()) {
            const QJsonArray songs = doc.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("info")).toArray();
            for (const QJsonValue &v : songs) {
                if (result.size() >= 10) break;
                const QJsonObject obj = v.toObject();
                const QString songname = obj.value(QStringLiteral("songname")).toString();
                const QString singername = obj.value(QStringLiteral("singername")).toString();
                const qint64 dur = obj.value(QStringLiteral("duration")).toInt();
                const QString hash = obj.value(QStringLiteral("hash")).toString();
                if (hash.isEmpty()) continue;

                const qint64 durMs = dur > 0 ? dur * 1000 : 0;
                if (!TrackMatch::matchCandidate(meta, songname, singername, durMs, 4000)) {
                    continue;
                }

                LyricCandidate cand;
                cand.id = hash + QStringLiteral(":") + QString::number(dur);
                cand.providerId = id();
                cand.sourceName = displayName();
                cand.title = songname;
                cand.artist = singername;
                cand.durationMs = durMs;
                cand.syncType = CandidateSyncType::Unknown;
                result.append(cand);
            }
        }

        Q_EMIT candidatesFound(searchId, result, id());
    });
}

void KugouProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    cancelCandidateFetch(0);

    const QStringList parts = candidateId.split(QLatin1Char(':'));
    const QString hash = parts.value(0);

    QString base = m_apiBase;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QUrl url(base.isEmpty()
        ? QStringLiteral("https://krcs.kugou.com/search")
        : (base + QStringLiteral("/search")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("man"), QStringLiteral("yes"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("mobi"));
    query.addQueryItem(QStringLiteral("keyword"), QString());
    query.addQueryItem(QStringLiteral("duration"), QString());
    query.addQueryItem(QStringLiteral("hash"), hash);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KuGouMusic/9108 (Android)"));

    m_candidateFetchId = fetchId;
    m_candidateFetchStep1Reply = m_network.get(req);
    LyricsLimits::attachSizeLimitGuard(m_candidateFetchStep1Reply);

    connect(m_candidateFetchStep1Reply, &QNetworkReply::finished, this, [this, reply = m_candidateFetchStep1Reply, base, fetchId] {
        if (!reply) return;
        reply->deleteLater();
        if (reply == m_candidateFetchStep1Reply) {
            m_candidateFetchStep1Reply = nullptr;
        }
        if (fetchId != m_candidateFetchId) {
            return;
        }
        if (LyricsLimits::isSizeLimitExceeded(reply) || reply->error() != QNetworkReply::NoError) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            return;
        }

        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        const QJsonArray cands = doc.object().value(QStringLiteral("candidates")).toArray();
        if (cands.isEmpty()) {
            Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            return;
        }

        const QJsonObject best = cands.first().toObject();
        const QString candId = best.value(QStringLiteral("id")).toString();
        const QString accessKey = best.value(QStringLiteral("accesskey")).toString();

        QUrl dlUrl(base.isEmpty()
            ? QStringLiteral("https://lyrics.kugou.com/download")
            : (base + QStringLiteral("/download")));
        QUrlQuery dlQuery;
        dlQuery.addQueryItem(QStringLiteral("ver"), QStringLiteral("1"));
        dlQuery.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
        dlQuery.addQueryItem(QStringLiteral("id"), candId);
        dlQuery.addQueryItem(QStringLiteral("accesskey"), accessKey);
        dlQuery.addQueryItem(QStringLiteral("fmt"), QStringLiteral("krc"));
        dlQuery.addQueryItem(QStringLiteral("charset"), QStringLiteral("utf8"));
        dlUrl.setQuery(dlQuery);

        QNetworkRequest dlReq(dlUrl);
        dlReq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KuGouMusic/9108 (Android)"));

        m_candidateFetchStep2Reply = m_network.get(dlReq);
        LyricsLimits::attachSizeLimitGuard(m_candidateFetchStep2Reply);

        connect(m_candidateFetchStep2Reply, &QNetworkReply::finished, this, [this, dlReply = m_candidateFetchStep2Reply, fetchId] {
            if (!dlReply) return;
            dlReply->deleteLater();
            if (dlReply == m_candidateFetchStep2Reply) {
                m_candidateFetchStep2Reply = nullptr;
            }
            if (fetchId != m_candidateFetchId) {
                return;
            }
            if (LyricsLimits::isSizeLimitExceeded(dlReply)) {
                Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
                return;
            }
            if (dlReply->error() != QNetworkReply::NoError) {
                Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
                return;
            }

            const QByteArray dlData = dlReply->readAll();
            const QJsonDocument dlDoc = QJsonDocument::fromJson(dlData);
            QString content = dlDoc.object().value(QStringLiteral("content")).toString();
            if (content.isEmpty()) {
                content = dlDoc.object().value(QStringLiteral("data")).toObject().value(QStringLiteral("content")).toString();
            }
            if (content.isEmpty()) {
                Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
                return;
            }

            Lyrics lyrics;
            bool limitExceeded = false;
            bool ok = KrcParser::parse(content.toUtf8(), &lyrics, 0, &limitExceeded);
            if (!ok && !limitExceeded) {
                const QByteArray lrcBytes = QByteArray::fromBase64(content.toUtf8());
                ok = LrcParser::parseLrc(QString::fromUtf8(lrcBytes), &lyrics, 0, &limitExceeded);
            }

            if (limitExceeded) {
                Q_EMIT candidateLyricsFailed(fetchId, MissingReason::SizeLimitExceeded, id());
                return;
            }

            if (ok && lyrics.isUsable()) {
                lyrics.attribution = QStringLiteral("KuGou (酷狗音乐)");
                lyrics.attributionData.provider = QStringLiteral("KuGou");
                lyrics.attributionData.providerUrl = QStringLiteral("https://www.kugou.com");
                Q_EMIT candidateLyricsLoaded(fetchId, lyrics, id());
            } else {
                Q_EMIT candidateLyricsFailed(fetchId, MissingReason::NotFound, id());
            }
        });
    });
}

void KugouProvider::cancelCandidateSearch(quint64 searchId)
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

void KugouProvider::cancelCandidateFetch(quint64 fetchId)
{
    if (fetchId == 0 || fetchId == m_candidateFetchId) {
        m_candidateFetchId = 0;
        if (m_candidateFetchStep1Reply) {
            QPointer<QNetworkReply> reply = m_candidateFetchStep1Reply;
            m_candidateFetchStep1Reply = nullptr;
            if (reply) {
                reply->abort();
                if (reply) {
                    reply->deleteLater();
                }
            }
        }
        if (m_candidateFetchStep2Reply) {
            QPointer<QNetworkReply> reply = m_candidateFetchStep2Reply;
            m_candidateFetchStep2Reply = nullptr;
            if (reply) {
                reply->abort();
                if (reply) {
                    reply->deleteLater();
                }
            }
        }
    }
}
