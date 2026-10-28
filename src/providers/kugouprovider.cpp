/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "kugouprovider.h"
#include "krcparser.h"
#include "lrcparser.h"
#include "trackmatch.h"
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
} // namespace

KugouProvider::KugouProvider(QObject *parent)
    : LyricsProvider(parent)
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
    bool parsed = false;

    if (isKrc) {
        parsed = KrcParser::parse(content.toUtf8(), &lyrics, m_meta.durationMs);
    } else {
        const QByteArray lrcBytes = QByteArray::fromBase64(content.toUtf8());
        parsed = LrcParser::parseLrc(QString::fromUtf8(lrcBytes), &lyrics, m_meta.durationMs);
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
