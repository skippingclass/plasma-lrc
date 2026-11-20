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
#include "trackname.h"

#include <QDateTime>
#include <QScopedValueRollback>
#include <utility>

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
        connect(p, &LyricsProvider::candidateSearchFailed, this, &CandidateSearchController::onCandidateSearchFailed);
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
            connect(provider, &LyricsProvider::candidateSearchFailed, this, &CandidateSearchController::onCandidateSearchFailed);
            connect(provider, &LyricsProvider::candidateLyricsLoaded, this, &CandidateSearchController::onCandidateLyricsLoaded);
            connect(provider, &LyricsProvider::candidateLyricsFailed, this, &CandidateSearchController::onCandidateLyricsFailed);
            return;
        }
    }
    m_allProviders.append(provider);
    connect(provider, &LyricsProvider::candidatesFound, this, &CandidateSearchController::onCandidatesFound);
    connect(provider, &LyricsProvider::candidateSearchFailed, this, &CandidateSearchController::onCandidateSearchFailed);
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
    if (m_status == QLatin1String("error")) {
        m_status = QStringLiteral("ready");
        m_errorMessage.clear();
        Q_EMIT statusChanged();
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

QString CandidateSearchController::makeTrackContextKey(const TrackMetadata &meta)
{
    const QString expTitle = meta.effectiveExpectedTitle().trimmed().toLower();
    const QString expArtist = meta.effectiveExpectedArtist().trimmed().toLower();
    const qint64 durSec = meta.durationMs > 0 ? (meta.durationMs / 1000) : 0;
    return QStringLiteral("%1\t%2\t%3").arg(expTitle, expArtist, QString::number(durSec));
}

qint64 CandidateSearchController::estimateCandidateSizeBytes(const QList<LyricCandidate> &candidates)
{
    qint64 bytes = sizeof(QList<LyricCandidate>);
    for (const auto &c : candidates) {
        bytes += sizeof(LyricCandidate);
        bytes += c.id.size() * sizeof(QChar);
        bytes += c.providerId.size() * sizeof(QChar);
        bytes += c.sourceName.size() * sizeof(QChar);
        bytes += c.title.size() * sizeof(QChar);
        bytes += c.artist.size() * sizeof(QChar);
    }
    return bytes;
}

void CandidateSearchController::purgeExpiredCache()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_trackCache.begin(); it != m_trackCache.end();) {
        qint64 contextBytes = 0;
        for (auto resIt = it.value().providerResults.begin(); resIt != it.value().providerResults.end();) {
            if (now - resIt.value().timestampMs > kStaleTtlMs) {
                resIt = it.value().providerResults.erase(resIt);
            } else {
                contextBytes += estimateCandidateSizeBytes(resIt.value().candidates);
                ++resIt;
            }
        }
        if (it.value().providerResults.isEmpty()) {
            m_totalCacheSizeBytes -= it.value().estimatedSizeBytes;
            it = m_trackCache.erase(it);
        } else {
            m_totalCacheSizeBytes -= it.value().estimatedSizeBytes;
            it.value().estimatedSizeBytes = contextBytes;
            m_totalCacheSizeBytes += contextBytes;
            ++it;
        }
    }
    if (m_totalCacheSizeBytes < 0) {
        m_totalCacheSizeBytes = 0;
    }
}

void CandidateSearchController::evictCacheIfNeeded()
{
    while (m_trackCache.size() > kMaxTrackContexts || m_totalCacheSizeBytes > kMaxCacheSizeBytes) {
        if (m_trackCache.isEmpty()) {
            break;
        }
        auto oldestIt = m_trackCache.begin();
        qint64 oldestTime = oldestIt.value().lastAccessedMs;
        for (auto it = m_trackCache.begin(); it != m_trackCache.end(); ++it) {
            if (it.value().lastAccessedMs < oldestTime) {
                oldestTime = it.value().lastAccessedMs;
                oldestIt = it;
            }
        }
        m_totalCacheSizeBytes -= oldestIt.value().estimatedSizeBytes;
        m_trackCache.erase(oldestIt);
    }
    if (m_totalCacheSizeBytes < 0) {
        m_totalCacheSizeBytes = 0;
    }
}

