/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

/**
 * The mpris:trackid as text.
 *
 * MPRIS says it is an object path, and Qt hands that over as a QDBusObjectPath —
 * a QVariant of which QVariant::toString() answers with an empty string. Every
 * rule about track ids, and the lookup by Spotify id, were quietly reading nothing
 * for every player that follows the spec on this point.
 */
QString mprisTrackId(const QVariant &value);

/**
 * How much a player looks like it is playing music rather than something that
 * merely implements MPRIS.
 *
 * Plenty of applications implement org.mpris.MediaPlayer2.Player for things that
 * are not tracks: Telegram Desktop reports an unviewed voice message in a
 * minimised window as "Playing", and a browser makes up a media session for a clip
 * on a social network. What tells a real player apart is the metadata a track has
 * and those do not: an artist, an album, a track number, a length that is not a
 * few seconds.
 *
 * The rules live in a file of their own so that they can be tested: they decide
 * which player the panel follows, and every one of them was wrong at least once.
 */
int musicScore(const QVariantMap &properties);

/**
 * Whether an MPRIS player name is the one the user wrote down.
 *
 * Names on the bus carry more than the bare application: a browser is
 * "chromium.instance18422", and which number that is changes with every start. So
 * "chromium", "Chromium" and the full name all have to mean the same player.
 */
bool playerNameMatches(const QString &configured, const QString &playerName);

/**
 * The artist of a track, with the empty entries a browser puts in the list taken
 * out. A media session for a web page carries [""] and nothing else.
 */
QStringList trackArtists(const QVariant &value);
