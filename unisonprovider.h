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

class UnisonProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit UnisonProvider(QObject *parent = nullptr);
    ~UnisonProvider() override;

    QString id() const override { return QStringLiteral("unison"); }
    QString displayName() const override { return QStringLiteral("Unison (boidu.dev)"); }
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
        DirectLookup,
        Search,
        FetchDetails
    };

    void startDirectLookup();
    void startSearch();
    void startFetchDetails(const QString &lyricsId);

    void handleDirectReply(const QByteArray &data, int httpCode);
    void handleSearchReply(const QByteArray &data);
    void handleDetailsReply(const QByteArray &data);

    bool parsePayload(const QJsonObject &dataObj, Lyrics *outLyrics);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;
    Step m_step = Step::Idle;

    TrackMetadata m_meta;
};
