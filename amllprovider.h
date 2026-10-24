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
                          const QString &cdnBase = QStringLiteral("https://cdn.jsdelivr.net/gh/amll-dev/amll-ttml-db@main/spotify-lyrics/"));
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

    static QString cacheDirectory();

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    void executeFetch(const QString &trackId, bool isFallback);
    bool checkDiskCache(const QString &trackId, qint64 durationMs, Lyrics *outLyrics);
    void writeDiskCache(const QString &trackId, const QByteArray &data);

    QString m_cdnBase;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    QString m_pendingTrackId;
    qint64 m_pendingDurationMs = 0;
    QHash<QString, qint64> m_negativeCache; // trackId -> timestamp
};
