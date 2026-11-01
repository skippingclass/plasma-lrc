/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#if __has_include(<QNetworkReply>)
#include <QNetworkReply>
#endif

namespace LyricsLimits
{
    // Max input response or file size: 8 MiB (8,388,608 bytes)
    constexpr qint64 kMaxInputSizeBytes = 8LL * 1024 * 1024;

    // Max decompressed or decoded text size: 16 MiB (16,777,216 bytes)
    constexpr qint64 kMaxDecodedSizeBytes = 16LL * 1024 * 1024;

    // Max lines allowed in a lyric document
    constexpr int kMaxLines = 10000;

    // Max words/syllables across all lines
    constexpr int kMaxWords = 100000;

    // Max UTF-16 code units in a single line
    constexpr int kMaxLineLengthChars = 16384;

    inline bool checkInputSize(qint64 size)
    {
        return size >= 0 && size <= kMaxInputSizeBytes;
    }

    inline bool checkDecodedSize(qint64 size)
    {
        return size >= 0 && size <= kMaxDecodedSizeBytes;
    }

    inline bool checkLineLength(int length)
    {
        return length >= 0 && length <= kMaxLineLengthChars;
    }

    inline bool validateLyricsLimits(const Lyrics &lyrics)
    {
        if (lyrics.lines.size() > kMaxLines) {
            return false;
        }

        int totalWords = 0;
        for (const auto &line : lyrics.lines) {
            if (line.text.size() > kMaxLineLengthChars) {
                return false;
            }
            if (!line.words.isEmpty()) {
                totalWords += line.words.size();
            } else {
                // Approximate word count for line-synced or unsynced
                totalWords += line.text.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
            }
            if (totalWords > kMaxWords) {
                return false;
            }
        }

        return true;
    }

#if __has_include(<QNetworkReply>)
    inline void attachSizeLimitGuard(QNetworkReply *reply, qint64 maxBytes = kMaxInputSizeBytes)
    {
        if (!reply) {
            return;
        }

        reply->setProperty("_lrc_limit_exceeded", false);

        QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, maxBytes](qint64 bytesReceived, qint64 bytesTotal) {
            if (bytesReceived > maxBytes || bytesTotal > maxBytes) {
                reply->setProperty("_lrc_limit_exceeded", true);
                reply->abort();
            }
        });

        // Also guard chunked streaming without Content-Length
        QObject::connect(reply, &QIODevice::readyRead, reply, [reply, maxBytes]() {
            if (reply->bytesAvailable() > maxBytes) {
                reply->setProperty("_lrc_limit_exceeded", true);
                reply->abort();
            }
        });
    }

    inline bool isSizeLimitExceeded(QNetworkReply *reply)
    {
        if (!reply) {
            return false;
        }
        return reply->property("_lrc_limit_exceeded").toBool();
    }
#endif
}
