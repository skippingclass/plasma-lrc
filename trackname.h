/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>


/**
 * Cleans up the artist and title a media player reports.
 *
 * Browsers, YouTube and yt-dlp hand out things like "Song (Official Video) [4K]"
 * or "Artist - Topic", and lrclib is searched by words: "deaf note (with Playboi
 * Carti) [Official Video]" finds nothing where "deaf note" finds the track.
 *
 * The rules come from a lyrics widget that cleans titles the same way before its
 * own lookup; the regexes live in a file of their own so that they can be
 * tested, since they rot silently.
 */

/** Strips "(Official Video)", "[4K]", "| chapter list", "(feat. …)" and friends. */
QString cleanTrackTitle(QString title);

/** Strips "- Topic", "VEVO", a trailing "(official)" and a feature list. */
QString cleanTrackArtist(QString artist);

/**
 * Splits "Artist - Song" when a player puts something else in the artist field.
 *
 * A browser reports the channel that uploaded a video, so "Ken Carson - deaf note"
 * arrives with the artist set to whichever channel did the upload. The split
 * therefore happens when the artist is empty, when the left half contains what the
 * player said, or when @p fromWebPage says the metadata came from a web page
 * rather than from a music player — the last one is what keeps "Love - Hate" by
 * Drake from being torn into artist "Love" when it arrives from mpv.
 */
void splitTrackArtistAndTitle(QString *artist, QString *title, bool fromWebPage = false);