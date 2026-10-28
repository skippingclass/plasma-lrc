/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QString>

namespace YrcParser {

/**
 * Convert NetEase YRC lyric text into standard Apple-compatible TTML.
 */
QString toTtml(const QString &yrcText);

/**
 * Parse NetEase YRC lyric text into Lyrics structure (LyricsType::Syllable).
 */
bool parse(const QString &yrcText, Lyrics *outLyrics, qint64 trackDurationMs = 0);

} // namespace YrcParser
