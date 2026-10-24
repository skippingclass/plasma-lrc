/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrclibprovider.h"

LrclibProvider::LrclibProvider(QObject *parent)
    : LyricsProvider(parent)
    , m_client(new LrclibClient(this))
{
    connect(m_client, &LrclibClient::loaded, this, &LrclibProvider::onLoaded);
    connect(m_client, &LrclibClient::missing, this, &LrclibProvider::onMissing);
}

bool LrclibProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.title.isEmpty();
}

void LrclibProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    m_currentGeneration = generation;
    m_pendingKey = m_client->request(meta.title, meta.artist, meta.durationMs, meta.isWebSession);
}

void LrclibProvider::cancel()
{
    m_pendingKey.clear();
    m_client->cancel();
}

void LrclibProvider::onLoaded(const QString &requestKey, const Lyrics &lyrics)
{
    if (requestKey != m_pendingKey) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_pendingKey.clear();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

void LrclibProvider::onMissing(const QString &requestKey)
{
    if (requestKey != m_pendingKey) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_pendingKey.clear();
    Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
}
