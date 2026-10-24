/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QByteArray>
#include <QString>

namespace QrcDecoder {

/**
 * Decrypt hexadecimal QRC payload using Tencent 3DES-ECB and decompress via zlib.
 */
QString decryptHex(const QString &hexStr);

/**
 * Convert decrypted QRC text into standard Apple-compatible TTML.
 */
QString toTtml(const QString &qrcText);

/**
 * Convenience method: decrypt and parse into Lyrics structure (LyricsType::Syllable).
 */
bool parse(const QString &hexOrQrcText, Lyrics *outLyrics);

} // namespace QrcDecoder