QVariantList CandidateSearchController::candidatesList() const
{
    QVariantList list;
    for (int i = 0; i < m_candidates.size(); ++i) {
        const LyricCandidate &c = m_candidates.at(i);
        const QString candKey = c.providerId + QLatin1Char(':') + c.id;
        const bool isStale = m_candidateStaleMap.value(candKey, false);

        QVariantMap map;
        map[QStringLiteral("index")] = i;
        map[QStringLiteral("id")] = c.id;
        map[QStringLiteral("providerId")] = c.providerId;
        map[QStringLiteral("sourceName")] = isStale ? QStringLiteral("%1 (Cached)").arg(c.sourceName) : c.sourceName;
        map[QStringLiteral("isStale")] = isStale;
        map[QStringLiteral("title")] = c.title;
        map[QStringLiteral("artist")] = c.artist;
        map[QStringLiteral("durationMs")] = c.durationMs;
        map[QStringLiteral("durationText")] = formatDuration(c.durationMs);
        map[QStringLiteral("syncQualityText")] = formatSyncType(c.syncType);
        list.append(map);
    }
    return list;
}

void CandidateSearchController::startSearch(const TrackMetadata &rawMeta,
                                           const QStringList &providerOrder,
                                           const QStringList &enabledProviders)
{
    const TrackMetadata meta = prepareCandidateSearchMetadata(rawMeta);
    const QString contextKey = makeTrackContextKey(meta);

    // If search is currently running for this exact track context, don't restart it
    if (m_status == QLatin1String("searching") && m_currentContextKey == contextKey) {
        m_providerOrder = providerOrder;
        m_enabledProviders = enabledProviders;
        rebuildCandidatesList();
        return;
    }

    m_currentMeta = meta;
    m_providerOrder = providerOrder;
    m_enabledProviders = enabledProviders;
    m_currentContextKey = contextKey;
    m_isExplicitRefresh = false;
    startSearchInternal(/*isRefresh=*/false);
}

void CandidateSearchController::searchAgain()
{
    const quint64 searchId = m_searchId;
    m_isExplicitRefresh = true;
    m_refreshErrorMessage.clear();
    Q_EMIT refreshErrorMessageChanged();
    if (searchId == m_searchId) {
        startSearchInternal(/*isRefresh=*/true);
    }
}

void CandidateSearchController::clearRefreshErrorMessage()
{
    if (!m_refreshErrorMessage.isEmpty()) {
        m_refreshErrorMessage.clear();
        Q_EMIT refreshErrorMessageChanged();
    }
}

void CandidateSearchController::clearCache()
{
    m_trackCache.clear();
    m_providerRateLimitUntilMs.clear();
    m_totalCacheSizeBytes = 0;
    m_candidateStaleMap.clear();
    m_isUsingCachedResults = false;
    Q_EMIT isUsingCachedResultsChanged();
}

