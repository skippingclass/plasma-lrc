/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "spicylyricsprovider.h"

SpicyLyricsProvider::SpicyLyricsProvider(QObject *parent)
    : LyricsProvider(parent)
    , m_spicy(new SpicyLyrics(this))
{
    connect(m_spicy, &SpicyLyrics::loaded, this, &SpicyLyricsProvider::onLoaded);
    connect(m_spicy, &SpicyLyrics::missing, this, &SpicyLyricsProvider::onMissing);
}

bool SpicyLyricsProvider::isConfigured() const
{
    return m_spicy->isEnabled() && !m_spicy->key().isEmpty();
}

bool SpicyLyricsProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.spotifyTrackId.isEmpty() && isConfigured();
}

void SpicyLyricsProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    m_currentGeneration = generation;
    m_diagGeneration = generation;
    m_diagReason.clear();
    m_diagCache = QStringLiteral("Miss");
    m_pendingTrackId = meta.spotifyTrackId;
    m_spicy->request(meta.spotifyTrackId);
}

void SpicyLyricsProvider::cancel()
{
    m_pendingTrackId.clear();
    m_spicy->cancel();
}

void SpicyLyricsProvider::invalidateCache(const TrackMetadata &meta)
{
    if (m_spicy && !meta.spotifyTrackId.isEmpty()) {
        if (meta.spotifyTrackId == m_pendingTrackId) {
            m_pendingTrackId.clear();
        }
        m_spicy->invalidate(meta.spotifyTrackId);
    }
}

void SpicyLyricsProvider::clearAllCaches()
{
    cancel();
    if (m_spicy) {
        m_spicy->clearCache();
    }
}

void SpicyLyricsProvider::onLoaded(const QString &trackId, const Lyrics &lyrics)
{
    if (trackId != m_pendingTrackId) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_diagGeneration = gen;
    m_diagReason.clear();
    m_pendingTrackId.clear();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void SpicyLyricsProvider::onMissing(const QString &trackId, MissingReason reason)
{
    if (trackId != m_pendingTrackId) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_diagGeneration = gen;

    if (m_spicy) {
        const int httpStatus = m_spicy->lastHttpStatus();
        const QString errCode = m_spicy->lastErrorCode();
        if (httpStatus == 429) {
            m_diagReason = QStringLiteral("Rate limit reached (HTTP 429)");
        } else if (httpStatus == 401) {
            m_diagReason = QStringLiteral("API key rejected (HTTP 401)");
        } else if (httpStatus == 403) {
            m_diagReason = QStringLiteral("Access denied (HTTP 403)");
        } else if (httpStatus == 404) {
            m_diagReason = QStringLiteral("Track not found in database (HTTP 404)");
        } else if (httpStatus >= 500) {
            m_diagReason = QStringLiteral("HTTP server error (%1)").arg(httpStatus);
        } else if (errCode == QLatin1String("application_suspended")) {
            m_diagReason = QStringLiteral("Application suspended");
        } else if (errCode == QLatin1String("invalid_response")) {
            m_diagReason = QStringLiteral("Invalid response format");
        } else if (reason == MissingReason::Unsynced) {
            m_diagReason = QStringLiteral("No synchronized lyrics available");
        } else if (reason == MissingReason::NotFound) {
            m_diagReason = QStringLiteral("Track not found in database");
        } else {
            m_diagReason = QStringLiteral("Network error or service unreachable");
        }
    }

    m_pendingTrackId.clear();
    Q_EMIT lyricsFailed(gen, reason, id());
}
