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
    m_pendingTrackId = meta.spotifyTrackId;
    m_spicy->request(meta.spotifyTrackId);
}

void SpicyLyricsProvider::cancel()
{
    m_pendingTrackId.clear();
    m_spicy->cancel();
}

void SpicyLyricsProvider::onLoaded(const QString &trackId, const Lyrics &lyrics)
{
    if (trackId != m_pendingTrackId) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_pendingTrackId.clear();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void SpicyLyricsProvider::onMissing(const QString &trackId, MissingReason reason)
{
    if (trackId != m_pendingTrackId) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_pendingTrackId.clear();
    Q_EMIT lyricsFailed(gen, reason, id());
}