void CandidateSearchController::startSearchInternal(bool isRefresh)
{
    const quint64 searchId = m_searchId + 1;
    cancel();
    if (searchId != m_searchId) {
        return;
    }

    m_hadSuccessfulResponseInSearch = false;
    purgeExpiredCache();

    if (m_enabledProviders.isEmpty()) {
        m_status = QStringLiteral("all_disabled");
        m_candidates.clear();
        m_candidateStaleMap.clear();
        m_selectedIndex = -1;
        m_isUsingCachedResults = false;
        Q_EMIT isUsingCachedResultsChanged();
        Q_EMIT statusChanged();
        Q_EMIT candidatesChanged();
        Q_EMIT selectedIndexChanged();
        Q_EMIT hasUsableResultsChanged();
        return;
    }

    // Set up active providers in order
    m_activeProviders.clear();
    int priority = 0;
    for (const QString &id : m_providerOrder) {
        if (!m_enabledProviders.contains(id)) {
            continue;
        }
        for (LyricsProvider *p : m_allProviders) {
            if (p->id() == id && p->supportsCandidateSearch() && p->canHandle(m_currentMeta)) {
                ProviderEntry entry;
                entry.provider = p;
                entry.priority = priority++;
                m_activeProviders.append(entry);
                break;
            }
        }
    }

    for (LyricsProvider *p : m_allProviders) {
        if (!m_enabledProviders.contains(p->id()) || !p->supportsCandidateSearch() || !p->canHandle(m_currentMeta)) {
            continue;
        }
        bool exists = false;
        for (const auto &entry : m_activeProviders) {
            if (entry.provider == p) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            ProviderEntry entry;
            entry.provider = p;
            entry.priority = priority++;
            m_activeProviders.append(entry);
        }
    }

    if (m_activeProviders.isEmpty()) {
        m_status = QStringLiteral("all_disabled");
        m_candidates.clear();
        m_candidateStaleMap.clear();
        m_selectedIndex = -1;
        m_isUsingCachedResults = false;
        Q_EMIT isUsingCachedResultsChanged();
        Q_EMIT statusChanged();
        Q_EMIT candidatesChanged();
        Q_EMIT selectedIndexChanged();
        Q_EMIT hasUsableResultsChanged();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    TrackCandidateCache &cacheEntry = m_trackCache[m_currentContextKey];
    cacheEntry.trackContextKey = m_currentContextKey;
    cacheEntry.lastAccessedMs = now;

    if (!isRefresh) {
        // Initial search: check cache
        bool allCompleted = true;
        bool anyCached = false;

        for (auto &entry : m_activeProviders) {
            const QString pid = entry.provider->id();
            const bool isRateLimitedGlobal = (m_providerRateLimitUntilMs.value(pid, 0) > now);

            if (cacheEntry.providerResults.contains(pid)) {
                const CachedProviderResult &res = cacheEntry.providerResults.value(pid);
                const qint64 ageMs = now - res.timestampMs;

                // 1. Fresh hit (<5 min)
                const int ttlMs = res.candidates.isEmpty() ? kNegativeTtlMs : kFreshTtlMs;
                if (res.isValidResponse && ageMs < ttlMs) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = false;
                    entry.foundCandidates = res.candidates;
                    anyCached = true;
                    m_hadSuccessfulResponseInSearch = true;
                    continue;
                }

                // 2. 429 Cooldown check
                if (isRateLimitedGlobal || (res.isRateLimited && now < res.retryAfterUntilMs)) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                    anyCached = true;
                    continue;
                }

                if (res.isValidResponse && !res.candidates.isEmpty() && ageMs < kStaleTtlMs) {
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                    anyCached = true;
                }
            } else if (isRateLimitedGlobal) {
                entry.completed = true;
                continue;
            }

            allCompleted = false;
        }

        if (allCompleted) {
            m_isUsingCachedResults = anyCached;
            Q_EMIT isUsingCachedResultsChanged();
            if (searchId == m_searchId) {
                finishSearch();
            }
            return;
        }

        // Some or all providers need querying
        m_candidates.clear();
        m_selectedIndex = -1;
        m_status = QStringLiteral("searching");
        m_isUsingCachedResults = anyCached;
        rebuildCandidatesList();
        if (searchId != m_searchId) {
            return;
        }
        Q_EMIT isUsingCachedResultsChanged();
    } else {
        // Explicit refresh ("Search again"): retain previous successful m_candidates on screen!
        m_status = QStringLiteral("searching");

        for (auto &entry : m_activeProviders) {
            const QString pid = entry.provider->id();
            const bool isRateLimitedGlobal = (m_providerRateLimitUntilMs.value(pid, 0) > now);

            if (cacheEntry.providerResults.contains(pid)) {
                const CachedProviderResult &res = cacheEntry.providerResults.value(pid);
                if (res.isValidResponse && now - res.timestampMs < kStaleTtlMs) {
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                }
                if (isRateLimitedGlobal || (res.isRateLimited && now < res.retryAfterUntilMs)) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                    continue;
                }
            } else if (isRateLimitedGlobal) {
                entry.completed = true;
                entry.isCached = true;
                entry.isStale = true;
                entry.foundCandidates.clear();
                continue;
            }
            entry.completed = false;
            entry.inFlight = false;
        }

        rebuildCandidatesList();
    }

    if (searchId != m_searchId) {
        return;
    }
    Q_EMIT statusChanged();
    if (searchId == m_searchId && isSearching()) {
        m_deadlineTimer->start(kDeadlineMs);
        pumpQueue();
    }
}

