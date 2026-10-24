/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QList>
#include <QObject>
#include <QStringList>

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

    QString currentProviderId() const { return m_currentProviderId; }

Q_SIGNALS:
    void lyricsResolved(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void allFailed(quint64 generation, MissingReason lastReason);

private Q_SLOTS:
    void onProviderLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void onProviderFailed(quint64 generation, MissingReason reason, const QString &providerId);

private:
    void tryNextCandidate();

    QList<LyricsProvider *> m_providers;
    QStringList m_providerOrder;

    quint64 m_currentGeneration = 0;
    TrackMetadata m_currentMeta;
    QList<LyricsProvider *> m_activeCandidates;
    int m_candidateIndex = -1;
    QString m_currentProviderId;
    MissingReason m_lastReason = MissingReason::NotFound;
};
