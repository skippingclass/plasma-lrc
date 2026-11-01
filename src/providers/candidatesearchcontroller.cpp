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
    m_isExplicitRefresh = true;
    m_refreshErrorMessage.clear();
    Q_EMIT refreshErrorMessageChanged();
    startSearchInternal(/*isRefresh=*/true);
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
    cancel();

    ++m_searchId;
    m_hadSuccessfulResponseInSearch = false;
    purgeExpiredCache();

    if (m_enabledProviders.isEmpty()) {
        m_status = QStringLiteral("all_disabled");
        m_candidates.clear();
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
                if (res.isValidResponse && ageMs < kFreshTtlMs) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = false;
                    entry.foundCandidates = res.candidates;
                    anyCached = true;
                    for (const auto &c : res.candidates) {
                        m_candidateStaleMap[c.providerId + QLatin1Char(':') + c.id] = false;
                    }
                    continue;
                }

                // 2. 429 Cooldown check
                if (isRateLimitedGlobal || (res.isRateLimited && now < res.retryAfterUntilMs)) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                    anyCached = true;
                    for (const auto &c : res.candidates) {
                        m_candidateStaleMap[c.providerId + QLatin1Char(':') + c.id] = true;
                    }
                    continue;
                }

                // 3. Negative cache: valid empty response within 60s
                if (res.isValidResponse && res.candidates.isEmpty() && ageMs < kNegativeTtlMs) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.foundCandidates.clear();
                    anyCached = true;
                    continue;
                }
            } else if (isRateLimitedGlobal) {
                entry.completed = true;
                entry.isCached = true;
                entry.isStale = true;
                entry.foundCandidates.clear();
                anyCached = true;
                continue;
            }

            allCompleted = false;
        }

        if (allCompleted) {
            m_isUsingCachedResults = anyCached;
            rebuildCandidatesList();
            m_status = m_candidates.isEmpty() ? QStringLiteral("nomatches") : QStringLiteral("ready");
            Q_EMIT isUsingCachedResultsChanged();
            Q_EMIT statusChanged();
            return;
        }

        // Some or all providers need querying
        m_candidates.clear();
        m_selectedIndex = -1;
        rebuildCandidatesList();
        m_status = QStringLiteral("searching");
        m_isUsingCachedResults = anyCached;
        Q_EMIT isUsingCachedResultsChanged();
        Q_EMIT statusChanged();

        m_deadlineTimer->start(kDeadlineMs);
        pumpQueue();
    } else {
        // Explicit refresh ("Search again"): retain previous successful m_candidates on screen!
        m_status = QStringLiteral("searching");
        Q_EMIT statusChanged();

        for (auto &entry : m_activeProviders) {
            const QString pid = entry.provider->id();
            const bool isRateLimitedGlobal = (m_providerRateLimitUntilMs.value(pid, 0) > now);

            if (cacheEntry.providerResults.contains(pid)) {
                const CachedProviderResult &res = cacheEntry.providerResults.value(pid);
                if (isRateLimitedGlobal || (res.isRateLimited && now < res.retryAfterUntilMs)) {
                    entry.completed = true;
                    entry.isCached = true;
                    entry.isStale = true;
                    entry.foundCandidates = res.candidates;
                    for (const auto &c : res.candidates) {
                        m_candidateStaleMap[c.providerId + QLatin1Char(':') + c.id] = true;
                    }
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
            entry.foundCandidates.clear();
        }

        m_deadlineTimer->start(kDeadlineMs);
        pumpQueue();
    }
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

    m_hadSuccessfulResponseInSearch = true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (auto &entry : m_activeProviders) {
        if (entry.provider && entry.provider->id() == providerId) {
            entry.inFlight = false;
            entry.completed = true;
            entry.failed = false;
            entry.foundCandidates = candidates;
            break;
        }
    }

    for (const auto &c : candidates) {
        m_candidateStaleMap[c.providerId + QLatin1Char(':') + c.id] = false;
    }

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

    cacheEntry.estimatedSizeBytes = 0;
    for (const auto &r : cacheEntry.providerResults) {
        cacheEntry.estimatedSizeBytes += estimateCandidateSizeBytes(r.candidates);
    }
    evictCacheIfNeeded();

    rebuildCandidatesList();

    bool allCompleted = true;
    for (const auto &entry : m_activeProviders) {
        if (!entry.completed) {
            allCompleted = false;
            break;
        }
    }

    if (allCompleted) {
        finishSearch();
    } else {
        pumpQueue();
    }
}

void CandidateSearchController::onCandidateSearchFailed(quint64 searchId, MissingReason reason, const QString &providerId, int retryAfterSec)
{
    if (searchId != m_searchId || m_status != QLatin1String("searching")) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto &entry : m_activeProviders) {
        if (entry.provider && entry.provider->id() == providerId) {
            entry.inFlight = false;
            entry.completed = true;
            entry.failed = true;
            break;
        }
    }

    if (reason == MissingReason::Unreachable) {
        const int cooldownMs = (retryAfterSec > 0 ? retryAfterSec * 1000 : kDefaultRateLimitCooldownMs);
        TrackCandidateCache &cacheEntry = m_trackCache[m_currentContextKey];
        cacheEntry.trackContextKey = m_currentContextKey;
        cacheEntry.lastAccessedMs = now;

        CachedProviderResult &res = cacheEntry.providerResults[providerId];
        res.isRateLimited = true;
        res.retryAfterUntilMs = now + cooldownMs;
        m_providerRateLimitUntilMs[providerId] = now + cooldownMs;
    }

    bool allCompleted = true;
    for (const auto &entry : m_activeProviders) {
        if (!entry.completed) {
            allCompleted = false;
            break;
        }
    }

    if (allCompleted) {
        finishSearch();
    } else {
        pumpQueue();
    }
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

    // Add candidates in priority order
    for (const QString &provId : m_providerOrder) {
        if (!m_enabledProviders.contains(provId)) {
            continue;
        }
        for (const auto &entry : m_activeProviders) {
            if (entry.provider && entry.provider->id() == provId) {
                m_candidates.append(entry.foundCandidates);
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
        m_candidates.append(entry.foundCandidates);
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

    if (m_isExplicitRefresh) {
        bool anyProviderSucceeded = false;
        for (const auto &entry : m_activeProviders) {
            if (!entry.failed) {
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
                Q_EMIT isUsingCachedResultsChanged();
            } else {
                m_status = QStringLiteral("no_response");
            }
        } else {
            rebuildCandidatesList();
            m_status = m_candidates.isEmpty() ? QStringLiteral("nomatches") : QStringLiteral("ready");
        }
        m_isExplicitRefresh = false;
    } else {
        rebuildCandidatesList();
        if (m_candidates.isEmpty()) {
            m_status = m_hadSuccessfulResponseInSearch ? QStringLiteral("nomatches") : QStringLiteral("no_response");
        } else {
            m_status = QStringLiteral("ready");
        }
    }

    Q_EMIT statusChanged();
    Q_EMIT hasUsableResultsChanged();
}

void CandidateSearchController::onDeadlineTimeout()
{
    if (m_status == QLatin1String("searching")) {
        for (auto &entry : m_activeProviders) {
            if (entry.inFlight && entry.provider) {
                entry.provider->cancelCandidateSearch(m_searchId);
                entry.inFlight = false;
                entry.completed = true;
                entry.failed = true;
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
    // Abort in-flight searches, but keep completed provider results in cache!
    cancel();
}
