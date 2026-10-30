/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"
#include "spicylyrics.h"

class SpicyLyricsProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit SpicyLyricsProvider(QObject *parent = nullptr);
    ~SpicyLyricsProvider() override = default;

    QString id() const override { return QStringLiteral("spicy"); }
    QString displayName() const override { return QStringLiteral("Spicy Lyrics"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync | Capability::RequiresSpotifyId | Capability::RequiresAuthKey;
    }

    bool isConfigured() const override;
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

    SpicyLyrics *client() const { return m_spicy; }

private Q_SLOTS:
    void onLoaded(const QString &trackId, const Lyrics &lyrics);
    void onMissing(const QString &trackId, MissingReason reason);

private:
    SpicyLyrics *m_spicy;
    quint64 m_currentGeneration = 0;
    QString m_pendingTrackId;
    quint64 m_diagGeneration = 0;
    QString m_diagReason;
    QString m_diagCache;
};
