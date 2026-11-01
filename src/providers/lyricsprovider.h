/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QObject>
#include <QString>

class LyricsProvider : public QObject
{
    Q_OBJECT

public:
    enum class Capability {
        None = 0,
        LineSync = 1 << 0,
        WordSync = 1 << 1,
        RequiresSpotifyId = 1 << 2,
        RequiresIsrc = 1 << 3,
        SearchByTitleArtist = 1 << 4,
        RequiresAuthKey = 1 << 5
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    explicit LyricsProvider(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~LyricsProvider() override = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual Capabilities capabilities() const = 0;

    virtual bool isConfigured() const { return true; }
    virtual bool isEnabled() const { return m_enabled; }
    virtual void setEnabled(bool enabled) { m_enabled = enabled; }

    virtual bool canHandle(const TrackMetadata &meta) const = 0;
    virtual void requestLyrics(const TrackMetadata &meta, quint64 generation) = 0;
    virtual void cancel() = 0;
    virtual void invalidateCache(const TrackMetadata &meta) { Q_UNUSED(meta); }
    virtual void clearAllCaches() {}
    virtual QString confirmedRejectionReason(quint64 generation) const { Q_UNUSED(generation); return QString(); }
    virtual QString cacheUsage(quint64 generation) const { Q_UNUSED(generation); return QString(); }

    virtual bool supportsCandidateSearch() const { return false; }
    virtual void searchCandidates(const TrackMetadata &meta, quint64 searchId) { Q_UNUSED(meta); Q_UNUSED(searchId); }
    virtual void fetchCandidateLyrics(const QString &candidateId, quint64 fetchId) { Q_UNUSED(candidateId); Q_UNUSED(fetchId); }
    virtual void cancelCandidateSearch(quint64 searchId) { Q_UNUSED(searchId); }
    virtual void cancelCandidateFetch(quint64 fetchId) { Q_UNUSED(fetchId); }

Q_SIGNALS:
    void lyricsLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void lyricsFailed(quint64 generation, MissingReason reason, const QString &providerId);
    void candidatesFound(quint64 searchId, const QList<LyricCandidate> &candidates, const QString &providerId);
    void candidateLyricsLoaded(quint64 fetchId, const Lyrics &lyrics, const QString &providerId);
    void candidateLyricsFailed(quint64 fetchId, MissingReason reason, const QString &providerId);

protected:
    bool m_enabled = true;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(LyricsProvider::Capabilities)