void CandidateSearchController::pumpQueue()
{
    if (m_pumping) {
        return;
    }
    const QScopedValueRollback pumping(m_pumping, true);

    while (isSearching()) {
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
            continue;
        }
        if (inFlightCount >= m_maxConcurrent || pendingIndex == -1) {
            break;
        }

        ProviderEntry &entry = m_activeProviders[pendingIndex];
        entry.inFlight = true;
        entry.attemptToken = ++m_nextAttemptToken;
        if (m_providerTimeoutMs > 0) {
            QTimer::singleShot(m_providerTimeoutMs, this,
                               [this, sid = m_searchId, token = entry.attemptToken, id = entry.provider->id()] {
                onProviderTimeout(sid, token, id);
            });
        }
        entry.provider->searchCandidates(m_currentMeta, m_searchId);
    }
}

void CandidateSearchController::onProviderTimeout(quint64 searchId, quint64 token, const QString &providerId)
{
    if (searchId != m_searchId || m_status != QLatin1String("searching")) {
        return;
    }
    for (auto &entry : m_activeProviders) {
        if (!entry.provider || entry.provider->id() != providerId
            || !entry.inFlight || entry.attemptToken != token) {
            continue;
        }
        entry.inFlight = false;
        entry.completed = true;
        entry.failed = true;
        LyricsProvider *p = entry.provider;
        p->cancelCandidateSearch(searchId);
        // A local scheduling timeout is transient, not an API rate-limit or a
        // successful empty response. Preserve previous cached results unchanged.
        if (searchId == m_searchId && m_status == QLatin1String("searching")) {
            pumpQueue();
        }
        return;
    }
}

