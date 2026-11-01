/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "candidatesearchcontroller.h"
#include "lrclibprovider.h"
#include "lrcredprovider.h"
#include "amllprovider.h"
#include "neteaseprovider.h"
#include "kugouprovider.h"
#include "qqmusicprovider.h"
#include "unisonprovider.h"

CandidateSearchController::CandidateSearchController(QObject *parent)
    : QObject(parent)
    , m_deadlineTimer(new QTimer(this))
    , m_fetchDeadlineTimer(new QTimer(this))
{
    m_deadlineTimer->setSingleShot(true);
    connect(m_deadlineTimer, &QTimer::timeout, this, &CandidateSearchController::onDeadlineTimeout);

    m_fetchDeadlineTimer->setSingleShot(true);
    connect(m_fetchDeadlineTimer, &QTimer::timeout, this, &CandidateSearchController::onFetchDeadlineTimeout);

    // Create dedicated provider instances for manual candidate search
    m_allProviders.append(new LrcRedProvider(this));
    m_allProviders.append(new AmllProvider(this));
    m_allProviders.append(new LrclibProvider(this));
    m_allProviders.append(new NeteaseProvider(this));
    m_allProviders.append(new KugouProvider(this));
    m_allProviders.append(new QqMusicProvider(this));
    m_allProviders.append(new UnisonProvider(this));

    for (LyricsProvider *p : m_allProviders) {
        connect(p, &LyricsProvider::candidatesFound, this, &CandidateSearchController::onCandidatesFound);
        connect(p, &LyricsProvider::candidateLyricsLoaded, this, &CandidateSearchController::onCandidateLyricsLoaded);
        connect(p, &LyricsProvider::candidateLyricsFailed, this, &CandidateSearchController::onCandidateLyricsFailed);
    }
}

CandidateSearchController::~CandidateSearchController()
{
    cancel();
}

void CandidateSearchController::registerProvider(LyricsProvider *provider)
{
    if (!provider) {
        return;
    }
    for (int i = 0; i < m_allProviders.size(); ++i) {
        if (m_allProviders[i]->id() == provider->id()) {
            m_allProviders[i]->deleteLater();
            m_allProviders[i] = provider;
            connect(provider, &LyricsProvider::candidatesFound, this, &CandidateSearchController::onCandidatesFound);
            connect(provider, &LyricsProvider::candidateLyricsLoaded, this, &CandidateSearchController::onCandidateLyricsLoaded);
            connect(provider, &LyricsProvider::candidateLyricsFailed, this, &CandidateSearchController::onCandidateLyricsFailed);
            return;
        }
    }
    m_allProviders.append(provider);
    connect(provider, &LyricsProvider::candidatesFound, this, &CandidateSearchController::onCandidatesFound);
    connect(provider, &LyricsProvider::candidateLyricsLoaded, this, &CandidateSearchController::onCandidateLyricsLoaded);
    connect(provider, &LyricsProvider::candidateLyricsFailed, this, &CandidateSearchController::onCandidateLyricsFailed);
}

LyricsProvider *CandidateSearchController::provider(const QString &id) const
{
    for (LyricsProvider *p : m_allProviders) {
        if (p->id() == id) {
            return p;
        }
    }
    return nullptr;
}

void CandidateSearchController::setSelectedIndex(int index)
{
    if (m_selectedIndex != index) {
        m_selectedIndex = index;
        Q_EMIT selectedIndexChanged();
    }
}

