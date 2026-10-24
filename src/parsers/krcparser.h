/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QByteArray>
#include <QString>

namespace KrcParser {

/**
 * Decrypt and decompress raw or base64-encoded KRC data to plain text.
 */
QString decode(const QByteArray &rawOrBase64Data);

/**
 * Convert decrypted KRC text into standard Apple-compatible TTML.
 */
QString toTtml(const QString &krcText);

/**
 * Convenience method: decode and parse into Lyrics structure (LyricsType::Syllable).
 */
bool parse(const QByteArray &rawOrBase64Data, Lyrics *outLyrics);

} // namespace KrcParser