void CandidateSearchController::onCandidatesFound(quint64 searchId, const QList<LyricCandidate> &candidates, const QString &providerId)
{
    if (searchId != m_searchId || m_status != QLatin1String("searching")) {
        return;
    }

    bool found = false;
    for (auto &entry : m_activeProviders) {
        if (entry.provider && entry.provider->id() == providerId) {
            if (!entry.inFlight) {
                return;
            }
            entry.inFlight = false;
            entry.completed = true;
            entry.failed = false;
            entry.isCached = false;
            entry.isStale = false;
            entry.foundCandidates = candidates;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    m_hadSuccessfulResponseInSearch = true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Immediately cache results per provider
    TrackCandidateCache &cacheEntry = m_trackCache[m_currentContextKey];
    cacheEntry.trackContextKey = m_currentContextKey;
    cacheEntry.lastAccessedMs = now;

    CachedProviderResult &res = cacheEntry.providerResults[providerId];
    res.timestampMs = now;
    res.candidates = candidates;
    res.isValidResponse = true;
    res.isRateLimited = false;
    res.retryAfterUntilMs = 0;
    m_providerRateLimitUntilMs.remove(providerId);

    m_totalCacheSizeBytes -= cacheEntry.estimatedSizeBytes;
    cacheEntry.estimatedSizeBytes = 0;
    for (const auto &r : cacheEntry.providerResults) {
        cacheEntry.estimatedSizeBytes += estimateCandidateSizeBytes(r.candidates);
    }
    m_totalCacheSizeBytes += cacheEntry.estimatedSizeBytes;
    evictCacheIfNeeded();

    rebuildCandidatesList();
    if (searchId == m_searchId) {
        pumpQueue();
    }
}

void CandidateSearchController::onCandidateSearchFailed(quint64 searchId, MissingReason reason, const QString &providerId, int retryAfterSec)
{
    if (searchId != m_searchId || m_status != QLatin1String("searching")) {
        return;
    }

    bool found = false;
    for (auto &entry : m_activeProviders) {
        if (entry.provider && entry.provider->id() == providerId) {
            if (!entry.inFlight) {
                return;
            }
            entry.inFlight = false;
            entry.completed = true;
            entry.failed = true;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (reason == MissingReason::Unreachable && retryAfterSec > 0) {
        const qint64 cooldownMs = qint64(retryAfterSec) * 1000;
        TrackCandidateCache &cacheEntry = m_trackCache[m_currentContextKey];
        cacheEntry.trackContextKey = m_currentContextKey;
        cacheEntry.lastAccessedMs = now;

        CachedProviderResult &res = cacheEntry.providerResults[providerId];
        res.isRateLimited = true;
        res.retryAfterUntilMs = now + cooldownMs;
        m_providerRateLimitUntilMs[providerId] = now + cooldownMs;
    }

    pumpQueue();
}

void CandidateSearchController::rebuildCandidatesList()
{
    // Save current selection identity
    QString selProviderId;
    QString selCandidateId;
    if (m_selectedIndex >= 0 && m_selectedIndex < m_candidates.size()) {
        selProviderId = m_candidates.at(m_selectedIndex).providerId;
        selCandidateId = m_candidates.at(m_selectedIndex).id;
    }

    m_candidates.clear();
    m_candidateStaleMap.clear();
    const auto appendCandidates = [this](const ProviderEntry &entry) {
        m_candidates.append(entry.foundCandidates);
        if (entry.isStale) {
            for (const auto &candidate : entry.foundCandidates) {
                m_candidateStaleMap[candidate.providerId + QLatin1Char(':') + candidate.id] = true;
            }
        }
    };

    // Add candidates in priority order
    for (const QString &provId : m_providerOrder) {
        if (!m_enabledProviders.contains(provId)) {
            continue;
        }
        for (const auto &entry : m_activeProviders) {
            if (entry.provider && entry.provider->id() == provId) {
                appendCandidates(entry);
                break;
            }
        }
    }

    for (const auto &entry : m_activeProviders) {
        if (!entry.provider) continue;
        const QString pid = entry.provider->id();
        if (!m_enabledProviders.contains(pid) || m_providerOrder.contains(pid)) {
            continue;
        }
        appendCandidates(entry);
    }

    // Restore selection by (providerId, candidateId)
    int newIndex = -1;
    if (!selProviderId.isEmpty() && !selCandidateId.isEmpty()) {
        for (int i = 0; i < m_candidates.size(); ++i) {
            if (m_candidates.at(i).providerId == selProviderId && m_candidates.at(i).id == selCandidateId) {
                newIndex = i;
                break;
            }
        }
    }
    if (newIndex == -1 && !m_candidates.isEmpty()) {
        newIndex = 0;
    }
    setSelectedIndex(newIndex);
    Q_EMIT candidatesChanged();
    Q_EMIT hasUsableResultsChanged();
}

void CandidateSearchController::finishSearch()
{
    m_deadlineTimer->stop();
    m_retryAvailableAtMs = 0;
    const quint64 searchId = m_searchId;

    if (std::exchange(m_isExplicitRefresh, false)) {
        bool anyProviderSucceeded = false;
        for (const auto &entry : m_activeProviders) {
            if (entry.completed && !entry.failed && !entry.isCached) {
                anyProviderSucceeded = true;
                break;
            }
        }

        if (!anyProviderSucceeded) {
            if (!m_candidates.isEmpty()) {
                m_status = QStringLiteral("ready");
                m_isUsingCachedResults = true;
                m_refreshErrorMessage = tr("Refresh failed. Showing previous results.");
                Q_EMIT refreshErrorMessageChanged();
                if (searchId != m_searchId) {
                    return;
                }
                Q_EMIT isUsingCachedResultsChanged();
            } else {
                m_status = emptySearchStatus();
            }
        } else {
            rebuildCandidatesList();
            if (searchId != m_searchId) {
                return;
            }
            m_status = m_candidates.isEmpty() ? QStringLiteral("nomatches") : QStringLiteral("ready");
        }
    } else {
        rebuildCandidatesList();
        if (searchId != m_searchId) {
            return;
        }
        if (m_candidates.isEmpty()) {
            m_status = emptySearchStatus();
        } else {
            m_status = QStringLiteral("ready");
        }
    }

    if (searchId != m_searchId) {
        return;
    }
    Q_EMIT statusChanged();
    if (searchId == m_searchId) {
        Q_EMIT hasUsableResultsChanged();
    }
}

QString CandidateSearchController::emptySearchStatus()
{
    if (m_hadSuccessfulResponseInSearch) {
        return QStringLiteral("nomatches");
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 earliestRetry = 0;
    for (const auto &entry : m_activeProviders) {
        const qint64 retry = m_providerRateLimitUntilMs.value(entry.provider->id());
        if (retry <= now) {
            return QStringLiteral("no_response");
        }
        earliestRetry = earliestRetry == 0 ? retry : qMin(earliestRetry, retry);
    }
    if (earliestRetry > 0) {
        m_retryAvailableAtMs = earliestRetry;
        return QStringLiteral("rate_limited");
    }
    return QStringLiteral("no_response");
}

void CandidateSearchController::onDeadlineTimeout()
{
    if (m_status == QLatin1String("searching")) {
        const quint64 searchId = m_searchId;
        cancelSearchRequests(searchId);
        if (searchId == m_searchId && isSearching()) {
            finishSearch();
        }
    }
}

void CandidateSearchController::cancelSearchRequests(quint64 searchId)
{
    QList<LyricsProvider *> inFlight;
    for (auto &entry : m_activeProviders) {
        if (entry.inFlight && entry.provider) {
            entry.inFlight = false;
            entry.completed = true;
            entry.failed = true;
            inFlight.append(entry.provider);
        }
    }
    // Retire the whole batch before aborting, as abort can emit a result.
    for (LyricsProvider *source : std::as_const(inFlight)) {
        source->cancelCandidateSearch(searchId);
    }
}

void CandidateSearchController::useSelected()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= m_candidates.size()) {
        return;
    }

    const LyricCandidate cand = m_candidates.at(m_selectedIndex);
    LyricsProvider *targetProvider = provider(cand.providerId);

    if (!targetProvider) {
        m_status = QStringLiteral("error");
        m_errorMessage = QStringLiteral("Provider not found: %1").arg(cand.providerId);
        Q_EMIT statusChanged();
        Q_EMIT candidateLyricsFailed(m_errorMessage);
        return;
    }

    if (m_status == QLatin1String("searching")) {
        // The user chose an available result. Do not wait for slower sources,
        // and do not let their late callbacks disturb the candidate download.
        const quint64 oldSearchId = m_searchId++;
        m_deadlineTimer->stop();
        cancelSearchRequests(oldSearchId);
    }

    startFetch(targetProvider, cand.id, false);
}

bool CandidateSearchController::restoreMatch(const QString &providerId, const QString &candidateId)
{
    LyricsProvider *source = provider(providerId);
    if (m_status != QLatin1String("idle") || !source || !source->supportsCandidateSearch() || candidateId.isEmpty()) {
        return false;
    }
    startFetch(source, candidateId, true);
    return true;
}

void CandidateSearchController::startFetch(LyricsProvider *source, const QString &candidateId, bool restoring)
{
    cancelFetch();
    const quint64 fetchId = m_fetchId;
    m_loadingProviderId = source->id();
    m_loadingCandidateId = candidateId;
    m_status = restoring ? QStringLiteral("restoring") : QStringLiteral("loading");
    m_errorMessage.clear();
    m_fetchDeadlineTimer->start(restoring ? kRestoreDeadlineMs : kFetchDeadlineMs);
    Q_EMIT statusChanged();
    if (m_fetchId == fetchId) {
        source->fetchCandidateLyrics(candidateId, fetchId);
    }
}

void CandidateSearchController::onCandidateLyricsLoaded(quint64 fetchId, const Lyrics &lyrics, const QString &providerId)
{
    if (fetchId != m_fetchId || providerId != m_loadingProviderId) {
        return;
    }
    finishFetch(lyrics, {});
}

void CandidateSearchController::onCandidateLyricsFailed(quint64 fetchId, MissingReason reason, const QString &providerId)
{
    if (fetchId != m_fetchId || providerId != m_loadingProviderId) {
        return;
    }

    QString message;
    if (reason == MissingReason::SizeLimitExceeded) {
        message = tr("Selected lyrics exceed the size limit.");
    } else if (reason == MissingReason::Unsynced) {
        message = tr("Selected lyrics have no synchronization.");
    } else {
        message = tr("Failed to download selected lyrics.");
    }
    finishFetch({}, message);
}

void CandidateSearchController::finishFetch(const Lyrics &lyrics, const QString &errorMessage)
{
    const quint64 fetchId = m_fetchId;
    const bool restoring = isRestoringMatch();
    const QString sourceId = std::exchange(m_loadingProviderId, {});
    const QString candidateId = std::exchange(m_loadingCandidateId, {});
    m_fetchDeadlineTimer->stop();
    m_errorMessage = errorMessage;
    m_status = restoring ? QStringLiteral("idle") : (errorMessage.isEmpty() ? QStringLiteral("ready") : QStringLiteral("error"));
    Q_EMIT statusChanged();
    if (fetchId != m_fetchId) {
        return;
    }
    if (restoring) {
        Q_EMIT rememberedMatchFinished(lyrics, sourceId, candidateId);
    } else if (errorMessage.isEmpty()) {
        Q_EMIT candidateLyricsLoaded(lyrics, sourceId, candidateId);
    } else {
        Q_EMIT candidateLyricsFailed(errorMessage);
    }
}

void CandidateSearchController::onFetchDeadlineTimeout()
{
    if (!m_loadingProviderId.isEmpty()) {
        const quint64 requestId = m_fetchId++;
        if (LyricsProvider *source = provider(m_loadingProviderId)) {
            source->cancelCandidateFetch(requestId);
        }
        finishFetch({}, tr("Timed out fetching selected lyrics."));
    }
}

void CandidateSearchController::cancelFetch()
{
    const quint64 requestId = m_fetchId++;
    m_fetchDeadlineTimer->stop();
    if (!m_loadingProviderId.isEmpty()) {
        if (LyricsProvider *source = provider(m_loadingProviderId)) {
            source->cancelCandidateFetch(requestId);
        }
        m_loadingProviderId.clear();
        m_loadingCandidateId.clear();
    }
}

void CandidateSearchController::cancel()
{
    m_deadlineTimer->stop();
    const quint64 searchId = m_searchId++;
    m_retryAvailableAtMs = 0;
    m_status = QStringLiteral("idle");
    cancelSearchRequests(searchId);
    cancelFetch();
    Q_EMIT statusChanged();
}

void CandidateSearchController::close()
{
    // Abort in-flight searches, but keep completed provider results in cache!
    cancel();
}
