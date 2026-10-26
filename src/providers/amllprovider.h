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
                          const QString &cdnBase = QStringLiteral("https://cdn.jsdelivr.net/gh/amll-dev/amll-ttml-db@main/spotify-lyrics/"),
                          const QString &fallbackBase = QStringLiteral("https://raw.githubusercontent.com/amll-dev/amll-ttml-db/main/spotify-lyrics/"));
    ~AmllProvider() override;

    QString id() const override { return QStringLiteral("amll"); }
    QString displayName() const override { return QStringLiteral("AMLL TTML DB"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::RequiresSpotifyId;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;
    void invalidateCache(const TrackMetadata &meta) override;

    QString confirmedRejectionReason(quint64 generation) const override
    {
        return (generation == m_diagGeneration) ? m_diagReason : QString();
    }
    QString cacheUsage(quint64 generation) const override
    {
        return (generation == m_diagGeneration) ? m_diagCache : QString();
    }

    void setFallbackBase(const QString &fallbackBase) { m_fallbackBase = fallbackBase; }
    QString fallbackBase() const { return m_fallbackBase; }

    static QString cacheDirectory();

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    void executeFetch(const QString &trackId, bool isFallback);
    bool checkDiskCache(const QString &trackId, qint64 durationMs, Lyrics *outLyrics);
    void writeDiskCache(const QString &trackId, const QByteArray &data);

    QString m_cdnBase;
    QString m_fallbackBase;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    QString m_pendingTrackId;
    qint64 m_pendingDurationMs = 0;
    QHash<QString, qint64> m_negativeCache; // trackId -> timestamp
    quint64 m_diagGeneration = 0;
    QString m_diagReason;
    QString m_diagCache;
};
