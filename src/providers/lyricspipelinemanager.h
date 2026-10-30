/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

class QTimer;

class LyricsPipelineManager : public QObject
{
    Q_OBJECT

public:
    explicit LyricsPipelineManager(QObject *parent = nullptr);
    ~LyricsPipelineManager() override;

    void registerProvider(LyricsProvider *provider);
    LyricsProvider *provider(const QString &id) const;
    QList<LyricsProvider *> allProviders() const { return m_providers; }

    QStringList providerOrder() const { return m_providerOrder; }
    void setProviderOrder(const QStringList &order);

    bool isProviderEnabled(const QString &id) const;
    void setProviderEnabled(const QString &id, bool enabled);

    void requestLyrics(const TrackMetadata &meta, quint64 generation);
    void cancel();

    QString currentProviderId() const;

    void invalidateCaches(const TrackMetadata &meta);
    void clearAllCaches();

    int lookupTimeoutMs() const { return m_lookupTimeoutMs; }
    void setLookupTimeoutMs(int timeoutMs) { m_lookupTimeoutMs = timeoutMs; }

    int maxConcurrentLookups() const { return m_maxConcurrentLookups; }
    void setMaxConcurrentLookups(int max) { m_maxConcurrentLookups = max; }

    struct ProviderDiagnostic {
        QString id;
        QString displayName;
        QString status;          // "disabled", "skipped", "pending", "searching", "provisional", "selected", "failed", "cancelled"
        QString rejectionReason; // Confirmed reason e.g. "Candidate duration mismatch (>4s)", "Unsynced lyrics", etc.
        QString cacheUsage;      // "Hit (disk)", "Hit (negative cache)", "Miss", "None"
        QString syncQuality;     // "Syllable", "Line", "Unsynced", "None"
        int priority = -1;
    };

    QList<ProviderDiagnostic> providerDiagnostics() const;
    TrackMetadata currentMeta() const { return m_currentMeta; }
    quint64 currentGeneration() const { return m_currentGeneration; }
    bool isLookupActive() const { return m_isLookupActive; }
    QString bestProviderId() const { return m_bestProviderId; }
    LyricsType bestQuality() const { return m_bestQuality; }

Q_SIGNALS:
    void lyricsResolved(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void allFailed(quint64 generation, MissingReason lastReason);
    void lookupFinished(quint64 generation);

private Q_SLOTS:
    void onProviderLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void onProviderFailed(quint64 generation, MissingReason reason, const QString &providerId);
    void onLookupDeadline();

private:
    struct CandidateEntry {
        LyricsProvider *provider = nullptr;
        int priority = -1;
        bool inFlight = false;
        bool completed = false;
    };

    void pumpQueue();
    void pruneIrrelevantCandidates();
    void finishLookup();

    QList<LyricsProvider *> m_providers;
    QStringList m_providerOrder;

    quint64 m_currentGeneration = 0;
    TrackMetadata m_currentMeta;
    QList<CandidateEntry> m_candidates;
    QTimer *m_deadlineTimer = nullptr;

    int m_lookupTimeoutMs = 20000;
    int m_maxConcurrentLookups = 2;
    MissingReason m_lastReason = MissingReason::NotFound;

    bool m_hasEmittedResult = false;
    LyricsType m_bestQuality = LyricsType::None;
    int m_bestPriority = -1;
    QString m_bestProviderId;

    bool m_isLookupActive = false;
    bool m_pumping = false;
    QMap<QString, ProviderDiagnostic> m_providerDiagnostics;
};
