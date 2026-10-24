/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyricsprovider.h"

class LocalProvider : public LyricsProvider
{
    Q_OBJECT

public:
    explicit LocalProvider(QObject *parent = nullptr);
    ~LocalProvider() override = default;

    QString id() const override { return QStringLiteral("local"); }
    QString displayName() const override { return QStringLiteral("Local Files"); }
    Capabilities capabilities() const override
    {
        return Capability::WordSync | Capability::LineSync;
    }

    bool canHandle(const TrackMetadata &meta) const override;
    void requestLyrics(const TrackMetadata &meta, quint64 generation) override;
    void cancel() override;

    QString customDirectory() const { return m_customDirectory; }
    void setCustomDirectory(const QString &dir) { m_customDirectory = dir; }

private:
    QString findLocalFile(const TrackMetadata &meta) const;

    QString m_customDirectory;
    quint64 m_currentGeneration = 0;
};
