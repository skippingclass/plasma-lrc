/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QHash>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QVector>

class QNetworkReply;
class QTimer;

#include "lyrictypes.h"

/**
 * Minimal client for https://api.spicylyrics.org/v1/lyrics/{spotifyTrackId}.
 *
 * Responses are cached under the user's cache directory for 30 days, which is
 * what the API terms ask for: a contributor can withdraw a sync at any time and
 * the operator has no way to reach copies that are older than that.
 */
class SpicyLyrics : public QObject
{
    Q_OBJECT

public:
    explicit SpicyLyrics(QObject *parent = nullptr);
    ~SpicyLyrics() override;

    /// API key. Empty disables the source entirely.
    void setKey(const QString &key);
    QString key() const;

    /// Whether this source may be used at all.
    void setEnabled(bool enabled);
    bool isEnabled() const;

    /// Cancels any active network request.
    void cancel();
    /// Explicit refresh of one track, including a cached negative result.
    void invalidate(const QString &trackId);
    int lastHttpStatus() const { return m_lastHttpStatus; }
    QString lastErrorCode() const { return m_lastErrorCode; }

    /**
     * Fetches the lyrics for @p trackId, or answers from the cache.
     *
     * Exactly one of loaded() or missing() is emitted per request.
     */
    void request(const QString &trackId);

    /**
     * Extracts a Spotify track id from an MPRIS mpris:trackid value.
     *
     * The desktop client reports "/com/spotify/track/<id>", other players and
     * the web player report "spotify:track:<id>".
     */
    /// Takes the mpris:trackid as text: the caller has to unwrap the object path first.
    static QString trackIdFromMpris(const QString &trackId);

    /// Parses an API response. Exposed for testing.
    static bool parse(const QByteArray &json, Lyrics *lyrics, QString *error);

    static bool isSafeHttpUrl(const QString &url);
    static QString escapeHtml(const QString &text);
    static LyricAttribution parseAttribution(const QString &source, const QJsonObject &uploadAttribution);
    static QString buildAttributionText(const LyricAttribution &attr);
    static QString buildAttributionHtml(const LyricAttribution &attr);
    static QString primaryAttributionUrl(const LyricAttribution &attr);

    /// Directory used for the response cache.
    static QString cacheDirectory();
    void clearCache();

Q_SIGNALS:
    /// The response for @p trackId arrived and has usable timings.
    void loaded(const QString &trackId, const Lyrics &lyrics);
    /// The API cannot show @p trackId, and @p reason says why.
    void missing(const QString &trackId, MissingReason reason);

private:
    void load(const QString &trackId);
    /// Writes the cache entry for a track: a response, or a marker.
    void write(const QString &trackId, const QByteArray &payload);
    /// Deletes cache entries that are past their time to live.
    void pruneCache();
    void fetch(const QString &trackId);
    void emitMissing(const QString &trackId, MissingReason reason);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_activeReply;
    struct CachedLyrics {
        Lyrics lyrics;
        qint64 expiresAtMs;
    };
    QHash<QString, CachedLyrics> m_memoryCache;
    QTimer *m_watchdog;
    QTimer *m_cleanupTimer = nullptr;
    quint64 m_generation = 0;
    int m_lastHttpStatus = 0;
    QString m_lastErrorCode;
    QElapsedTimer m_retryClock;
    qint64 m_retryDelayMs = 0;
    QString m_key;
    QString m_currentTrackId;
    bool m_enabled = false;
    bool m_suspended = false;
};
