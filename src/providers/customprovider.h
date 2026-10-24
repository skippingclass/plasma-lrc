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

class CustomProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit CustomProvider(QObject *parent = nullptr);
    ~CustomProvider() override;

    QString id() const override { return QStringLiteral("custom"); }
    QString displayName() const override { return QStringLiteral("Custom URL"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync | Capability::SearchByTitleArtist;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;

    QString urlTemplate() const { return m_urlTemplate; }
    void setUrlTemplate(const QString &tmpl) { m_urlTemplate = tmpl; }

    QString headers() const { return m_headers; }
    void setHeaders(const QString &h) { m_headers = h; }

private Q_SLOTS:
    void onReplyFinished();
    void onTimeout();

private:
    QString buildUrl(const TrackMetadata &meta) const;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_currentReply;
    QTimer m_watchdog;
    quint64 m_currentGeneration = 0;

    QString m_urlTemplate;
    QString m_headers;
    TrackMetadata m_meta;
};
