/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lyricspipelinemanager.h"

#include <QTimer>

LyricsPipelineManager::LyricsPipelineManager(QObject *parent)
    : QObject(parent)
{
    m_providerOrder = {
        QStringLiteral("lrcred"),
        QStringLiteral("amll"),
        QStringLiteral("spicy"),
        QStringLiteral("lrclib")
    };

    m_deadlineTimer = new QTimer(this);
    m_deadlineTimer->setSingleShot(true);
    connect(m_deadlineTimer, &QTimer::timeout, this, &LyricsPipelineManager::onLookupDeadline);
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

QString LyricsPipelineManager::currentProviderId() const
{
    for (const auto &cand : m_candidates) {
        if (cand.inFlight && cand.provider) {
            return cand.provider->id();
        }
    }
    return m_bestProviderId;
}

void LyricsPipelineManager::invalidateCaches(const TrackMetadata &meta)
{
    for (LyricsProvider *p : m_providers) {
        if (p) {
            p->invalidateCache(meta);
        }
    }
}

void LyricsPipelineManager::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_currentGeneration = generation;
    m_currentMeta = meta;
    m_lastReason = MissingReason::NotFound;
    m_hasEmittedResult = false;
    m_bestQuality = LyricsType::None;
    m_bestPriority = -1;
    m_bestProviderId.clear();

    // Build candidate list based on user priority order
    int priority = 0;
    for (const QString &id : m_providerOrder) {
        LyricsProvider *p = provider(id);
        if (p && p->isEnabled() && p->canHandle(meta)) {
            CandidateEntry entry;
            entry.provider = p;
            entry.priority = priority++;
            entry.inFlight = false;
            entry.completed = false;
            m_candidates.append(entry);
        }
    }

    if (m_candidates.isEmpty()) {
        const quint64 gen = m_currentGeneration;
        const MissingReason reason = m_lastReason;
        Q_EMIT allFailed(gen, reason);
        if (gen == m_currentGeneration) {
            Q_EMIT lookupFinished(gen);
        }
        return;
    }

    m_isLookupActive = true;
    if (m_deadlineTimer && m_lookupTimeoutMs > 0) {
        m_deadlineTimer->start(m_lookupTimeoutMs);
    }

    pumpQueue();
}

void LyricsPipelineManager::cancel()
{
    if (m_deadlineTimer) {
        m_deadlineTimer->stop();
    }

    m_isLookupActive = false;

    for (LyricsProvider *p : m_providers) {
        if (p) {
            p->cancel();
        }
    }

    m_candidates.clear();
    m_hasEmittedResult = false;
    m_bestQuality = LyricsType::None;
    m_bestPriority = -1;
    m_bestProviderId.clear();
    m_lastReason = MissingReason::NotFound;
}

void LyricsPipelineManager::pruneIrrelevantCandidates()
{
    if (!m_isLookupActive) {
        return;
    }

    for (int i = 0; i < m_candidates.size(); ++i) {
        CandidateEntry &cand = m_candidates[i];
        if (cand.completed) {
            continue;
        }

        const bool canWord = cand.provider &&
            cand.provider->capabilities().testFlag(LyricsProvider::Capability::WordSync);

        bool irrelevant = false;

        if (m_hasEmittedResult) {
            if (m_bestQuality == LyricsType::Syllable) {
                // Best quality is Syllable.
                // Any candidate with cand.priority >= m_bestPriority is irrelevant.
                // Any candidate with cand.priority < m_bestPriority is ONLY relevant
                // if it is a potential word source (canWord). If !canWord, it cannot beat Syllable.
                if (cand.priority >= m_bestPriority) {
                    irrelevant = true;
                } else if (!canWord) {
                    irrelevant = true;
                }
            } else if (m_bestQuality == LyricsType::Line) {
                // Best quality is Line.
                // Candidates with cand.priority < m_bestPriority can supersede (with Line) or upgrade (with Syllable).
                // Candidates with cand.priority > m_bestPriority can ONLY beat Line if capable of WordSync.
                if (cand.priority > m_bestPriority && !canWord) {
                    irrelevant = true;
                }
            }
        }

        if (irrelevant) {
            if (cand.inFlight) {
                if (cand.provider) {
                    cand.provider->cancel();
                }
                cand.inFlight = false;
            }
            cand.completed = true;
        }
    }
}

