/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QString>

namespace TtmlParser
{
/**
 * Parses Apple Music / W3C TTML XML lyrics into a Lyrics structure.
 * Supports both line-synced (<p>) and syllable/word-synced (<span>) tracks.
 *
 * @param ttmlContent Raw TTML XML text.
 * @param lyrics Destination Lyrics struct.
 * @param trackDurationMs Optional duration to bound outro / last line.
 * @return True if at least one usable line was parsed.
 */
bool parseTtml(const QString &ttmlContent, Lyrics *lyrics, qint64 trackDurationMs = 0);

/**
 * Parses a TTML time expression (e.g. "00:03.760", "01:23.456", "00:01:23.456", "3.76s")
 * into milliseconds.
 *
 * @param timeStr Time string from TTML attribute.
 * @param outMs Output milliseconds.
 * @return True if time string was valid.
 */
bool parseTimestamp(const QString &timeStr, qint64 *outMs);
}
