/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "spicylyrics.h"

#include <QString>

namespace LrcParser
{
/**
 * Parses standard LRC text (lines with timestamps like [mm:ss.xx] or [mm:ss.xxx])
 * into a Lyrics object with Line sync.
 *
 * @param lrcText The full content of a .lrc file.
 * @param lyrics Destination Lyrics struct.
 * @param trackDurationMs Track duration in milliseconds (if known), used to bound the final line.
 * @return True if at least one usable line was parsed.
 */
bool parseLrc(const QString &lrcText, Lyrics *lyrics, qint64 trackDurationMs = 0);
}
