/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "qqmusicprovider.h"
#include "lrcparser.h"
#include "qrcdecoder.h"
#include "trackname.h"

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

bool titlesMatch(const QString &candidate, const QString &target)
{
    const QString c = cleanTrackTitle(candidate).trimmed().toLower();
    const QString t = cleanTrackTitle(target).trimmed().toLower();
    return c == t || c.contains(t) || t.contains(c);
}
} // namespace

QqMusicProvider::QqMusicProvider(QObject *parent)
    : LyricsProvider(parent)
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

    const qint64 targetDurSec = m_meta.durationMs > 0 ? (m_meta.durationMs + 500) / 1000 : 0;
    QString matchedMid;

    for (const QJsonValue &v : songs) {
        const QJsonObject obj = v.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const qint64 interval = obj.value(QStringLiteral("interval")).toInt();
        const QString mid = obj.value(QStringLiteral("mid")).toString();

        if (mid.isEmpty()) {
            continue;
        }

        if (targetDurSec > 0 && interval > 0 && qAbs(targetDurSec - interval) > kMaxDurationDiffSec) {
            continue;
        }

        if (titlesMatch(name, m_meta.title)) {
            matchedMid = mid;
            break;
        }
    }

    if (matchedMid.isEmpty() && !songs.isEmpty()) {
        for (const QJsonValue &v : songs) {
            const QJsonObject obj = v.toObject();
            const qint64 interval = obj.value(QStringLiteral("interval")).toInt();
            const QString mid = obj.value(QStringLiteral("mid")).toString();
            if (mid.isEmpty()) {
                continue;
            }
            if (targetDurSec <= 0 || interval <= 0 || qAbs(targetDurSec - interval) <= kMaxDurationDiffSec) {
                matchedMid = mid;
                break;
            }
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
    if (!lyric.isEmpty() && QrcDecoder::parse(lyric, &lyrics) && lyrics.isUsable()) {
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
    if (!LrcParser::parseLrc(lyric, &lyrics, m_meta.durationMs) || !lyrics.isUsable()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attribution = QStringLiteral("QQ Music (QQ音乐)");
    lyrics.attributionData.provider = QStringLiteral("QQ Music");
    lyrics.attributionData.providerUrl = QStringLiteral("https://y.qq.com");

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}
