/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

class LrcRedProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit LrcRedProvider(QObject *parent = nullptr,
                            const QString &apiBase = QStringLiteral("https://lrc.red/"));
    ~LrcRedProvider() override;

    QString id() const override { return QStringLiteral("lrcred"); }
    QString displayName() const override { return QStringLiteral("lrc.red"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync | Capability::SearchByTitleArtist | Capability::RequiresIsrc;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;

    static QString cacheDirectory();
    static QString makeCacheKey(const QString &title, const QString &artist, qint64 durationMs, const QString &isrc);

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    void startSearch(const QString &query);
    void startFileDownload(const QUrl &url, bool isTtml, const QString &fallbackIsrc = QString());
    void handleSearchReply(const QByteArray &data);
    void handleFileReply(const QByteArray &data, bool isTtml);
    bool checkDiskCache(const QString &cacheKey, qint64 durationMs, Lyrics *outLyrics);
    void writeDiskCache(const QString &cacheKey, const QByteArray &data, bool isTtml);

    QString m_apiBase;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    QString m_currentCacheKey;
    qint64 m_targetDurationMs = 0;
    QHash<QString, qint64> m_negativeCache;
};