QString CandidateSearchController::formatDuration(qint64 ms) const
{
    if (ms <= 0) {
        return QStringLiteral("--:--");
    }
    const qint64 totalSec = ms / 1000;
    const qint64 minutes = totalSec / 60;
    const qint64 seconds = totalSec % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString CandidateSearchController::formatSyncType(CandidateSyncType type) const
{
    switch (type) {
    case CandidateSyncType::Word:
        return QStringLiteral("Word");
    case CandidateSyncType::Line:
        return QStringLiteral("Line");
    case CandidateSyncType::None:
        return QStringLiteral("None");
    case CandidateSyncType::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QVariantList CandidateSearchController::candidatesList() const
{
    QVariantList list;
    for (int i = 0; i < m_candidates.size(); ++i) {
        const LyricCandidate &c = m_candidates.at(i);
        QVariantMap map;
        map[QStringLiteral("index")] = i;
        map[QStringLiteral("id")] = c.id;
        map[QStringLiteral("providerId")] = c.providerId;
        map[QStringLiteral("sourceName")] = c.sourceName;
        map[QStringLiteral("title")] = c.title;
        map[QStringLiteral("artist")] = c.artist;
        map[QStringLiteral("durationMs")] = c.durationMs;
        map[QStringLiteral("durationText")] = formatDuration(c.durationMs);
        map[QStringLiteral("syncQualityText")] = formatSyncType(c.syncType);
        list.append(map);
    }
    return list;
}

void CandidateSearchController::startSearch(const TrackMetadata &meta,
                                           const QStringList &providerOrder,
                                           const QStringList &enabledProviders)
{
    cancel();

    m_currentMeta = meta;
    m_providerOrder = providerOrder;
    m_enabledProviders = enabledProviders;
    ++m_searchId;

    m_status = QStringLiteral("searching");
    m_errorMessage.clear();
    m_selectedIndex = -1;
    m_candidates.clear();
    m_activeProviders.clear();

    Q_EMIT statusChanged();
    Q_EMIT candidatesChanged();
    Q_EMIT selectedIndexChanged();

    if (m_enabledProviders.isEmpty()) {
        m_status = QStringLiteral("nomatches");
        Q_EMIT statusChanged();
        return;
    }

    // Order active providers according to user settings
    int priority = 0;
    for (const QString &id : m_providerOrder) {
        if (!m_enabledProviders.contains(id)) {
            continue;
        }
        for (LyricsProvider *p : m_allProviders) {
            if (p->id() == id && p->supportsCandidateSearch() && p->canHandle(meta)) {
                ProviderEntry entry;
                entry.provider = p;
                entry.priority = priority++;
                m_activeProviders.append(entry);
                break;
            }
        }
    }

    // Append any enabled provider not in providerOrder
    for (LyricsProvider *p : m_allProviders) {
        if (!m_enabledProviders.contains(p->id())) {
            continue;
        }
        if (!p->supportsCandidateSearch() || !p->canHandle(meta)) {
            continue;
        }
        bool alreadyAdded = false;
        for (const auto &entry : m_activeProviders) {
            if (entry.provider == p) {
                alreadyAdded = true;
                break;
            }
        }
        if (!alreadyAdded) {
            ProviderEntry entry;
            entry.provider = p;
            entry.priority = priority++;
            m_activeProviders.append(entry);
        }
    }

    if (m_activeProviders.isEmpty()) {
        m_status = QStringLiteral("nomatches");
        Q_EMIT statusChanged();
        return;
    }

    m_deadlineTimer->start(kDeadlineMs);
    pumpQueue();
}

void CandidateSearchController::searchAgain()
{
    startSearch(m_currentMeta, m_providerOrder, m_enabledProviders);
}

void CandidateSearchController::pumpQueue()
{
    if (m_status != QLatin1String("searching")) {
        return;
    }

    int inFlightCount = 0;
    int pendingIndex = -1;

    for (int i = 0; i < m_activeProviders.size(); ++i) {
        if (m_activeProviders[i].inFlight) {
            ++inFlightCount;
        } else if (!m_activeProviders[i].completed && pendingIndex == -1) {
            pendingIndex = i;
        }
    }

    if (inFlightCount == 0 && pendingIndex == -1) {
        finishSearch();
        return;
    }

    while (inFlightCount < m_maxConcurrent && pendingIndex != -1) {
        ProviderEntry &entry = m_activeProviders[pendingIndex];
        entry.inFlight = true;
        entry.provider->searchCandidates(m_currentMeta, m_searchId);
        ++inFlightCount;

        pendingIndex = -1;
        for (int i = 0; i < m_activeProviders.size(); ++i) {
            if (!m_activeProviders[i].inFlight && !m_activeProviders[i].completed) {
                pendingIndex = i;
                break;
            }
        }
    }
}

void CandidateSearchController::onCandidatesFound(quint64 searchId, const QList<LyricCandidate> &candidates, const QString &providerId)
{
    if (searchId != m_searchId || m_status != QLatin1String("searching")) {
        return;
    }

    for (auto &entry : m_activeProviders) {
        if (entry.provider && entry.provider->id() == providerId) {
            entry.inFlight = false;
            entry.completed = true;
            entry.foundCandidates = candidates;
            break;
        }
    }

    rebuildCandidatesList();
    pumpQueue();
}

void CandidateSearchController::rebuildCandidatesList()
{
    m_candidates.clear();
    for (const auto &entry : m_activeProviders) {
        m_candidates.append(entry.foundCandidates);
    }
    Q_EMIT candidatesChanged();
}

void CandidateSearchController::finishSearch()
{
    m_deadlineTimer->stop();
    rebuildCandidatesList();

    if (m_candidates.isEmpty()) {
        m_status = QStringLiteral("nomatches");
    } else {
        m_status = QStringLiteral("ready");
        if (m_selectedIndex < 0 || m_selectedIndex >= m_candidates.size()) {
            setSelectedIndex(0);
        }
    }
    Q_EMIT statusChanged();
}

void CandidateSearchController::onDeadlineTimeout()
{
    if (m_status == QLatin1String("searching")) {
        for (auto &entry : m_activeProviders) {
            if (entry.inFlight && entry.provider) {
                entry.provider->cancelCandidateSearch(m_searchId);
                entry.inFlight = false;
                entry.completed = true;
            }
        }
        finishSearch();
    }
}

void CandidateSearchController::useSelected()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= m_candidates.size()) {
        return;
    }

    const LyricCandidate cand = m_candidates.at(m_selectedIndex);
    LyricsProvider *targetProvider = nullptr;
    for (LyricsProvider *p : m_allProviders) {
        if (p->id() == cand.providerId) {
            targetProvider = p;
            break;
        }
    }

    if (!targetProvider) {
        m_status = QStringLiteral("error");
        m_errorMessage = QStringLiteral("Provider not found: %1").arg(cand.providerId);
        Q_EMIT statusChanged();
        Q_EMIT candidateLyricsFailed(m_errorMessage);
        return;
    }

    // Cancel any previous in-flight candidate fetch
    if (m_status == QLatin1String("loading") && !m_loadingProviderId.isEmpty()) {
        LyricsProvider *prev = provider(m_loadingProviderId);
        if (prev) {
            prev->cancelCandidateFetch(m_fetchId);
        }
    }

    ++m_fetchId;
    m_loadingProviderId = cand.providerId;
    m_status = QStringLiteral("loading");
    m_errorMessage.clear();
    Q_EMIT statusChanged();

    m_fetchDeadlineTimer->start(kFetchDeadlineMs);
    targetProvider->fetchCandidateLyrics(cand.id, m_fetchId);
}

void CandidateSearchController::onCandidateLyricsLoaded(quint64 fetchId, const Lyrics &lyrics, const QString &providerId)
{
    if (fetchId != m_fetchId || m_status != QLatin1String("loading")) {
        return;
    }

    m_fetchDeadlineTimer->stop();
    m_loadingProviderId.clear();
    m_status = QStringLiteral("ready");
    Q_EMIT statusChanged();
    Q_EMIT candidateLyricsLoaded(lyrics, providerId);
}

void CandidateSearchController::onCandidateLyricsFailed(quint64 fetchId, MissingReason reason, const QString &providerId)
{
    Q_UNUSED(providerId);
    if (fetchId != m_fetchId || m_status != QLatin1String("loading")) {
        return;
    }

    m_fetchDeadlineTimer->stop();
    m_loadingProviderId.clear();
    m_status = QStringLiteral("error");
    if (reason == MissingReason::SizeLimitExceeded) {
        m_errorMessage = QStringLiteral("Candidate response size limit exceeded (max 8 MiB)");
    } else if (reason == MissingReason::Unsynced) {
        m_errorMessage = QStringLiteral("Selected candidate has unsynced lyrics");
    } else {
        m_errorMessage = QStringLiteral("Failed to download selected lyrics");
    }

    Q_EMIT statusChanged();
    Q_EMIT candidateLyricsFailed(m_errorMessage);
}

void CandidateSearchController::onFetchDeadlineTimeout()
{
    if (m_status == QLatin1String("loading")) {
        if (!m_loadingProviderId.isEmpty()) {
            LyricsProvider *p = provider(m_loadingProviderId);
            if (p) {
                p->cancelCandidateFetch(m_fetchId);
            }
        }
        ++m_fetchId;
        m_loadingProviderId.clear();
        m_status = QStringLiteral("error");
        m_errorMessage = QStringLiteral("Timed out fetching selected lyrics");
        Q_EMIT statusChanged();
        Q_EMIT candidateLyricsFailed(m_errorMessage);
    }
}

void CandidateSearchController::cancel()
{
    m_deadlineTimer->stop();
    m_fetchDeadlineTimer->stop();
    for (auto &entry : m_activeProviders) {
        if (entry.inFlight && entry.provider) {
            entry.provider->cancelCandidateSearch(m_searchId);
            entry.inFlight = false;
        }
    }

    if (m_status == QLatin1String("loading") && !m_loadingProviderId.isEmpty()) {
        LyricsProvider *loadingP = provider(m_loadingProviderId);
        if (loadingP) {
            loadingP->cancelCandidateFetch(m_fetchId);
        }
    }

    ++m_searchId;
    ++m_fetchId;
    m_loadingProviderId.clear();
    m_status = QStringLiteral("idle");
    Q_EMIT statusChanged();
}

void CandidateSearchController::close()
{
    cancel();
}
