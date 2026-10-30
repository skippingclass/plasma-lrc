/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"
#include "lrclib.h"

class LrclibProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit LrclibProvider(QObject *parent = nullptr);
    ~LrclibProvider() override = default;

    QString id() const override { return QStringLiteral("lrclib"); }
    QString displayName() const override { return QStringLiteral("LRCLIB"); }
    Capabilities capabilities() const override
    {
        return Capability::LineSync | Capability::SearchByTitleArtist;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;
    void invalidateCache(const TrackMetadata &meta) override;
    void clearAllCaches() override;

    QString confirmedRejectionReason(quint64 generation) const override
    {
        return (generation == m_currentGeneration && m_client) ? m_client->lastRejectionReason() : QString();
    }
    QString cacheUsage(quint64 generation) const override
    {
        return (generation == m_currentGeneration && m_client) ? m_client->lastCacheStatus() : QString();
    }

    LrclibClient *client() const { return m_client; }

private Q_SLOTS:
    void onLoaded(const QString &requestKey, const Lyrics &lyrics);
    void onMissing(const QString &requestKey);

private:
    LrclibClient *m_client;
    quint64 m_currentGeneration = 0;
    QString m_pendingKey;
};
