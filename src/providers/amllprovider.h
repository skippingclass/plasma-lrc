/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

class AmllProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit AmllProvider(QObject *parent = nullptr,
                          const QString &apiBase = QStringLiteral("https://api.amll.dev"));
    ~AmllProvider() override;

    QString id() const override { return QStringLiteral("amll"); }
    QString displayName() const override { return QStringLiteral("AMLL TTML DB"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;
    void invalidateCache(const TrackMetadata &meta) override;
    void clearAllCaches() override;

    QString confirmedRejectionReason(quint64 generation) const override
    {
        return (generation == m_diagGeneration) ? m_diagReason : QString();
    }
    QString cacheUsage(quint64 generation) const override
    {
        return (generation == m_diagGeneration) ? m_diagCache : QString();
    }

    void setApiBase(const QString &apiBase) { m_apiBase = apiBase; }
    QString apiBase() const { return m_apiBase; }

    static QString cacheDirectory();
    static QString makeCacheKey(const TrackMetadata &meta);

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    enum class Step {
        Idle,
        GetBySpotifyId,
        SearchByTitleArtist,
        GetById
    };

    void startGetBySpotifyId();
    void startSearchByTitleArtist();
    void startGetById(qint64 idVal, const QString &filename);
    void handlePayload(const QByteArray &data, quint64 gen);
    void failCurrent(MissingReason reason, const QString &diagReason, bool is404 = false);

    bool checkDiskCache(const QString &cacheKey, qint64 durationMs, Lyrics *outLyrics);
    void writeDiskCache(const QString &cacheKey, const QByteArray &data);

    QString m_apiBase;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    TrackMetadata m_currentMeta;
    QString m_currentCacheKey;
    Step m_currentStep = Step::Idle;
    QHash<QString, qint64> m_negativeCache; // cacheKey -> timestamp
    quint64 m_diagGeneration = 0;
    QString m_diagReason;
    QString m_diagCache;
};
