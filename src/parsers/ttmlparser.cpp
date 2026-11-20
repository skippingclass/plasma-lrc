/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QXmlStreamReader>
#include <QtCore/qnumeric.h>
#include <algorithm>
#include <limits>

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

bool isCjkHanziOrKana(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3040 && u <= 0x30FF)   // Hiragana / Katakana
        || (u >= 0x3400 && u <= 0x4DBF)   // CJK Ext A
        || (u >= 0x4E00 && u <= 0x9FFF)   // CJK Unified
        || (u >= 0xF900 && u <= 0xFAFF);  // CJK Compatibility
}

bool isOpenBracket(QChar c)
{
    return c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')
        || c == QChar(0xFF08) || c == QChar(0x3010);
}

bool isCloseBracket(QChar c)
{
    return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')
        || c == QChar(0xFF09) || c == QChar(0x3011);
}

bool isClosingPunctuation(QChar c)
{
    return c == QLatin1Char(',') || c == QLatin1Char('.') || c == QLatin1Char('?')
        || c == QLatin1Char('!') || c == QLatin1Char(':') || c == QLatin1Char(';')
        || c == QChar(0xFF0C) || c == QChar(0x3002) || c == QChar(0xFF01)
        || c == QChar(0xFF1F) || c == QChar(0xFF1A) || c == QChar(0xFF1B);
}

bool calculateShouldInsertSpace(bool pendingSpace, int leadingWs, const QString &built, const QString &trimmed)
{
    if (trimmed.isEmpty()) {
        return false;
    }

    const bool needSpaceBeforeBracket = !built.isEmpty()
        && !built.endsWith(QLatin1Char(' '))
        && (isOpenBracket(trimmed.front()) || isCloseBracket(built.back()));

    bool shouldInsertSpace = pendingSpace || leadingWs > 0 || needSpaceBeforeBracket;

    if (!built.isEmpty()) {
        const QChar prevC = built.back();
        const QChar nextC = trimmed.front();

        if (isCjkHanziOrKana(prevC) && isCjkHanziOrKana(nextC)) {
            shouldInsertSpace = false;
        }
        if (isOpenBracket(prevC)) {
            shouldInsertSpace = false;
        }
        if (isCloseBracket(nextC) || isClosingPunctuation(nextC)) {
            shouldInsertSpace = false;
        }
    }

    return shouldInsertSpace;
}

QString getRoleAttribute(const QXmlStreamAttributes &attrs)
{
    const QStringView metaVal = attrs.value(QLatin1String("http://www.w3.org/ns/ttml#metadata"), QLatin1String("role"));
    if (!metaVal.isEmpty()) {
        return metaVal.toString();
    }
    const QStringView ttmVal = attrs.value(QLatin1String("ttm:role"));
    if (!ttmVal.isEmpty()) {
        return ttmVal.toString();
    }
    return attrs.value(QLatin1String("role")).toString();
}

bool parseTimeUnit(QStringView value, qint64 millisecondsPerUnit, qint64 *outMs)
{
    value = value.trimmed();
    const QStringView decimal = value.startsWith(QLatin1Char('+')) ? value.sliced(1) : value;
    const qsizetype dot = decimal.indexOf(QLatin1Char('.'));
    const QStringView whole = dot < 0 ? decimal : decimal.first(dot);
    const QStringView fraction = dot < 0 ? QStringView() : decimal.sliced(dot + 1);
    const auto isDigit = [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    };
    if ((!whole.isEmpty() || !fraction.isEmpty())
        && std::all_of(whole.begin(), whole.end(), isDigit)
        && std::all_of(fraction.begin(), fraction.end(), isDigit)) {
        bool ok = true;
        const qint64 units = whole.isEmpty() ? 0 : whole.toLongLong(&ok);
        qint64 ms = 0;
        if (!ok || qMulOverflow(units, millisecondsPerUnit, &ms)) {
            return false;
        }
        // Convert ordinary decimal fractions exactly and truncate below a millisecond.
        if (millisecondsPerUnit == 1000) {
            qint64 place = 100;
            for (QChar digit : fraction.left(qMin<qsizetype>(fraction.size(), 3))) {
                if (qAddOverflow(ms, digit.digitValue() * place, &ms)) {
                    return false;
                }
                place /= 10;
            }
        }
        *outMs = ms;
        return true;
    }

    // Preserve accepted numeric forms such as scientific notation.
    bool ok = false;
    const double ms = value.toDouble(&ok) * millisecondsPerUnit;
    if (ok && ms >= 0.0 && ms < 0x1p63) {
        *outMs = static_cast<qint64>(ms);
        return true;
    }
    return false;
}

