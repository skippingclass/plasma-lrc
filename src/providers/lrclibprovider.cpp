/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrclibprovider.h"

LrclibProvider::LrclibProvider(QObject *parent, const QUrl &apiBase)
    : LyricsProvider(parent)
    , m_client(new LrclibClient(this, apiBase))
{
    connect(m_client, &LrclibClient::loaded, this, &LrclibProvider::onLoaded);
    connect(m_client, &LrclibClient::missing, this, &LrclibProvider::onMissing);
    connect(m_client, &LrclibClient::candidateSearchFailed, this, [this](quint64 sid, MissingReason reason, int retryAfter) {
        Q_EMIT candidateSearchFailed(sid, reason, id(), retryAfter);
    });
    connect(m_client, &LrclibClient::candidatesFound, this, [this](quint64 sid, const QList<LyricCandidate> &cands) {
        Q_EMIT candidatesFound(sid, cands, id());
    });
    connect(m_client, &LrclibClient::candidateLyricsLoaded, this, [this](quint64 sid, const Lyrics &lyrics) {
        Q_EMIT candidateLyricsLoaded(sid, lyrics, id());
    });
    connect(m_client, &LrclibClient::candidateLyricsFailed, this, [this](quint64 sid, MissingReason reason) {
        Q_EMIT candidateLyricsFailed(sid, reason, id());
    });
}

bool LrclibProvider::canHandle(const TrackMetadata &meta) const
{
    return !meta.title.isEmpty();
}

void LrclibProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    m_currentGeneration = generation;
    m_pendingKey = m_client->request(meta);
}

void LrclibProvider::cancel()
{
    m_pendingKey.clear();
    m_client->cancel();
}

void LrclibProvider::invalidateCache(const TrackMetadata &meta)
{
    if (m_client) {
        m_client->invalidate(meta);
        if (m_client->currentKey().isEmpty()) {
            m_pendingKey.clear();
        }
    }
}

void LrclibProvider::clearAllCaches()
{
    if (m_client) {
        m_client->clearCache();
    }
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

void LrclibProvider::onMissing(const QString &requestKey, MissingReason reason)
{
    if (requestKey != m_pendingKey) {
        return;
    }
    const quint64 gen = m_currentGeneration;
    m_pendingKey.clear();
    Q_EMIT lyricsFailed(gen, reason, id());
}

void LrclibProvider::searchCandidates(const TrackMetadata &meta, quint64 searchId)
{
    if (m_client) {
        m_client->searchCandidates(meta, searchId);
    }
}

void LrclibProvider::fetchCandidateLyrics(const QString &candidateId, quint64 fetchId)
{
    if (m_client) {
        m_client->fetchCandidate(candidateId, fetchId);
    }
}

void LrclibProvider::cancelCandidateSearch(quint64 searchId)
{
    if (m_client) {
        m_client->cancelCandidateSearch(searchId);
    }
}

void LrclibProvider::cancelCandidateFetch(quint64 fetchId)
{
    if (m_client) {
        m_client->cancelCandidateFetch(fetchId);
    }
}
