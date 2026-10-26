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

    // Build candidate list based on user priority order and initialize diagnostics
    m_providerDiagnostics.clear();
    int priority = 0;
    for (const QString &id : m_providerOrder) {
        LyricsProvider *p = provider(id);
        if (!p) {
            continue;
        }
        ProviderDiagnostic diag;
        diag.id = p->id();
        diag.displayName = p->displayName();
        diag.priority = priority;
        diag.cacheUsage = QStringLiteral("None");
        diag.syncQuality = QStringLiteral("None");

        if (!p->isEnabled()) {
            diag.status = QStringLiteral("disabled");
            diag.rejectionReason = QStringLiteral("Disabled in settings");
        } else if (!p->canHandle(meta)) {
            diag.status = QStringLiteral("skipped");
            if (p->capabilities().testFlag(LyricsProvider::Capability::RequiresSpotifyId) && meta.spotifyTrackId.isEmpty()) {
                diag.rejectionReason = QStringLiteral("Missing Spotify track ID");
            } else if (p->id() == QLatin1String("local") && meta.url.isEmpty()) {
                diag.rejectionReason = QStringLiteral("No local file URL");
            } else {
                diag.rejectionReason = QStringLiteral("Cannot handle track");
            }
        } else {
            diag.status = QStringLiteral("pending");
            diag.rejectionReason.clear();

            CandidateEntry entry;
            entry.provider = p;
            entry.priority = priority;
            entry.inFlight = false;
            entry.completed = false;
            m_candidates.append(entry);
        }
        ++priority;
        m_providerDiagnostics.insert(p->id(), diag);
    }

    for (LyricsProvider *p : m_providers) {
        if (!p || m_providerDiagnostics.contains(p->id())) {
            continue;
        }
        ProviderDiagnostic diag;
        diag.id = p->id();
        diag.displayName = p->displayName();
        diag.priority = priority++;
        diag.cacheUsage = QStringLiteral("None");
        diag.syncQuality = QStringLiteral("None");

        if (!p->isEnabled()) {
            diag.status = QStringLiteral("disabled");
            diag.rejectionReason = QStringLiteral("Disabled in settings");
        } else if (!p->canHandle(meta)) {
            diag.status = QStringLiteral("skipped");
            diag.rejectionReason = QStringLiteral("Cannot handle track");
        } else {
            diag.status = QStringLiteral("pending");
        }
        m_providerDiagnostics.insert(p->id(), diag);
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

    for (CandidateEntry &cand : m_candidates) {
        if (!cand.provider) continue;
        auto &diag = m_providerDiagnostics[cand.provider->id()];
        if (diag.status == QLatin1String("searching")) {
            diag.status = QStringLiteral("cancelled");
            diag.rejectionReason = QStringLiteral("Search cancelled");
        } else if (diag.status == QLatin1String("pending")) {
            diag.status = QStringLiteral("skipped");
            diag.rejectionReason = QStringLiteral("Search cancelled");
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
                if (cand.priority >= m_bestPriority) {
                    irrelevant = true;
                } else if (!canWord) {
                    irrelevant = true;
                }
            } else if (m_bestQuality == LyricsType::Line) {
                if (cand.priority > m_bestPriority && !canWord) {
                    irrelevant = true;
                }
            }
        }

        if (irrelevant) {
            if (cand.inFlight) {
                if (cand.provider) {
                    cand.provider->cancel();
                    auto &diag = m_providerDiagnostics[cand.provider->id()];
                    diag.status = QStringLiteral("cancelled");
                    diag.rejectionReason = QStringLiteral("Superseded by higher quality/priority result");
                }
                cand.inFlight = false;
            } else {
                if (cand.provider) {
                    auto &diag = m_providerDiagnostics[cand.provider->id()];
                    diag.status = QStringLiteral("skipped");
                    diag.rejectionReason = QStringLiteral("Superseded before starting");
                }
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

    if (!m_bestProviderId.isEmpty() && m_providerDiagnostics.contains(m_bestProviderId)) {
        m_providerDiagnostics[m_bestProviderId].status = QStringLiteral("selected");
    }

    for (CandidateEntry &cand : m_candidates) {
        if (!cand.provider) continue;
        if (cand.provider->id() == m_bestProviderId) {
            continue;
        }
        auto &diag = m_providerDiagnostics[cand.provider->id()];
        if (cand.inFlight) {
            cand.provider->cancel();
            diag.status = QStringLiteral("cancelled");
            diag.rejectionReason = hadResult ? QStringLiteral("Superseded by selected provider") : QStringLiteral("Search deadline reached");
        } else if (diag.status == QLatin1String("pending")) {
            diag.status = QStringLiteral("skipped");
            diag.rejectionReason = hadResult ? QStringLiteral("Superseded before starting") : QStringLiteral("Search deadline reached");
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
            if (cand.provider) {
                m_providerDiagnostics[cand.provider->id()].status = QStringLiteral("searching");
            }
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

    auto &diag = m_providerDiagnostics[providerId];
    if (foundCand->provider) {
        const QString cache = foundCand->provider->cacheUsage(generation);
        if (!cache.isEmpty()) {
            diag.cacheUsage = cache;
        }
    }

    if (!lyrics.isUsable()) {
        diag.status = QStringLiteral("failed");
        diag.syncQuality = (lyrics.type == LyricsType::Static ? QStringLiteral("Unsynced") : QStringLiteral("None"));
        diag.rejectionReason = (lyrics.type == LyricsType::Static ? QStringLiteral("Unsynced lyrics") : QStringLiteral("Empty or invalid lyrics"));
        m_lastReason = MissingReason::NotFound;
        pumpQueue();
        return;
    }

    const LyricsType quality = lyrics.type;
    const int candPrio = foundCand->priority;
    diag.syncQuality = (quality == LyricsType::Syllable ? QStringLiteral("Syllable") : QStringLiteral("Line"));

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

        if (quality == LyricsType::Syllable) {
            diag.status = QStringLiteral("selected");
        } else {
            diag.status = QStringLiteral("provisional");
        }

        const quint64 gen = m_currentGeneration;
        Q_EMIT lyricsResolved(gen, lyrics, providerId);

        if (gen != m_currentGeneration || !m_isLookupActive) {
            return;
        }
    } else {
        diag.status = QStringLiteral("skipped");
        diag.rejectionReason = QStringLiteral("Lower quality or priority than current selection");
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

    auto &diag = m_providerDiagnostics[providerId];
    diag.status = QStringLiteral("failed");
    diag.syncQuality = (reason == MissingReason::Unsynced ? QStringLiteral("Unsynced") : QStringLiteral("None"));
    if (foundCand->provider) {
        const QString cache = foundCand->provider->cacheUsage(generation);
        if (!cache.isEmpty()) {
            diag.cacheUsage = cache;
        }
        const QString specReason = foundCand->provider->confirmedRejectionReason(generation);
        if (!specReason.isEmpty()) {
            diag.rejectionReason = specReason;
        }
    }
    if (diag.rejectionReason.isEmpty()) {
        if (reason == MissingReason::Unsynced) {
            diag.rejectionReason = QStringLiteral("Unsynced lyrics");
        } else if (reason == MissingReason::NotFound) {
            diag.rejectionReason = QStringLiteral("Not found in provider database");
        } else {
            diag.rejectionReason = QStringLiteral("Network or server error (no details available)");
        }
    }

    pumpQueue();
}

QList<LyricsPipelineManager::ProviderDiagnostic> LyricsPipelineManager::providerDiagnostics() const
{
    QList<ProviderDiagnostic> result;
    for (const QString &id : m_providerOrder) {
        if (m_providerDiagnostics.contains(id)) {
            result.append(m_providerDiagnostics.value(id));
        }
    }
    for (auto it = m_providerDiagnostics.constBegin(); it != m_providerDiagnostics.constEnd(); ++it) {
        if (!m_providerOrder.contains(it.key())) {
            result.append(it.value());
        }
    }
    return result;
}