void LyricsPipelineManager::finishLookup()
{
    if (!m_isLookupActive) {
        return;
    }

    if (m_deadlineTimer) {
        m_deadlineTimer->stop();
    }

    const quint64 gen = m_currentGeneration;
    const bool hadResult = m_hasEmittedResult;
    const MissingReason lastReason = m_lastReason;

    m_isLookupActive = false;
    for (CandidateEntry &cand : m_candidates) {
        if (cand.inFlight && cand.provider) {
            cand.provider->cancel();
        }
        cand.inFlight = false;
        cand.completed = true;
    }

    if (!hadResult) {
        Q_EMIT allFailed(gen, lastReason);
    }
    if (gen == m_currentGeneration) {
        Q_EMIT lookupFinished(gen);
    }
}

void LyricsPipelineManager::onLookupDeadline()
{
    if (!m_isLookupActive) {
        return;
    }
    finishLookup();
}

void LyricsPipelineManager::pumpQueue()
{
    if (m_pumping || !m_isLookupActive) {
        return;
    }
    m_pumping = true;

    while (m_isLookupActive) {
        const quint64 gen = m_currentGeneration;
        pruneIrrelevantCandidates();

        int inFlightCount = 0;
        int pendingIndex = -1;

        for (int i = 0; i < m_candidates.size(); ++i) {
            if (m_candidates[i].inFlight) {
                ++inFlightCount;
            } else if (!m_candidates[i].completed && pendingIndex == -1) {
                pendingIndex = i;
            }
        }

        if (inFlightCount == 0 && pendingIndex == -1) {
            finishLookup();
            continue;
        }

        if (inFlightCount < m_maxConcurrentLookups && pendingIndex != -1) {
            CandidateEntry &cand = m_candidates[pendingIndex];
            cand.inFlight = true;
            LyricsProvider *p = cand.provider;
            const TrackMetadata meta = m_currentMeta;

            p->requestLyrics(meta, gen);
            continue;
        }

        break;
    }

    m_pumping = false;
}

void LyricsPipelineManager::onProviderLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId)
{
    if (generation != m_currentGeneration || !m_isLookupActive) {
        return;
    }

    CandidateEntry *foundCand = nullptr;
    for (auto &cand : m_candidates) {
        if (cand.provider && cand.provider->id() == providerId) {
            foundCand = &cand;
            break;
        }
    }

    if (!foundCand || !foundCand->inFlight) {
        return;
    }

    foundCand->inFlight = false;
    foundCand->completed = true;

    if (!lyrics.isUsable()) {
        m_lastReason = MissingReason::NotFound;
        pumpQueue();
        return;
    }

    const LyricsType quality = lyrics.type;
    const int candPrio = foundCand->priority;

    bool accept = false;
    if (!m_hasEmittedResult) {
        accept = true;
    } else {
        if (quality == LyricsType::Syllable && m_bestQuality != LyricsType::Syllable) {
            accept = true;
        } else if (quality == m_bestQuality) {
            if (candPrio < m_bestPriority) {
                accept = true;
            }
        }
    }

    if (accept) {
        m_hasEmittedResult = true;
        m_bestQuality = quality;
        m_bestPriority = candPrio;
        m_bestProviderId = providerId;

        const quint64 gen = m_currentGeneration;
        Q_EMIT lyricsResolved(gen, lyrics, providerId);

        if (gen != m_currentGeneration || !m_isLookupActive) {
            return;
        }
    }

    pumpQueue();
}

void LyricsPipelineManager::onProviderFailed(quint64 generation, MissingReason reason, const QString &providerId)
{
    if (generation != m_currentGeneration || !m_isLookupActive) {
        return;
    }

    CandidateEntry *foundCand = nullptr;
    for (auto &cand : m_candidates) {
        if (cand.provider && cand.provider->id() == providerId) {
            foundCand = &cand;
            break;
        }
    }

    if (!foundCand || !foundCand->inFlight) {
        return;
    }

    foundCand->inFlight = false;
    foundCand->completed = true;
    m_lastReason = reason;

    pumpQueue();
}
