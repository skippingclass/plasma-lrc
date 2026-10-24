/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <QString>
#include <QVector>

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
    // A word can arrive in several pieces ("o" + "k"), and the timings are per
    // piece. These are the offsets of the whole word, so that the highlight
    // covers "ok" instead of blinking between its halves.
    int groupStart = 0;
    int groupEnd = 0;
    bool isBackground = false;
    int backgroundGroup = -1;
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
    bool isBackground = false;
};

enum class LyricsType {
    None, ///< nothing usable
    Static, ///< text without any timing
    Line, ///< line-synced
    Syllable, ///< word- or syllable-synced
};

/**
 * Why the API did not provide anything to display, so that the UI can tell the
 * difference between "no such track" and "no timings for it".
 */
enum class MissingReason {
    Unsynced, ///< the track is there, the text has no timings
    NotFound, ///< the API does not know the track
    Unreachable, ///< no network, key rejected, rate limited, bad reply
};

struct LyricContributor
{
    QString role; ///< e.g. "maker", "uploader", "maker & uploader"
    QString name; ///< exact username from API
    QString url;  ///< exact safe http(s) URL, or empty
};

struct LyricAttribution
{
    QString provider; ///< "Spicy Lyrics", "Apple Music", "Spotify", "lrclib", "lrc.red", etc.
    QString providerUrl; ///< safe http(s) URL for provider, if any
    QVector<LyricContributor> contributors;

    bool isEmpty() const
    {
        return provider.isEmpty() && contributors.isEmpty();
    }
};

/**
 * Lyrics of one track, as served by any provider.
 */
struct Lyrics
{
    LyricsType type = LyricsType::None;
    QVector<LyricLine> lines;

    QString attribution; ///< Human-readable plain text
    QString attributionUrl; ///< Compatibility URL
    LyricAttribution attributionData;
    qint64 validUntilMs = 0;

    bool isUsable() const
    {
        return (type == LyricsType::Line || type == LyricsType::Syllable) && !lines.isEmpty();
    }

    /// Index of the line that is being sung at @p positionMs, or -1.
    int lineAt(qint64 positionMs, int *nextLineIndex = nullptr) const
    {
        const auto it = std::upper_bound(lines.begin(), lines.end(), positionMs, [](qint64 value, const LyricLine &line) {
            return value < line.startMs;
        });

        if (nextLineIndex) {
            *nextLineIndex = (it != lines.end()) ? static_cast<int>(it - lines.begin()) : -1;
        }

        if (it == lines.begin()) {
            return -1;
        }

        const int newest = static_cast<int>(it - lines.begin()) - 1;

        // Normal newest active primary can return immediately
        if (!lines.at(newest).isBackground && positionMs < lines.at(newest).endMs) {
            return newest;
        }

        // Reverse search with early return for latest active primary,
        // while retaining newest active background fallback.
        int newestActiveBg = -1;
        for (int i = newest; i >= 0; --i) {
            const LyricLine &line = lines.at(i);
            if (positionMs >= line.startMs && positionMs < line.endMs) {
                if (!line.isBackground) {
                    return i;
                } else if (newestActiveBg == -1) {
                    newestActiveBg = i;
                }
            }
        }

        return newestActiveBg;
    }
};

/**
 * Metadata of the track currently reported by MPRIS / player.
 */
struct TrackMetadata
{
    QString title;
    QString artist;
    QString album;
    qint64 durationMs = 0;
    QString spotifyTrackId;   ///< 22-char base62 Spotify track id
    QString mprisTrackId;     ///< Raw D-Bus track ID / object path
    QString isrc;             ///< International Standard Recording Code if known
    bool isWebSession = false;///< Browser/web page media session
    QString url;              ///< xesam:url from MPRIS (e.g. file:///path/to/song.flac)
    bool searchNormalized = false;
};
