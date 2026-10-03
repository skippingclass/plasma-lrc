/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QVector>

class QTimer;

/**
 * One timed piece of a line: a whole word, or a syllable inside a word.
 */
struct LyricWord
{
    QString text;
    qint64 startMs = 0;
    qint64 endMs = 0;
    // Offsets into LyricLine::text, so that the UI can split the line around
    // the word that is being sung right now.
    int textStart = 0;
    int textEnd = 0;
};

/**
 * One timed lyric line.
 */
struct LyricLine
{
    QString text;
    qint64 startMs = 0;
    qint64 endMs = 0;
    QVector<LyricWord> words;
};

enum class LyricsType {
    None, ///< nothing usable
    Static, ///< text without any timing
    Line, ///< line-synced
    Syllable, ///< word- or syllable-synced
};

/**
 * Lyrics of one track, as served by the Spicy Lyrics API.
 *
 * The API answers with the best sync it has for a Spotify track id: a
 * community-made word-level sync if there is one, otherwise Apple Music,
 * otherwise Spotify itself.
 */
struct Lyrics
{
    LyricsType type = LyricsType::None;
    QVector<LyricLine> lines;

    QString attribution; ///< "Spicy Lyrics · слова: maker", already human readable
    QString attributionUrl; ///< profile of the contributor to link to

    bool isUsable() const
    {
        return (type == LyricsType::Line || type == LyricsType::Syllable) && !lines.isEmpty();
    }

    /// Index of the line that is being sung at @p positionMs, or -1.
    int lineAt(qint64 positionMs) const;
};

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
    static QString trackIdFromMpris(const QVariant &mprisTrackId);

    /// Parses an API response. Exposed for testing.
    static bool parse(const QByteArray &json, Lyrics *lyrics, QString *error);

    /// Directory used for the response cache.
    static QString cacheDirectory();

Q_SIGNALS:
    /// The response for @p trackId arrived and has usable timings.
    void loaded(const QString &trackId, const Lyrics &lyrics);
    /// The API has nothing timed for @p trackId; fall back to lrc_tty.
    void missing(const QString &trackId);

private:
    void load(const QString &trackId);
    void save(const QString &trackId, const QByteArray &json);
    void fetch(const QString &trackId);
    void emitMissing(const QString &trackId);

    QNetworkAccessManager m_network;
    QHash<QString, Lyrics> m_memoryCache;
    QTimer *m_watchdog;
    QString m_key;
    bool m_enabled = false;
};