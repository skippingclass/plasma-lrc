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
#include <QXmlStreamReader>

namespace
{
constexpr qint64 kAmllCacheTtlMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kNegativeCacheTtlMs = 24LL * 60 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10000;
const QByteArray s_userAgent = QByteArrayLiteral("plasma-lrc/1.8.0 (https://github.com/skippingclass/plasma-lrc)");
} // namespace

AmllProvider::AmllProvider(QObject *parent, const QString &cdnBase, const QString &fallbackBase)
    : LyricsProvider(parent)
    , m_cdnBase(cdnBase)
    , m_fallbackBase(fallbackBase)
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
    m_currentGeneration = 0;
}

void AmllProvider::invalidateCache(const TrackMetadata &meta)
{
    const QString trackId = meta.spotifyTrackId.trimmed();
    if (trackId.isEmpty()) {
        return;
    }

    if (m_pendingTrackId == trackId) {
        cancel();
    }

    m_negativeCache.remove(trackId);

    const QString diskPath = cacheDirectory() + QStringLiteral("/") + trackId + QStringLiteral(".ttml");
    if (QFile::exists(diskPath)) {
        QFile::remove(diskPath);
    }
}

void AmllProvider::executeFetch(const QString &trackId, bool isFallback)
{
    QString urlStr = isFallback ? m_fallbackBase : m_cdnBase;
    if (urlStr.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        m_pendingTrackId.clear();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
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

    if (trackId.isEmpty() || gen == 0) {
        return;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
        if (!isFallback && !m_fallbackBase.isEmpty() && (statusCode == 404 || statusCode == 0 || statusCode >= 500)) {
            // Try fallback mirror before failing
            executeFetch(trackId, true);
            return;
        }

        m_pendingTrackId.clear();
        // Negative cache ONLY on confirmed 404 Not Found!
        // Never on transient network errors, 429, 5xx, or status 0.
        if (statusCode == 404) {
            m_negativeCache.insert(trackId, QDateTime::currentMSecsSinceEpoch());
            Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        } else {
            Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        }
        return;
    }

    const QByteArray data = reply->readAll();
    m_pendingTrackId.clear();

    const QString text = QString::fromUtf8(data);
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
        || trimmed.startsWith(QLatin1String("<html"), Qt::CaseInsensitive)
        || !trimmed.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
        // Malformed transient response (empty, HTML error page, or not TTML)
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    // Check XML well-formedness
    QXmlStreamReader xmlReader(trimmed);
    while (!xmlReader.atEnd()) {
        xmlReader.readNext();
    }
    if (xmlReader.hasError()) {
        // Malformed / truncated XML transient response
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    Lyrics lyrics;
    if (!TtmlParser::parseTtml(trimmed, &lyrics, durationMs) || !lyrics.isUsable()) {
        // Well-formed TTML document, but confirmed unsynced
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
