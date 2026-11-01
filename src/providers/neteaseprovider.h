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
    explicit NeteaseProvider(QObject *parent = nullptr,
                             const QString &apiBase = QStringLiteral("https://music.163.com"));
    ~NeteaseProvider() override;

    void setApiBase(const QString &base) { m_apiBase = base; }
    QString apiBase() const { return m_apiBase; }

    QString id() const override { return QStringLiteral("netease"); }
    QString displayName() const override { return QStringLiteral("NetEase"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync | Capability::SearchByTitleArtist;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;

    bool supportsCandidateSearch() const override { return true; }
    void searchCandidates(const TrackMetadata &meta, quint64 searchId) override;
    void fetchCandidateLyrics(const QString &candidateId, quint64 fetchId) override;
    void cancelCandidateSearch(quint64 searchId) override;
    void cancelCandidateFetch(quint64 fetchId) override;

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
    QString m_apiBase;

    QPointer<QNetworkReply> m_candidateSearchReply;
    QPointer<QNetworkReply> m_candidateFetchReply;
    quint64 m_candidateSearchId = 0;
    quint64 m_candidateFetchId = 0;
};
