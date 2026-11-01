/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QVector>

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

    static QString cacheDirectory();
    static QString makeCacheKey(const QString &title, const QString &artist, qint64 durationMs, const QString &isrc);

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    struct CandidateItem {
        QString ttmlUrl;
        QString lrcUrl;
        QString isrc;
        int score = 0;
        double durationDiff = 999999.0;
    };

    void startSearch(const QString &query);
    void startFileDownload(const QUrl &url, bool isTtml, const QString &fallbackIsrc = QString());
    void tryNextCandidateOrFormat();
    QUrl resolveCandidateUrl(const QString &rawUrl) const;
    void handleSearchReply(const QByteArray &data);
    void handleFileReply(const QByteArray &data, bool isTtml);
    bool checkDiskCache(const QString &cacheKey, qint64 durationMs, Lyrics *outLyrics);
    void writeDiskCache(const QString &cacheKey, const QByteArray &data, bool isTtml);

    QString m_apiBase;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    QDeadlineTimer m_requestDeadline;
    quint64 m_currentGeneration = 0;
    QString m_currentCacheKey;
    TrackMetadata m_currentMeta;
    qint64 m_targetDurationMs = 0;
    QVector<CandidateItem> m_candidateQueue;
    int m_currentCandidateIdx = 0;
    bool m_currentCandidateTriedTtml = false;
    bool m_currentCandidateTriedLrc = false;
    bool m_hadTransientError = false;
    bool m_hadUnsyncedCandidate = false;
    bool m_hadTruncatedCandidates = false;
    QHash<QString, qint64> m_negativeCache;
    quint64 m_diagGeneration = 0;
    QString m_diagReason;
    QString m_diagCache;
};
