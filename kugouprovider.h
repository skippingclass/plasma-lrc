/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

class KugouProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit KugouProvider(QObject *parent = nullptr);
    ~KugouProvider() override;

    QString id() const override { return QStringLiteral("kugou"); }
    QString displayName() const override { return QStringLiteral("KuGou"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync | Capability::SearchByTitleArtist;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    enum class Step {
        Idle,
        SearchSong,
        SearchCandidates,
        DownloadKrc,
        DownloadLrcFallback
    };

    void startSongSearch();
    void startCandidateSearch(const QString &hash, qint64 songDurationSec);
    void startDownload(const QString &id, const QString &accesskey, bool isKrc);

    void handleSearchSongReply(const QByteArray &data);
    void handleCandidatesReply(const QByteArray &data);
    void handleDownloadReply(const QByteArray &data, bool isKrc);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    Step m_step = Step::Idle;

    TrackMetadata m_meta;
    QString m_currentId;
    QString m_currentAccessKey;
};
