/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"
#include "lyrictypes.h"

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

    Q_INVOKABLE void startSearch(const TrackMetadata &meta,
                                 const QStringList &providerOrder,
                                 const QStringList &enabledProviders);
    Q_INVOKABLE void searchAgain();
    Q_INVOKABLE void useSelected();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void close();

    void registerProvider(LyricsProvider *provider);
    LyricsProvider *provider(const QString &id) const;

    QList<LyricCandidate> rawCandidates() const { return m_candidates; }

Q_SIGNALS:
    void statusChanged();
    void candidatesChanged();
    void selectedIndexChanged();
    void candidateLyricsLoaded(const Lyrics &lyrics, const QString &providerId);
    void candidateLyricsFailed(const QString &errorMessage);

private Q_SLOTS:
    void onCandidatesFound(quint64 searchId, const QList<LyricCandidate> &candidates, const QString &providerId);
    void onCandidateLyricsLoaded(quint64 fetchId, const Lyrics &lyrics, const QString &providerId);
    void onCandidateLyricsFailed(quint64 fetchId, MissingReason reason, const QString &providerId);
    void onDeadlineTimeout();
    void onFetchDeadlineTimeout();

private:
    struct ProviderEntry {
        LyricsProvider *provider = nullptr;
        int priority = 0;
        bool inFlight = false;
        bool completed = false;
        QList<LyricCandidate> foundCandidates;
    };

    void pumpQueue();
    void finishSearch();
    void rebuildCandidatesList();
    QString formatDuration(qint64 ms) const;
    QString formatSyncType(CandidateSyncType type) const;

    QList<LyricsProvider *> m_allProviders;
    QList<ProviderEntry> m_activeProviders;
    QList<LyricCandidate> m_candidates;

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
    static constexpr int kDeadlineMs = 20000;
    static constexpr int kFetchDeadlineMs = 15000;

    friend class LrcAppletSyncTest;
};
