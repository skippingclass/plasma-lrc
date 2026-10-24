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

Q_SIGNALS:
    void lyricsLoaded(quint64 generation, const Lyrics &lyrics, const QString &providerId);
    void lyricsFailed(quint64 generation, MissingReason reason, const QString &providerId);

protected:
    bool m_enabled = true;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(LyricsProvider::Capabilities)
