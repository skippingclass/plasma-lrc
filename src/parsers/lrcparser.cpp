/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcparser.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QRegularExpression>
#include <QStringTokenizer>
#include <algorithm>
#include <limits>

namespace
{
// Matches timestamps like [01:23.45], [01:23.456], [01:23:45], [01:23]
const QRegularExpression s_timestampRegex(QStringLiteral(R"(\[(\d{1,2}):(\d{2})(?:[.:](\d{1,3}))?\])"));

// Matches metadata tags like [ar: Artist], [ti: Title], [offset: 500]
const QRegularExpression s_metaRegex(QStringLiteral(R"(^\[([a-zA-Z]+)\s*:\s*(.*)\]$)"));
constexpr qint64 kFinalLineSpanMs = 6000;

struct RawLine
{
    qint64 startMs = 0;
    QString text;
};
}

namespace LrcParser
{
bool parseLrc(const QString &lrcText, Lyrics *lyrics, qint64 trackDurationMs, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    if (!lyrics) {
        return false;
    }

    if (!LyricsLimits::checkDecodedSize(lrcText.size() * sizeof(QChar))) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return false;
    }

    lyrics->type = LyricsType::None;
    lyrics->lines.clear();
    lyrics->attributionData.provider = QStringLiteral("lrclib");
    lyrics->attributionData.providerUrl = QStringLiteral("https://lrclib.net");
    lyrics->attributionData.contributors.clear();
    lyrics->attribution = QStringLiteral("lrclib");
    lyrics->attributionUrl = QStringLiteral("https://lrclib.net");

    if (lrcText.trimmed().isEmpty()) {
        return false;
    }

    qint64 globalOffsetMs = 0;
    QVector<RawLine> rawLines;

    int inputLineCount = 0;
    for (QStringView rawInput : QStringView(lrcText).tokenize(QLatin1Char('\n'))) {
        if (++inputLineCount > LyricsLimits::kMaxLines || !LyricsLimits::checkLineLength(rawInput.size())) {
            if (outLimitExceeded) {
                *outLimitExceeded = true;
            }
            return false;
        }
        const QStringView line = rawInput.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        // Check for metadata tags
        const QRegularExpressionMatch metaMatch = s_metaRegex.matchView(line);
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

        auto it = s_timestampRegex.globalMatchView(line);
        while (it.hasNext()) {
            if (rawLines.size() + timestamps.size() >= LyricsLimits::kMaxLines) {
                if (outLimitExceeded) {
                    *outLimitExceeded = true;
                }
                return false;
            }
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

        const QString lyricText = line.mid(lastTagEnd).trimmed().toString();
        if (TtmlParser::isCreditOrNotice(lyricText)) {
            for (qint64 timeMs : timestamps) {
                rawLines.append({timeMs, QString()});
            }
            continue;
        }
        for (qint64 timeMs : timestamps) {
            rawLines.append({timeMs, lyricText});
        }
    }

    if (rawLines.isEmpty()) {
        lyrics->type = LyricsType::Static;
        return false;
    }

    if (rawLines.size() > 1) {
        bool hasAdvancingTimestamps = false;
        for (const auto &l : rawLines) {
            if (l.startMs > 0) {
                hasAdvancingTimestamps = true;
                break;
            }
        }
        if (!hasAdvancingTimestamps) {
            lyrics->type = LyricsType::Static;
            return false;
        }
    }

    // Sort by timestamp stably to preserve original file order for identical timestamps
    std::stable_sort(rawLines.begin(), rawLines.end(), [](const RawLine &lhs, const RawLine &rhs) {
        return lhs.startMs < rhs.startMs;
    });

    // Positive LRC offsets advance lyrics. Leave room to estimate the final end.
    constexpr qint64 maxStartMs = std::numeric_limits<qint64>::max() - kFinalLineSpanMs;
    for (RawLine &line : rawLines) {
        const qint64 offset = qMax(globalOffsetMs, line.startMs - maxStartMs);
        line.startMs = qMax(0LL, line.startMs - offset);
    }

    // Lines sharing a timestamp end together. Empty lines still bound the
    // previous group, so explicit gaps and filtered credits keep their timing.
    for (qsizetype first = 0; first < rawLines.size();) {
        const qint64 startMs = rawLines.at(first).startMs;
        qsizetype next = first + 1;
        while (next < rawLines.size() && rawLines.at(next).startMs == startMs) {
            ++next;
        }

        qint64 endMs = startMs + kFinalLineSpanMs;
        if (next < rawLines.size()) {
            endMs = rawLines.at(next).startMs;
        } else if (trackDurationMs > startMs) {
            endMs = qMin(endMs, trackDurationMs);
        }

        for (; first < next; ++first) {
            const QString &text = rawLines.at(first).text;
            if (!text.isEmpty()) {
                LyricLine line;
                line.text = text;
                line.startMs = startMs;
                line.endMs = endMs;
                lyrics->lines.append(line);
            }
        }
    }

    if (lyrics->lines.isEmpty()) {
        return false;
    }

    if (!LyricsLimits::validateLyricsLimits(*lyrics)) {
        lyrics->lines.clear();
        lyrics->type = LyricsType::None;
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return false;
    }

    lyrics->type = LyricsType::Line;
    return true;
}
}
