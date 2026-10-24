/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "ttmlparser.h"

#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

namespace TtmlParser
{
namespace
{
bool isWideScript(QChar c)
{
    return (c.unicode() >= 0x1100 && c.unicode() <= 0x11FF) // Hangul Jamo
        || (c.unicode() >= 0x2E80 && c.unicode() <= 0xA4CF) // CJK radicals .. Yi
        || (c.unicode() >= 0xAC00 && c.unicode() <= 0xD7A3) // Hangul syllables
        || (c.unicode() >= 0xF900 && c.unicode() <= 0xFAFF) // CJK compatibility
        || (c.unicode() >= 0xFF00 && c.unicode() <= 0xFFEF); // halfwidth/fullwidth
}

bool containsWideScript(const QString &text)
{
    for (const QChar c : text) {
        if (isWideScript(c)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool parseTimestamp(const QString &timeStr, qint64 *outMs)
{
    if (!outMs) {
        return false;
    }
    const QString s = timeStr.trimmed();
    if (s.isEmpty()) {
        return false;
    }

    // Format 1: seconds with "s" or "ms" suffix, e.g. "12.34s" or "1234ms"
    if (s.endsWith(QLatin1String("ms"), Qt::CaseInsensitive)) {
        bool ok = false;
        const double val = s.left(s.length() - 2).trimmed().toDouble(&ok);
        if (ok && val >= 0.0) {
            *outMs = static_cast<qint64>(val);
            return true;
        }
        return false;
    }
    if (s.endsWith(QLatin1Char('s'), Qt::CaseInsensitive)) {
        bool ok = false;
        const double val = s.left(s.length() - 1).trimmed().toDouble(&ok);
        if (ok && val >= 0.0) {
            *outMs = static_cast<qint64>(val * 1000.0);
            return true;
        }
        return false;
    }

    // Format 2: colon separated, e.g. "mm:ss.xxx" or "hh:mm:ss.xxx"
    if (s.contains(QLatin1Char(':'))) {
        const QStringList parts = s.split(QLatin1Char(':'));
        if (parts.size() == 2) {
            bool okMin = false;
            bool okSec = false;
            const qint64 mins = parts.at(0).trimmed().toLongLong(&okMin);
            const double secs = parts.at(1).trimmed().toDouble(&okSec);
            if (okMin && okSec && mins >= 0 && secs >= 0.0) {
                *outMs = mins * 60000 + static_cast<qint64>(secs * 1000.0);
                return true;
            }
        } else if (parts.size() == 3) {
            bool okHour = false;
            bool okMin = false;
            bool okSec = false;
            const qint64 hours = parts.at(0).trimmed().toLongLong(&okHour);
            const qint64 mins = parts.at(1).trimmed().toLongLong(&okMin);
            const double secs = parts.at(2).trimmed().toDouble(&okSec);
            if (okHour && okMin && okSec && hours >= 0 && mins >= 0 && secs >= 0.0) {
                *outMs = hours * 3600000 + mins * 60000 + static_cast<qint64>(secs * 1000.0);
                return true;
            }
        }
        return false;
    }

    // Format 3: pure numeric seconds "12.34"
    bool ok = false;
    const double val = s.toDouble(&ok);
    if (ok && val >= 0.0) {
        *outMs = static_cast<qint64>(val * 1000.0);
        return true;
    }

    return false;
}

bool parseTtml(const QString &ttmlContent, Lyrics *lyrics, qint64 trackDurationMs)
{
    if (!lyrics || ttmlContent.trimmed().isEmpty()) {
        return false;
    }

    QXmlStreamReader xml(ttmlContent);
    QVector<LyricLine> parsedLines;
    bool hasAnyWordTimings = false;

    struct TtmlChunk {
        bool isTimed = false;
        bool isWhitespace = false;
        QString text;
        qint64 startMs = -1;
        qint64 endMs = -1;
    };

    while (!xml.atEnd() && !xml.hasError()) {
        const QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            const QStringView tagName = xml.name();

            if (tagName == QLatin1String("p")) {
                const QXmlStreamAttributes pAttrs = xml.attributes();
                qint64 pBegin = -1;
                qint64 pEnd = -1;
                if (pAttrs.hasAttribute(QLatin1String("begin"))) {
                    parseTimestamp(pAttrs.value(QLatin1String("begin")).toString(), &pBegin);
                }
                if (pAttrs.hasAttribute(QLatin1String("end"))) {
                    parseTimestamp(pAttrs.value(QLatin1String("end")).toString(), &pEnd);
                }

                QVector<TtmlChunk> chunks;
                QString directText;
                int depth = 1;

                while (depth > 0 && !xml.atEnd() && !xml.hasError()) {
                    const QXmlStreamReader::TokenType innerToken = xml.readNext();

                    if (innerToken == QXmlStreamReader::StartElement) {
                        const QStringView name = xml.name();
                        if (name == QLatin1String("span")) {
                            const QXmlStreamAttributes spanAttrs = xml.attributes();
                            const QString role = spanAttrs.value(QLatin1String("ttm:role")).toString();
                            if (role == QLatin1String("x-translation") || role == QLatin1String("x-roman")) {
                                xml.skipCurrentElement();
                                continue;
                            }

                            qint64 spanBegin = -1;
                            qint64 spanEnd = -1;
                            const bool hasBegin = spanAttrs.hasAttribute(QLatin1String("begin"))
                                && parseTimestamp(spanAttrs.value(QLatin1String("begin")).toString(), &spanBegin);
                            const bool hasEnd = spanAttrs.hasAttribute(QLatin1String("end"))
                                && parseTimestamp(spanAttrs.value(QLatin1String("end")).toString(), &spanEnd);

                            if (hasBegin) {
                                hasAnyWordTimings = true;
                                const QString spanContent = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                                TtmlChunk c;
                                c.isTimed = true;
                                c.text = spanContent;
                                c.startMs = spanBegin;
                                c.endMs = hasEnd ? spanEnd : -1;
                                chunks.append(c);
                            } else {
                                // Untimed container span (e.g. x-bg)
                                ++depth;
                            }
                        } else if (name == QLatin1String("br")) {
                            TtmlChunk ws;
                            ws.isWhitespace = true;
                            chunks.append(ws);
                            xml.skipCurrentElement();
                        } else {
                            xml.skipCurrentElement();
                        }
                    } else if (innerToken == QXmlStreamReader::Characters) {
                        if (xml.isWhitespace()) {
                            const QString wsText = xml.text().toString();
                            if (!wsText.contains(QLatin1Char('\n')) && !wsText.contains(QLatin1Char('\r'))) {
                                TtmlChunk ws;
                                ws.isWhitespace = true;
                                chunks.append(ws);
                            }
                        } else {
                            TtmlChunk c;
                            c.isTimed = false;
                            c.text = xml.text().toString();
                            chunks.append(c);
                            directText += xml.text().toString();
                        }
                    } else if (innerToken == QXmlStreamReader::EndElement) {
                        --depth;
                        if (depth <= 0) {
                            break;
                        }
                    }
                }

                LyricLine line;
                line.startMs = qMax<qint64>(0, pBegin);
                line.endMs = pEnd > line.startMs ? pEnd : line.startMs;

                QString built;
                QVector<bool> wordBoundaryAfter;
                bool pendingSpace = false;

                for (int i = 0; i < chunks.size(); ++i) {
                    const TtmlChunk &c = chunks.at(i);
                    if (c.isWhitespace) {
                        pendingSpace = true;
                    } else if (c.isTimed) {
                        QString piece = c.text;
                        int leadingWs = 0;
                        while (leadingWs < piece.length() && piece.at(leadingWs).isSpace()) {
                            ++leadingWs;
                        }
                        int trailingWs = 0;
                        while (trailingWs < piece.length() && piece.at(piece.length() - 1 - trailingWs).isSpace()) {
                            ++trailingWs;
                        }

                        const QString trimmed = piece.trimmed();
                        if (trimmed.isEmpty()) {
                            pendingSpace = true;
                            continue;
                        }

                        if (pendingSpace || leadingWs > 0) {
                            if (!built.isEmpty() && !built.endsWith(QLatin1Char(' '))) {
                                built.append(QLatin1Char(' '));
                            }
                            if (!line.words.isEmpty()) {
                                wordBoundaryAfter.append(true);
                            }
                        } else if (!line.words.isEmpty()) {
                            wordBoundaryAfter.append(false);
                        }

                        LyricWord w;
                        w.text = trimmed;
                        w.startMs = c.startMs >= 0 ? c.startMs : line.startMs;
                        w.endMs = c.endMs > w.startMs ? c.endMs : w.startMs;
                        w.textStart = static_cast<int>(built.length());
                        built.append(trimmed);
                        w.textEnd = static_cast<int>(built.length());
                        line.words.append(w);

                        pendingSpace = (trailingWs > 0);
                    } else if (!c.text.trimmed().isEmpty()) {
                        if (pendingSpace) {
                            if (!built.isEmpty() && !built.endsWith(QLatin1Char(' '))) {
                                built.append(QLatin1Char(' '));
                            }
                            if (!line.words.isEmpty()) {
                                wordBoundaryAfter.append(true);
                            }
                        }
                        built.append(c.text.trimmed());
                        pendingSpace = c.text.endsWith(QLatin1Char(' '));
                    }
                }
                if (!line.words.isEmpty()) {
                    wordBoundaryAfter.append(true);
                }

                if (line.words.isEmpty()) {
                    line.text = directText.trimmed();
                } else {
                    line.text = built;
                    if (line.startMs <= 0 && !line.words.isEmpty()) {
                        line.startMs = line.words.first().startMs;
                    }
                    if (line.endMs <= line.startMs && !line.words.isEmpty()) {
                        line.endMs = line.words.last().endMs;
                    }

                    // Compute groupStart / groupEnd
                    int groupStart = 0;
                    for (int i = 0; i < line.words.size(); ++i) {
                        if (i == groupStart) {
                            int groupEndIdx = i;
                            while (groupEndIdx < line.words.size() && !wordBoundaryAfter.at(groupEndIdx)) {
                                if (containsWideScript(line.words.at(groupEndIdx).text)) {
                                    break;
                                }
                                ++groupEndIdx;
                            }
                            if (groupEndIdx >= line.words.size()) {
                                groupEndIdx = line.words.size() - 1;
                            }

                            const int startOffset = line.words.at(i).textStart;
                            const int endOffset = line.words.at(groupEndIdx).textEnd;
                            for (int k = i; k <= groupEndIdx; ++k) {
                                line.words[k].groupStart = startOffset;
                                line.words[k].groupEnd = endOffset;
                            }
                            groupStart = groupEndIdx + 1;
                        }
                    }
                }

                if (!line.text.isEmpty()) {
                    parsedLines.append(line);
                }
            }
        }
    }

    if (parsedLines.isEmpty()) {
        return false;
    }

    // Sort lines by start time
    std::sort(parsedLines.begin(), parsedLines.end(), [](const LyricLine &a, const LyricLine &b) {
        return a.startMs < b.startMs;
    });

    // Ensure each line has a valid endMs
    for (int i = 0; i < parsedLines.size(); ++i) {
        LyricLine &line = parsedLines[i];
        if (line.endMs <= line.startMs) {
            if (i + 1 < parsedLines.size() && parsedLines.at(i + 1).startMs > line.startMs) {
                line.endMs = parsedLines.at(i + 1).startMs;
            } else if (trackDurationMs > line.startMs) {
                line.endMs = qMin(trackDurationMs, line.startMs + 5000);
            } else {
                line.endMs = line.startMs + 3500;
            }
        }
        // If line has words but words have missing endMs, fix them up
        for (int w = 0; w < line.words.size(); ++w) {
            LyricWord &word = line.words[w];
            if (word.endMs <= word.startMs) {
                if (w + 1 < line.words.size() && line.words.at(w + 1).startMs > word.startMs) {
                    word.endMs = line.words.at(w + 1).startMs;
                } else {
                    word.endMs = line.endMs;
                }
            }
        }
    }

    lyrics->lines = parsedLines;
    lyrics->type = hasAnyWordTimings ? LyricsType::Syllable : LyricsType::Line;
    lyrics->attributionData.provider = QStringLiteral("Apple Music (TTML)");
    lyrics->attributionData.providerUrl = QStringLiteral("https://www.apple.com/apple-music/");
    lyrics->attribution = QStringLiteral("Apple Music (TTML)");
    lyrics->attributionUrl = lyrics->attributionData.providerUrl;

    return lyrics->isUsable();
}
} // namespace TtmlParser
