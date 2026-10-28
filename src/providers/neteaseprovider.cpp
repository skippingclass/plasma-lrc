/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "neteaseprovider.h"
#include "lrcparser.h"
#include "trackmatch.h"
#include "trackname.h"
#include "yrcparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace
{
constexpr int kTimeoutMs = 7000;
constexpr int kMaxDurationDiffMs = 4000;
} // namespace

NeteaseProvider::NeteaseProvider(QObject *parent)
    : LyricsProvider(parent)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &NeteaseProvider::onTimeout);
}

NeteaseProvider::~NeteaseProvider()
{
    cancel();
}

bool NeteaseProvider::canHandle(const TrackMetadata &meta) const
{
    return m_enabled && !meta.title.trimmed().isEmpty();
}

void NeteaseProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_meta = meta;
    m_currentGeneration = generation;
    m_step = Step::SearchSong;

    startSearch();
}

void NeteaseProvider::cancel()
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

void NeteaseProvider::onTimeout()
{
    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
}

void NeteaseProvider::onReplyFinished()
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
    case Step::FetchLyrics:
        handleLyricsReply(data);
        break;
    default:
        break;
    }
}

void NeteaseProvider::startSearch()
{
    QUrl url(QStringLiteral("https://music.163.com/api/cloudsearch/pc"));
    QUrlQuery query;
    const QString kw = (m_meta.artist.trimmed() + QLatin1Char(' ') + m_meta.title.trimmed()).trimmed();
    query.addQueryItem(QStringLiteral("s"), kw);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("offset"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("8"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) NetEase"));
    req.setRawHeader("Referer", "https://music.163.com");

    m_currentReply = m_network.get(req);
    connect(m_currentReply, &QNetworkReply::finished, this, &NeteaseProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void NeteaseProvider::handleSearchReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonArray songs = doc.object().value(QStringLiteral("result")).toObject().value(QStringLiteral("songs")).toArray();
    if (songs.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    QString matchedId;

    for (const QJsonValue &v : songs) {
        const QJsonObject obj = v.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const qint64 dt = obj.value(QStringLiteral("dt")).toVariant().toLongLong();
        const QString songId = QString::number(obj.value(QStringLiteral("id")).toVariant().toLongLong());

        if (songId.isEmpty() || songId == QLatin1String("0")) {
            continue;
        }

        // Parse artists from NetEase in-source fields: 'ar' or 'artists'
        QStringList artistList;
        QJsonArray arArray = obj.value(QStringLiteral("ar")).toArray();
        if (arArray.isEmpty()) {
            arArray = obj.value(QStringLiteral("artists")).toArray();
        }
        for (const QJsonValue &a : arArray) {
            const QString aName = a.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (!aName.isEmpty()) {
                artistList.append(aName);
            }
        }
        if (artistList.isEmpty() && obj.contains(QStringLiteral("artist"))) {
            const QString aName = obj.value(QStringLiteral("artist")).toString().trimmed();
            if (!aName.isEmpty()) {
                artistList.append(aName);
            }
        }

        if (TrackMatch::matchCandidate(m_meta, name, artistList, dt, kMaxDurationDiffMs)) {
            matchedId = songId;
            break;
        }
    }

    if (matchedId.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    m_songId = matchedId;
    m_step = Step::FetchLyrics;
    startFetchLyrics(m_songId);
}

void NeteaseProvider::startFetchLyrics(const QString &songId)
{
    QUrl url(QStringLiteral("https://music.163.com/api/song/lyric"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("os"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("id"), songId);
    query.addQueryItem(QStringLiteral("lv"), QStringLiteral("-1"));
    query.addQueryItem(QStringLiteral("kv"), QStringLiteral("-1"));
    query.addQueryItem(QStringLiteral("tv"), QStringLiteral("-1"));
    query.addQueryItem(QStringLiteral("rv"), QStringLiteral("-1"));
    query.addQueryItem(QStringLiteral("yv"), QStringLiteral("-1"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) NetEase"));
    req.setRawHeader("Referer", "https://music.163.com");

    m_currentReply = m_network.get(req);
    connect(m_currentReply, &QNetworkReply::finished, this, &NeteaseProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void NeteaseProvider::handleLyricsReply(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    const QJsonObject root = doc.object();
    const QString yrc = root.value(QStringLiteral("yrc")).toObject().value(QStringLiteral("lyric")).toString();
    const QString lrc = root.value(QStringLiteral("lrc")).toObject().value(QStringLiteral("lyric")).toString();

    Lyrics lyrics;
    bool parsed = false;

    if (!yrc.trimmed().isEmpty()) {
        parsed = YrcParser::parse(yrc, &lyrics, m_meta.durationMs);
    }

    if (!parsed || !lyrics.isUsable()) {
        if (!lrc.trimmed().isEmpty()) {
            parsed = LrcParser::parseLrc(lrc, &lyrics, m_meta.durationMs);
        }
    }

    if (!parsed || !lyrics.isUsable()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attribution = QStringLiteral("NetEase (网易云音乐)");
    lyrics.attributionData.provider = QStringLiteral("NetEase");
    lyrics.attributionData.providerUrl = QStringLiteral("https://music.163.com");

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}
