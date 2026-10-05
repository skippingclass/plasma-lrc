/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>

/**
 * Whether an MPRIS player name is the one the user wrote down.
 *
 * Names on the bus carry more than the bare application: a browser is
 * "chromium.instance18422", and which number that is changes with every start. So
 * "chromium", "Chromium" and the full name all have to mean the same player.
 */
bool playerNameMatches(const QString &configured, const QString &playerName);

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
 * Splits "Artist - Song" when a player puts the channel name in the artist field.
 *
 * Only when the artist is empty or the left half contains what the player said,
 * so a title like "Jay-Z - Song" is not torn apart by accident.
 */
void splitTrackArtistAndTitle(QString *artist, QString *title);