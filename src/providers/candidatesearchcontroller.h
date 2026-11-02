/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"
#include "lyrictypes.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class CandidateSearchController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList candidates READ candidatesList NOTIFY candidatesChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY statusChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(bool searching READ isSearching NOTIFY statusChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY statusChanged)
    Q_PROPERTY(bool hasUsableResults READ hasUsableResults NOTIFY hasUsableResultsChanged)
    Q_PROPERTY(bool isUsingCachedResults READ isUsingCachedResults NOTIFY isUsingCachedResultsChanged)
    Q_PROPERTY(QString refreshErrorMessage READ refreshErrorMessage NOTIFY refreshErrorMessageChanged)

public:
    explicit CandidateSearchController(QObject *parent = nullptr);
    ~CandidateSearchController() override;

    QString status() const { return m_status; }
    QVariantList candidatesList() const;
    QString errorMessage() const { return m_errorMessage; }
    int selectedIndex() const { return m_selectedIndex; }
    void setSelectedIndex(int index);

    bool isSearching() const { return m_status == QLatin1String("searching"); }
    bool isLoading() const { return m_status == QLatin1String("loading"); }
    bool hasUsableResults() const { return !m_candidates.isEmpty(); }
    bool isUsingCachedResults() const { return m_isUsingCachedResults; }
    QString refreshErrorMessage() const { return m_refreshErrorMessage; }

    Q_INVOKABLE void startSearch(const TrackMetadata &meta,
                                 const QStringList &providerOrder,
                                 const QStringList &enabledProviders);
    Q_INVOKABLE void searchAgain();
    Q_INVOKABLE void useSelected();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();
    Q_INVOKABLE void clearRefreshErrorMessage();
    Q_INVOKABLE void clearCache();

    void registerProvider(LyricsProvider *provider);
    LyricsProvider *provider(const QString &id) const;

    QList<LyricCandidate> rawCandidates() const { return m_candidates; }

    int providerTimeoutMs() const { return m_providerTimeoutMs; }
    void setProviderTimeoutMs(int timeoutMs) { m_providerTimeoutMs = qMax(0, timeoutMs); }

    static constexpr int kFreshTtlMs = 5 * 60 * 1000;    // 5 minutes
    static constexpr int kStaleTtlMs = 30 * 60 * 1000;   // 30 minutes
    static constexpr int kNegativeTtlMs = 60 * 1000;     // 60 seconds
    static constexpr int kDefaultRateLimitCooldownMs = 60 * 1000; // 60 seconds
    static constexpr int kMaxTrackContexts = 20;
    static constexpr qint64 kMaxCacheSizeBytes = 16 * 1024 * 1024; // 16 MiB

Q_SIGNALS:
    void statusChanged();
    void candidatesChanged();
    void selectedIndexChanged();
    void hasUsableResultsChanged();
    void isUsingCachedResultsChanged();
    void refreshErrorMessageChanged();
    void candidateLyricsLoaded(const Lyrics &lyrics, const QString &providerId);
    void candidateLyricsFailed(const QString &errorMessage);

private Q_SLOTS:
    void onCandidatesFound(quint64 searchId, const QList<LyricCandidate> &candidates, const QString &providerId);
    void onCandidateSearchFailed(quint64 searchId, MissingReason reason, const QString &providerId, int retryAfterSec = 0);
    void onCandidateLyricsLoaded(quint64 fetchId, const Lyrics &lyrics, const QString &providerId);
    void onCandidateLyricsFailed(quint64 fetchId, MissingReason reason, const QString &providerId);
    void onDeadlineTimeout();
    void onFetchDeadlineTimeout();

private:
    struct CachedProviderResult {
        qint64 timestampMs = 0;
        qint64 retryAfterUntilMs = 0;
        QList<LyricCandidate> candidates;
        bool isValidResponse = false;
        bool isRateLimited = false;
    };

    struct TrackCandidateCache {
        QString trackContextKey;
        qint64 lastAccessedMs = 0;
        QHash<QString, CachedProviderResult> providerResults;
        qint64 estimatedSizeBytes = 0;
    };

    struct ProviderEntry {
        LyricsProvider *provider = nullptr;
        int priority = 0;
        bool inFlight = false;
        bool completed = false;
        bool failed = false;
        bool isCached = false;
        bool isStale = false;
        quint64 attemptToken = 0;
        QList<LyricCandidate> foundCandidates;
    };

    void startSearchInternal(bool isRefresh);
    void pumpQueue();
    void onProviderTimeout(quint64 searchId, quint64 token, const QString &providerId);
    void finishSearch();
    void rebuildCandidatesList();
    QString formatDuration(qint64 ms) const;
    QString formatSyncType(CandidateSyncType type) const;

    static QString makeTrackContextKey(const TrackMetadata &meta);
    static qint64 estimateCandidateSizeBytes(const QList<LyricCandidate> &candidates);
    void evictCacheIfNeeded();
    void purgeExpiredCache();

    QList<LyricsProvider *> m_allProviders;
    QList<ProviderEntry> m_activeProviders;
    QList<LyricCandidate> m_candidates;
    QHash<QString, bool> m_candidateStaleMap; // providerId:candId -> isStale

    QHash<QString, TrackCandidateCache> m_trackCache;
    QHash<QString, qint64> m_providerRateLimitUntilMs;
    qint64 m_totalCacheSizeBytes = 0;
    QString m_currentContextKey;
    bool m_isUsingCachedResults = false;
    QString m_refreshErrorMessage;
    bool m_isExplicitRefresh = false;
    bool m_hadSuccessfulResponseInSearch = false;

    TrackMetadata m_currentMeta;
    QStringList m_providerOrder;
    QStringList m_enabledProviders;

    quint64 m_searchId = 0;
    quint64 m_fetchId = 0;
    QString m_loadingProviderId;
    QString m_status = QStringLiteral("idle");
    QString m_errorMessage;
    int m_selectedIndex = -1;

    QTimer *m_deadlineTimer = nullptr;
    QTimer *m_fetchDeadlineTimer = nullptr;
    int m_maxConcurrent = 2;
    int m_providerTimeoutMs = 4000;
    quint64 m_nextAttemptToken = 0;
    static constexpr int kDeadlineMs = 20000;
    static constexpr int kFetchDeadlineMs = 15000;

    friend class LrcAppletSyncTest;
};
