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

class NeteaseProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit NeteaseProvider(QObject *parent = nullptr);
    ~NeteaseProvider() override;

    QString id() const override { return QStringLiteral("netease"); }
    QString displayName() const override { return QStringLiteral("NetEase"); }
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
        FetchLyrics
    };

    void startSearch();
    void startFetchLyrics(const QString &songId);

    void handleSearchReply(const QByteArray &data);
    void handleLyricsReply(const QByteArray &data);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    Step m_step = Step::Idle;

    TrackMetadata m_meta;
    QString m_songId;
};
