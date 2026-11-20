/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "yrcparser.h"
#include "timedlyric.h"
#include "tokenpunctuationhelper.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QRegularExpression>
#include <QStringList>

namespace YrcParser {

using TimedLyric::formatSeconds;
using namespace TokenPunctuationHelper;

QString toTtml(const QString &yrcText, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    if (yrcText.isEmpty()) {
        return QString();
    }
    if (!LyricsLimits::checkDecodedSize(static_cast<qint64>(yrcText.size()) * sizeof(QChar))) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return QString();
    }

    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression wordRe(QStringLiteral("\\((\\d+),(\\d+),[^)]*\\)([^()]*)"));

    QStringList paragraphs;
    const QStringList lines = yrcText.split(QLatin1Char('\n'));
    if (lines.size() > LyricsLimits::kMaxLines) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return QString();
    }

    for (const QString &rawLine : lines) {
        if (!LyricsLimits::checkLineLength(rawLine.size())) {
            if (outLimitExceeded) {
                *outLimitExceeded = true;
            }
            return QString();
        }
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = lineRe.match(line);
        if (!m.hasMatch()) {
            continue;
        }

        const auto lineTiming = TimedLyric::parseRange(m.capturedView(1), m.capturedView(2));
        if (!lineTiming) {
            continue;
        }
        const QString rest = m.captured(3);

        struct WordToken {
            TimedLyric::Range timing;
            QString text;
        };
        QVector<WordToken> tokens;
        std::optional<qint64> base;
        constexpr qint64 kRelativeSlackMs = 1000;

        QRegularExpressionMatchIterator it = wordRe.globalMatch(rest);
        while (it.hasNext()) {
            const QRegularExpressionMatch wm = it.next();
            const auto timing = TimedLyric::parseRange(wm.capturedView(1), wm.capturedView(2));
            const QString text = wm.captured(3);
            if (!timing || text.trimmed().isEmpty()) {
                continue;
            }

            // Use the first valid word to distinguish relative and absolute timestamps.
            const qint64 wordBase = base.value_or(lineTiming->startMs - timing->startMs > kRelativeSlackMs ? lineTiming->startMs : 0);
            qint64 wordEnd = 0;
            if (qAddOverflow(wordBase, timing->endMs, &wordEnd)) {
                continue;
            }

            base = wordBase;
            tokens.append({{wordBase + timing->startMs, wordEnd}, text});
        }

        if (tokens.isEmpty()) {
            continue;
        }

        QString fullText;
        for (const auto &tok : tokens) {
            fullText.append(tok.text);
        }
        if (TtmlParser::isCreditOrNotice(fullText)) {
            continue;
        }

        // Ensure adjacent Latin words, punctuation, commas, and apostrophes in YRC have a space if omitted by NetEase
        const auto quoteRoles = classifyQuoteRoles(tokens);
        for (int i = 0; i < tokens.size() - 1; ++i) {
            auto &tok = tokens[i];
            const auto &nextTok = tokens[i + 1];
            const QString afterNext = (i + 2 < tokens.size()) ? tokens[i + 2].text : QString();
            const QString prev = (i > 0) ? tokens[i - 1].text : QString();
            if (shouldAddSpaceBetweenTokens(tok.text, nextTok.text, afterNext, prev, quoteRoles[i], quoteRoles[i + 1])) {
                tok.text.append(QLatin1Char(' '));
            }
        }

        const bool wholeLineBg = (tokens.size() > 0)
            && (tokens.first().text.trimmed().startsWith(QLatin1Char('('))
                || tokens.first().text.trimmed().startsWith(QChar(0xFF08)))
            && (tokens.last().text.trimmed().endsWith(QLatin1Char(')'))
                || tokens.last().text.trimmed().endsWith(QChar(0xFF09)));

        QStringList spans;
        bool inBgSpan = false;
        for (const auto &tok : tokens) {
            const QString tokTrimmed = tok.text.trimmed();

            const bool openBg = !wholeLineBg && !inBgSpan
                && (tokTrimmed.startsWith(QLatin1Char('(')) || tokTrimmed.startsWith(QChar(0xFF08)));

            if (openBg) {
                inBgSpan = true;
                spans.append(QStringLiteral("<span ttm:role=\"x-bg\">"));
            }

            spans.append(QStringLiteral("<span begin=\"%1\" end=\"%2\">%3</span>")
                             .arg(formatSeconds(tok.timing.startMs), formatSeconds(tok.timing.endMs), tok.text.toHtmlEscaped()));

            const bool closeBg = inBgSpan
                && (tokTrimmed.endsWith(QLatin1Char(')')) || tokTrimmed.endsWith(QChar(0xFF09)));
            if (closeBg) {
                inBgSpan = false;
                spans.append(QStringLiteral("</span>"));
            }
        }
        if (inBgSpan) {
            spans.append(QStringLiteral("</span>"));
        }

        if (!spans.isEmpty()) {
            if (wholeLineBg) {
                paragraphs.append(QStringLiteral("<p ttm:role=\"x-bg\" begin=\"%1\" end=\"%2\">%3</p>")
                                      .arg(formatSeconds(lineTiming->startMs), formatSeconds(lineTiming->endMs), spans.join(QString())));
            } else {
                paragraphs.append(QStringLiteral("<p begin=\"%1\" end=\"%2\">%3</p>")
                                      .arg(formatSeconds(lineTiming->startMs), formatSeconds(lineTiming->endMs), spans.join(QString())));
            }
        }
    }

    if (paragraphs.isEmpty()) {
        return QString();
    }

    return QStringLiteral("<tt xmlns=\"http://www.w3.org/ns/ttml\" "
                          "xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" "
                          "xmlns:itunes=\"http://music.apple.com/lyric-ttml-internal\" "
                          "itunes:timing=\"word\"><body><div>%1</div></body></tt>")
        .arg(paragraphs.join(QString()));
}

bool parse(const QString &yrcText, Lyrics *outLyrics, qint64 trackDurationMs, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    bool limitExceeded = false;
    const QString ttml = toTtml(yrcText, &limitExceeded);
    if (limitExceeded) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return false;
    }
    if (ttml.isEmpty()) {
        return false;
    }
    return TtmlParser::parseTtml(ttml, outLyrics, trackDurationMs, outLimitExceeded);
}

} // namespace YrcParser