qint64 endWithFallbackSpan(qint64 startMs, qint64 spanMs)
{
    return startMs + qMin(spanMs, std::numeric_limits<qint64>::max() - startMs);
}

bool parseTimestampView(QStringView time, qint64 *outMs)
{
    if (!outMs) {
        return false;
    }
    const QStringView s = time.trimmed();
    if (s.endsWith(QLatin1String("ms"), Qt::CaseInsensitive)) {
        return parseTimeUnit(s.chopped(2), 1, outMs);
    }
    if (s.endsWith(QLatin1Char('s'), Qt::CaseInsensitive)) {
        return parseTimeUnit(s.chopped(1), 1000, outMs);
    }
    if (!s.contains(QLatin1Char(':'))) {
        return parseTimeUnit(s, 1000, outMs);
    }

    const auto parts = s.split(QLatin1Char(':'));
    if (parts.size() > 3) {
        return false;
    }
    qint64 clockMs = 0;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        bool ok = false;
        const qint64 units = parts.at(i).trimmed().toLongLong(&ok);
        const qint64 scale = i + 2 == parts.size() ? 60000 : 3600000;
        qint64 unitMs = 0;
        if (!ok || units < 0 || qMulOverflow(units, scale, &unitMs) || qAddOverflow(clockMs, unitMs, &clockMs)) {
            return false;
        }
    }
    qint64 secondMs = 0;
    if (!parseTimeUnit(parts.last(), 1000, &secondMs) || qAddOverflow(clockMs, secondMs, &clockMs)) {
        return false;
    }
    *outMs = clockMs;
    return true;
}
} // namespace

bool parseTimestamp(const QString &timeStr, qint64 *outMs)
{
    return parseTimestampView(timeStr, outMs);
}

