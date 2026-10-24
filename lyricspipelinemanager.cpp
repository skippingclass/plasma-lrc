/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lyricspipelinemanager.h"

LyricsPipelineManager::LyricsPipelineManager(QObject *parent)
    : QObject(parent)
{
    m_providerOrder = {
        QStringLiteral("lrcred"),
        QStringLiteral("amll"),
        QStringLiteral("spicy"),
        QStringLiteral("lrclib")
    };
}

LyricsPipelineManager::~LyricsPipelineManager()
{
    cancel();
}

void LyricsPipelineManager::registerProvider(LyricsProvider *provider)
{
    if (!provider || m_providers.contains(provider)) {
        return;
    }
    m_providers.append(provider);
    connect(provider, &LyricsProvider::lyricsLoaded, this, &LyricsPipelineManager::onProviderLoaded);
    connect(provider, &LyricsProvider::lyricsFailed, this, &LyricsPipelineManager::onProviderFailed);

    if (!m_providerOrder.contains(provider->id())) {
        m_providerOrder.append(provider->id());
    }
}

LyricsProvider *LyricsPipelineManager::provider(const QString &id) const
{
    for (LyricsProvider *p : m_providers) {
        if (p->id() == id) {
            return p;
        }
    }
    return nullptr;
}

void LyricsPipelineManager::setProviderOrder(const QStringList &order)
{
    QStringList filtered;
    for (const QString &id : order) {
        if (!filtered.contains(id)) {
            filtered.append(id);
        }
    }
    for (LyricsProvider *p : m_providers) {
        if (!filtered.contains(p->id())) {
            filtered.append(p->id());
        }
    }
    m_providerOrder = filtered;
}

bool LyricsPipelineManager::isProviderEnabled(const QString &id) const
{
    LyricsProvider *p = provider(id);
    return p ? p->isEnabled() : false;
}

void LyricsPipelineManager::setProviderEnabled(const QString &id, bool enabled)
{
    LyricsProvider *p = provider(id);
    if (p) {
        p->setEnabled(enabled);
    }
}

void LyricsPipelineManager::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_currentGeneration = generation;
    m_currentMeta = meta;
    m_activeCandidates.clear();
    m_candidateIndex = -1;
    m_currentProviderId.clear();
    m_lastReason = MissingReason::NotFound;

    // Build candidate list based on user priority order
    for (const QString &id : m_providerOrder) {
        LyricsProvider *p = provider(id);
        if (p && p->isEnabled() && p->canHandle(meta)) {
            m_activeCandidates.append(p);
        }
    }

    tryNextCandidate();
}

void LyricsPipelineManager::cancel()
{
    for (LyricsProvider *p : m_providers) {
        p->cancel();
    }
    m_activeCandidates.clear();
    m_candidateIndex = -1;
    m_currentProviderId.clear();
}

void LyricsPipelineManager::tryNextCandidate()
{
    ++m_candidateIndex;
    if (m_candidateIndex >= m_activeCandidates.size()) {
        const quint64 gen = m_currentGeneration;
        const MissingReason reason = m_lastReason;
        cancel();
        Q_EMIT allFailed(gen, reason);
        return;
    }

    LyricsProvider *next = m_activeCandidates.at(m_candidateIndex);
    m_currentProviderId = next->id();
    next->requestLyrics(m_currentMeta, m_currentGeneration);
}

void LyricsPipelineManager::onProviderLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId)
{
    if (generation != m_currentGeneration || providerId != m_currentProviderId) {
        return;
    }

    const quint64 gen = m_currentGeneration;
    const QString pid = m_currentProviderId;
    m_activeCandidates.clear();
    m_candidateIndex = -1;
    m_currentProviderId.clear();

    Q_EMIT lyricsResolved(gen, lyrics, pid);
}

void LyricsPipelineManager::onProviderFailed(quint64 generation, MissingReason reason, const QString &providerId)
{
    if (generation != m_currentGeneration || providerId != m_currentProviderId) {
        return;
    }

    m_lastReason = reason;
    tryNextCandidate();
}
