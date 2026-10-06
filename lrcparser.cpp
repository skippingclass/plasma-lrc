/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcparser.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace
{
// Matches timestamps like [01:23.45], [01:23.456], [01:23:45], [01:23]
const QRegularExpression s_timestampRegex(QStringLiteral(R"(\[(\d{1,2}):(\d{2})(?:[.:](\d{1,3}))?\])"));

// Matches metadata tags like [ar: Artist], [ti: Title], [offset: 500]
const QRegularExpression s_metaRegex(QStringLiteral(R"(^\[([a-zA-Z]+)\s*:\s*(.*)\]$)"));

struct RawLine
{
    qint64 startMs = 0;
    QString text;
};
}

namespace LrcParser
{
bool parseLrc(const QString &lrcText, Lyrics *lyrics, qint64 trackDurationMs)
{
    if (!lyrics) {
        return false;
    }

    lyrics->type = LyricsType::None;
    lyrics->lines.clear();
    lyrics->attribution = QStringLiteral("lrclib");
    lyrics->attributionUrl = QStringLiteral("https://lrclib.net");

    if (lrcText.trimmed().isEmpty()) {
        return false;
    }

    qint64 globalOffsetMs = 0;
    QVector<RawLine> rawLines;

    const QStringList inputLines = lrcText.split(QLatin1Char('\n'));
    for (const QString &rawInput : inputLines) {
        const QString line = rawInput.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        // Check for metadata tags
        const QRegularExpressionMatch metaMatch = s_metaRegex.match(line);
        if (metaMatch.hasMatch()) {
            const QString tag = metaMatch.captured(1).toLower();
            if (tag == QLatin1String("offset")) {
                bool ok = false;
                const qint64 offsetVal = metaMatch.captured(2).trimmed().toLongLong(&ok);
                if (ok) {
                    globalOffsetMs = offsetVal;
                }
            }
            continue;
        }

        // Find all timestamp tags in the line
        QList<qint64> timestamps;
        int lastTagEnd = 0;

        auto it = s_timestampRegex.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const qint64 minutes = match.captured(1).toLongLong();
            const qint64 seconds = match.captured(2).toLongLong();
            const QString fractionStr = match.captured(3);

            qint64 msFraction = 0;
            if (!fractionStr.isEmpty()) {
                if (fractionStr.length() == 1) {
                    msFraction = fractionStr.toLongLong() * 100;
                } else if (fractionStr.length() == 2) {
                    msFraction = fractionStr.toLongLong() * 10;
                } else {
                    msFraction = fractionStr.left(3).toLongLong();
                }
            }

            const qint64 totalMs = minutes * 60000 + seconds * 1000 + msFraction;
            timestamps.append(totalMs);
            lastTagEnd = match.capturedEnd();
        }

        if (timestamps.isEmpty()) {
            continue;
        }

        const QString lyricText = line.mid(lastTagEnd).trimmed();
        for (qint64 timeMs : timestamps) {
            rawLines.append({timeMs, lyricText});
        }
    }

    if (rawLines.isEmpty()) {
        return false;
    }

    // Sort by timestamp
    std::sort(rawLines.begin(), rawLines.end(), [](const RawLine &lhs, const RawLine &rhs) {
        return lhs.startMs < rhs.startMs;
    });

    // Apply offset and build final LyricLine entries with startMs and endMs
    for (int i = 0; i < rawLines.size(); ++i) {
        const RawLine &cur = rawLines.at(i);
        const qint64 startMs = qMax(0LL, cur.startMs + globalOffsetMs);

        // An empty line marks an intentional gap or end of verse/track
        if (cur.text.isEmpty()) {
            continue;
        }

        // Find next line to determine endMs
        qint64 endMs = 0;
        if (i + 1 < rawLines.size()) {
            endMs = qMax(0LL, rawLines.at(i + 1).startMs + globalOffsetMs);
        } else {
            // Last line: estimate end
            if (trackDurationMs > startMs) {
                endMs = qMin(trackDurationMs, startMs + 6000);
            } else {
                endMs = startMs + 6000;
            }
        }

        // Ensure endMs is strictly after startMs
        if (endMs <= startMs) {
            endMs = startMs + 3000;
        }

        LyricLine lyricLine;
        lyricLine.text = cur.text;
        lyricLine.startMs = startMs;
        lyricLine.endMs = endMs;
        lyrics->lines.append(lyricLine);
    }

    if (lyrics->lines.isEmpty()) {
        return false;
    }

    lyrics->type = LyricsType::Line;
    return true;
}
}