bool parseTtml(const QString &ttmlContent, Lyrics *lyrics, qint64 trackDurationMs, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    if (!lyrics || ttmlContent.trimmed().isEmpty()) {
        return false;
    }

    if (!LyricsLimits::checkDecodedSize(ttmlContent.size() * sizeof(QChar))) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return false;
    }

    QXmlStreamReader xml(ttmlContent);
    QVector<LyricLine> parsedLines;
    bool hasAnyWordTimings = false;
    bool isExplicitlyUnsynced = false;
    int timedLinesCount = 0;

    struct TtmlChunk {
        bool isTimed = false;
        bool isWhitespace = false;
        QString text;
        qint64 startMs = -1;
        qint64 endMs = -1;
        bool isBackground = false;
        int backgroundGroup = -1;
    };

    while (!xml.atEnd() && !xml.hasError()) {
        const QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            const QStringView tagName = xml.name();

            if (tagName == QLatin1String("tt")) {
                const QXmlStreamAttributes ttAttrs = xml.attributes();
                for (const auto &attr : ttAttrs) {
                    if (attr.name().toString().compare(QLatin1String("timing"), Qt::CaseInsensitive) == 0
                        || attr.qualifiedName().toString().endsWith(QLatin1String(":timing"), Qt::CaseInsensitive)) {
                        const QString timingVal = attr.value().toString().trimmed();
                        if (timingVal.compare(QLatin1String("None"), Qt::CaseInsensitive) == 0
                            || timingVal.compare(QLatin1String("Unsynced"), Qt::CaseInsensitive) == 0) {
                            isExplicitlyUnsynced = true;
                        }
                    }
                }
            } else if (tagName == QLatin1String("p")) {
                const QXmlStreamAttributes pAttrs = xml.attributes();
                qint64 pBegin = -1;
                qint64 pEnd = -1;
                const bool hasParagraphBegin = pAttrs.hasAttribute(QLatin1String("begin"));
                if (hasParagraphBegin) {
                    parseTimestampView(pAttrs.value(QLatin1String("begin")), &pBegin);
                }
                bool hasInvalidBegin = hasParagraphBegin && pBegin < 0;
                if (pAttrs.hasAttribute(QLatin1String("end"))) {
                    parseTimestampView(pAttrs.value(QLatin1String("end")), &pEnd);
                }

                const bool pIsBg = (getRoleAttribute(pAttrs) == QLatin1String("x-bg"));
                int nextBgGroupId = 0;
                const int pBgGroup = pIsBg ? nextBgGroupId++ : -1;

                struct ContainerContext {
                    bool isBackground = false;
                    int backgroundGroup = -1;
                    qint64 begin = -1;
                    qint64 end = -1;
                    bool hasChildSpans = false;
                };

                QVector<ContainerContext> contextStack;
                contextStack.append({pIsBg, pBgGroup, -1, -1, false});

                QVector<TtmlChunk> chunks;
                QString directText;

                while (!contextStack.isEmpty() && !xml.atEnd() && !xml.hasError()) {
                    const QXmlStreamReader::TokenType innerToken = xml.readNext();

                    if (innerToken == QXmlStreamReader::StartElement) {
                        const QStringView name = xml.name();
                        if (name == QLatin1String("span")) {
                            const QXmlStreamAttributes spanAttrs = xml.attributes();
                            const QString role = getRoleAttribute(spanAttrs);
                            if (role == QLatin1String("x-translation") || role == QLatin1String("x-roman")) {
                                xml.skipCurrentElement();
                                continue;
                            }

                            contextStack.last().hasChildSpans = true;

                            const bool isBgRole = (role == QLatin1String("x-bg"));
                            const ContainerContext &parent = contextStack.last();
                            const bool isBg = parent.isBackground || isBgRole;
                            int bgGroup = parent.backgroundGroup;
                            if (isBgRole && !parent.isBackground) {
                                bgGroup = nextBgGroupId++;
                                // Always ensure a whitespace separation before background vocals.
                                TtmlChunk ws;
                                ws.isWhitespace = true;
                                chunks.append(ws);
                            }

                            qint64 spanBegin = -1;
                            qint64 spanEnd = -1;
                            const bool hasBeginAttribute = spanAttrs.hasAttribute(QLatin1String("begin"));
                            const bool hasBegin = hasBeginAttribute
                                && parseTimestampView(spanAttrs.value(QLatin1String("begin")), &spanBegin);
                            hasInvalidBegin |= hasBeginAttribute && !hasBegin;
                            const bool hasEnd = spanAttrs.hasAttribute(QLatin1String("end"))
                                && parseTimestampView(spanAttrs.value(QLatin1String("end")), &spanEnd);

                            if (isBgRole) {
                                contextStack.append({isBg, bgGroup, hasBegin ? spanBegin : -1, hasEnd ? spanEnd : -1, false});
                            } else if (hasBegin) {
                                const QString spanContent = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                                TtmlChunk c;
                                c.isTimed = true;
                                c.text = spanContent;
                                c.startMs = spanBegin;
                                c.endMs = hasEnd ? spanEnd : -1;
                                c.isBackground = isBg;
                                c.backgroundGroup = bgGroup;
                                chunks.append(c);
                            } else {
                                // Untimed container span
                                contextStack.append({isBg, bgGroup, -1, -1, false});
                            }
                        } else if (name == QLatin1String("br")) {
                            TtmlChunk ws;
                            ws.isWhitespace = true;
                            chunks.append(ws);
                            directText.append(QLatin1Char(' '));
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
                                directText.append(wsText);
                            }
                        } else {
                            const QString text = xml.text().toString();
                            const ContainerContext &ctx = contextStack.last();
                            if (ctx.begin >= 0 && !ctx.hasChildSpans) {
                                TtmlChunk c;
                                c.isTimed = true;
                                c.text = text;
                                c.startMs = ctx.begin;
                                c.endMs = ctx.end;
                                c.isBackground = ctx.isBackground;
                                c.backgroundGroup = ctx.backgroundGroup;
                                chunks.append(c);
                            } else {
                                TtmlChunk c;
                                c.isTimed = false;
                                c.text = text;
                                c.isBackground = ctx.isBackground;
                                c.backgroundGroup = ctx.backgroundGroup;
                                chunks.append(c);
                                directText += text;
                            }
                        }
                    } else if (innerToken == QXmlStreamReader::EndElement) {
                        contextStack.pop_back();
                        if (contextStack.isEmpty()) {
                            break;
                        }
                        TtmlChunk ws;
                        ws.isWhitespace = true;
                        chunks.append(ws);
                    }
                }
                if (!contextStack.isEmpty()) {
                    // A damaged final paragraph can still follow a usable prefix.
                    break;
                }

                LyricLine line;
                line.isBackground = pIsBg;
                line.startMs = qMax<qint64>(0, pBegin);
                line.endMs = pEnd > line.startMs ? pEnd : line.startMs;

                QString built;
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

                        const bool shouldInsertSpace = calculateShouldInsertSpace(pendingSpace, leadingWs, built, trimmed);

                        if (shouldInsertSpace) {
                            if (!built.isEmpty() && !built.endsWith(QLatin1Char(' '))) {
                                built.append(QLatin1Char(' '));
                            }
                        }

                        LyricWord w;
                        w.text = trimmed;
                        w.startMs = c.startMs >= 0 ? c.startMs : line.startMs;
                        w.endMs = c.endMs > w.startMs ? c.endMs : w.startMs;
                        w.textStart = static_cast<int>(built.length());
                        built.append(trimmed);
                        w.textEnd = static_cast<int>(built.length());
                        w.isBackground = c.isBackground;
                        w.backgroundGroup = c.backgroundGroup;
                        line.words.append(w);

                        pendingSpace = (trailingWs > 0);
                    } else if (!c.text.trimmed().isEmpty()) {
                        const QString trimmed = c.text.trimmed();
                        const bool shouldInsertSpace = calculateShouldInsertSpace(pendingSpace, 0, built, trimmed);

                        if (shouldInsertSpace) {
                            if (!built.isEmpty() && !built.endsWith(QLatin1Char(' '))) {
                                built.append(QLatin1Char(' '));
                            }
                        }
                        built.append(trimmed);
                        pendingSpace = c.text.endsWith(QLatin1Char(' '));
                    }
                }
                if (line.words.isEmpty()) {
                    if (pBegin < 0 && hasInvalidBegin) {
                        continue;
                    }
                    line.text = directText.trimmed();
                } else {
                    line.text = built;
                    if (pBegin < 0) {
                        line.startMs = line.words.first().startMs;
                        for (const auto &word : line.words) {
                            line.startMs = qMin(line.startMs, word.startMs);
                        }
                    }
                    if (pEnd > line.startMs) {
                        line.endMs = pEnd;
                    } else {
                        line.endMs = line.startMs;
                        for (const auto &word : line.words) {
                            line.endMs = qMax(line.endMs, word.endMs);
                        }
                    }

                    // Compute groupStart / groupEnd
                    int groupStart = 0;
                    for (int i = 0; i < line.words.size(); ++i) {
                        if (i == groupStart) {
                            int groupEndIdx = i;
                            while (groupEndIdx + 1 < line.words.size()
                                   && line.words.at(groupEndIdx).textEnd == line.words.at(groupEndIdx + 1).textStart
                                   && !containsWideScript(line.words.at(groupEndIdx).text)) {
                                ++groupEndIdx;
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

                if (!line.text.isEmpty() && !isCreditOrNotice(line.text)) {
                    hasAnyWordTimings |= !line.words.isEmpty();
                    if (pBegin >= 0 || !line.words.isEmpty()) {
                        ++timedLinesCount;
                    }
                    parsedLines.append(line);
                }
            }
        }
    }

    if (parsedLines.isEmpty()) {
        lyrics->lines.clear();
        lyrics->type = LyricsType::None;
        return false;
    }

    if (!LyricsLimits::validateLyricLines(parsedLines)) {
        lyrics->lines.clear();
        lyrics->type = LyricsType::None;
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return false;
    }

    bool hasAdvancingTimestamps = false;
    for (const auto &l : parsedLines) {
        if (l.startMs > 0) {
            hasAdvancingTimestamps = true;
            break;
        }
    }

    const bool isUnsynced = isExplicitlyUnsynced
        || (timedLinesCount == 0)
        || (parsedLines.size() > 1 && !hasAdvancingTimestamps && !hasAnyWordTimings);

    if (isUnsynced) {
        lyrics->lines = parsedLines;
        lyrics->type = LyricsType::Static;
        return false;
    }

    // Sort lines by start time
    std::stable_sort(parsedLines.begin(), parsedLines.end(), [](const LyricLine &a, const LyricLine &b) {
        return a.startMs < b.startMs;
    });

    if (trackDurationMs > 0 && parsedLines.last().startMs > trackDurationMs) {
        const qint64 allowedOverrun = (trackDurationMs / 100) * 15 + (trackDurationMs % 100) * 15 / 100;
        if (parsedLines.last().startMs - trackDurationMs > allowedOverrun) {
            return false;
        }
    }

    // Ensure each line has a valid endMs
    for (int i = 0; i < parsedLines.size(); ++i) {
        LyricLine &line = parsedLines[i];
        if (line.endMs <= line.startMs) {
            if (i + 1 < parsedLines.size() && parsedLines.at(i + 1).startMs > line.startMs) {
                line.endMs = parsedLines.at(i + 1).startMs;
            } else if (trackDurationMs > line.startMs) {
                line.endMs = qMin(trackDurationMs, endWithFallbackSpan(line.startMs, 5000));
            } else {
                line.endMs = endWithFallbackSpan(line.startMs, 3500);
            }
        }
        // If line has words but words have missing endMs, fix them up
        for (int w = 0; w < line.words.size(); ++w) {
            LyricWord &word = line.words[w];
            if (word.endMs <= word.startMs) {
                if (w + 1 < line.words.size() && line.words.at(w + 1).startMs > word.startMs) {
                    word.endMs = line.words.at(w + 1).startMs;
                } else {
                    word.endMs = qMax(word.startMs, line.endMs);
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
