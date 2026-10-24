/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "amllprovider.h"
#include "ttmlparser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

namespace
{
constexpr qint64 kAmllCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.7.0 (https://github.com/skippingclass/plasma-lrc)");
} // namespace

AmllProvider::AmllProvider(QObject *parent, const QString &cdnBase)
    : LyricsProvider(parent)
    , m_cdnBase(cdnBase)
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
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/plasma-lrc");
    }
    return base + QStringLiteral("/amll");
}

bool AmllProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.spotifyTrackId.isEmpty();
}

void AmllProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_currentGeneration = generation;
    m_pendingTrackId = meta.spotifyTrackId;
    m_pendingDurationMs = meta.durationMs;

    // Check negative cache
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_negativeCache.contains(m_pendingTrackId)) {
        if (now - m_negativeCache.value(m_pendingTrackId) < kNegativeCacheTtlMs) {
            const quint64 gen = m_currentGeneration;
            m_pendingTrackId.clear();
            Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
            return;
        }
        m_negativeCache.remove(m_pendingTrackId);
    }

    // Check disk cache
    Lyrics cachedLyrics;
    if (checkDiskCache(m_pendingTrackId, m_pendingDurationMs, &cachedLyrics)) {
        const quint64 gen = m_currentGeneration;
        m_pendingTrackId.clear();
        Q_EMIT lyricsLoaded(gen, cachedLyrics, id());
        return;
    }

    executeFetch(m_pendingTrackId, false);
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
    m_pendingTrackId.clear();
}

void AmllProvider::executeFetch(const QString &trackId, bool isFallback)
{
    QString urlStr = m_cdnBase;
    if (isFallback) {
        urlStr = QStringLiteral("https://raw.githubusercontent.com/amll-dev/amll-ttml-db/main/spotify-lyrics/");
    }
    if (!urlStr.endsWith(QLatin1Char('/'))) {
        urlStr.append(QLatin1Char('/'));
    }
    urlStr.append(trackId).append(QStringLiteral(".ttml"));

    QNetworkRequest request{QUrl(urlStr)};
    request.setHeader(QNetworkRequest::UserAgentHeader, s_userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_currentReply = m_network.get(request);
    m_currentReply->setProperty("isFallback", isFallback);
    connect(m_currentReply, &QNetworkReply::finished, this, &AmllProvider::onReplyFinished);

    m_watchdog.start(kRequestTimeoutMs);
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
    const QString trackId = m_pendingTrackId;
    const qint64 durationMs = m_pendingDurationMs;
    const bool isFallback = reply->property("isFallback").toBool();

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
        if (!isFallback && (statusCode == 404 || statusCode == 0 || statusCode >= 500)) {
            // Try GitHub raw mirror before failing
            executeFetch(trackId, true);
            return;
        }

        m_pendingTrackId.clear();
        m_negativeCache.insert(trackId, QDateTime::currentMSecsSinceEpoch());
        const MissingReason reason = (statusCode == 404) ? MissingReason::NotFound : MissingReason::Unreachable;
        Q_EMIT lyricsFailed(gen, reason, id());
        return;
    }

    const QByteArray data = reply->readAll();
    m_pendingTrackId.clear();

    Lyrics lyrics;
    if (!TtmlParser::parseTtml(QString::fromUtf8(data), &lyrics, durationMs)) {
        m_negativeCache.insert(trackId, QDateTime::currentMSecsSinceEpoch());
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attributionData.provider = QStringLiteral("AMLL TTML DB");
    lyrics.attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
    lyrics.attribution = QStringLiteral("AMLL TTML DB");
    lyrics.attributionUrl = lyrics.attributionData.providerUrl;

    writeDiskCache(trackId, data);
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void AmllProvider::onTimeout()
{
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
    }
}

bool AmllProvider::checkDiskCache(const QString &trackId, qint64 durationMs, Lyrics *outLyrics)
{
    if (!outLyrics || trackId.isEmpty()) {
        return false;
    }
    const QString path = cacheDirectory() + QStringLiteral("/") + trackId + QStringLiteral(".ttml");
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }

    const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - info.lastModified().toMSecsSinceEpoch();
    if (ageMs > kAmllCacheTtlMs) {
        QFile::remove(path);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QString content = QString::fromUtf8(file.readAll());
    if (TtmlParser::parseTtml(content, outLyrics, durationMs)) {
        outLyrics->attributionData.provider = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionData.providerUrl = QStringLiteral("https://github.com/amll-dev/amll-ttml-db");
        outLyrics->attribution = QStringLiteral("AMLL TTML DB");
        outLyrics->attributionUrl = outLyrics->attributionData.providerUrl;
        return true;
    }
    return false;
}

void AmllProvider::writeDiskCache(const QString &trackId, const QByteArray &data)
{
    if (trackId.isEmpty() || data.isEmpty()) {
        return;
    }
    const QString dirPath = cacheDirectory();
    QDir().mkpath(dirPath);
    QFile file(dirPath + QStringLiteral("/") + trackId + QStringLiteral(".ttml"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(data);
    }
}
