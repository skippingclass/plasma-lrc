/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "yrcparser.h"
#include "tokenpunctuationhelper.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QRegularExpression>
#include <QStringList>

namespace YrcParser {

static QString escapeXml(const QString &s)
{
    QString res = s;
    res.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    res.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    res.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    res.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return res;
}

static QString formatSeconds(qint64 ms)
{
    return QString::asprintf("%.3fs", ms / 1000.0);
}

using namespace TokenPunctuationHelper;

QString toTtml(const QString &yrcText)
{
    if (yrcText.isEmpty()) {
        return QString();
    }
    if (!LyricsLimits::checkDecodedSize(yrcText.size() * sizeof(QChar))) {
        return QString();
    }

    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression wordRe(QStringLiteral("\\((\\d+),(\\d+),[^)]*\\)([^()]*)"));

    QStringList paragraphs;
    const QStringList lines = yrcText.split(QLatin1Char('\n'));
    if (lines.size() > LyricsLimits::kMaxLines) {
        return QString();
    }

    for (const QString &rawLine : lines) {
        if (!LyricsLimits::checkLineLength(rawLine.size())) {
            return QString();
        }
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = lineRe.match(line);
        if (!m.hasMatch()) {
            continue;
        }

        const qint64 lineStart = m.captured(1).toLongLong();
        const qint64 lineDur = m.captured(2).toLongLong();
        const QString rest = m.captured(3);

        struct WordToken {
            qint64 at = 0;
            qint64 len = 0;
            QString text;
        };
        QVector<WordToken> tokens;

        QRegularExpressionMatchIterator it = wordRe.globalMatch(rest);
        while (it.hasNext()) {
            const QRegularExpressionMatch wm = it.next();
            const qint64 at = wm.captured(1).toLongLong();
            const qint64 len = wm.captured(2).toLongLong();
            const QString text = wm.captured(3);
            if (!text.trimmed().isEmpty()) {
                tokens.append({at, len, text});
            }
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

        // Relative slack: check if words timestamps are relative to line start or absolute
        constexpr qint64 kRelativeSlackMs = 1000;
        const qint64 base = (tokens.first().at + kRelativeSlackMs < lineStart) ? lineStart : 0;

        const bool wholeLineBg = (tokens.size() > 0)
            && (tokens.first().text.trimmed().startsWith(QLatin1Char('('))
                || tokens.first().text.trimmed().startsWith(QChar(0xFF08)))
            && (tokens.last().text.trimmed().endsWith(QLatin1Char(')'))
                || tokens.last().text.trimmed().endsWith(QChar(0xFF09)));

        QStringList spans;
        bool inBgSpan = false;
        for (const auto &tok : tokens) {
            const qint64 wordStart = base + tok.at;
            const qint64 wordEnd = wordStart + tok.len;
            const QString tokTrimmed = tok.text.trimmed();

            const bool openBg = !wholeLineBg && !inBgSpan
                && (tokTrimmed.startsWith(QLatin1Char('(')) || tokTrimmed.startsWith(QChar(0xFF08)));

            if (openBg) {
                inBgSpan = true;
                spans.append(QStringLiteral("<span ttm:role=\"x-bg\">"));
            }

            spans.append(QStringLiteral("<span begin=\"%1\" end=\"%2\">%3</span>")
                             .arg(formatSeconds(wordStart), formatSeconds(wordEnd), escapeXml(tok.text)));

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
                                      .arg(formatSeconds(lineStart), formatSeconds(lineStart + lineDur), spans.join(QString())));
            } else {
                paragraphs.append(QStringLiteral("<p begin=\"%1\" end=\"%2\">%3</p>")
                                      .arg(formatSeconds(lineStart), formatSeconds(lineStart + lineDur), spans.join(QString())));
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
    const QString ttml = toTtml(yrcText);
    if (ttml.isEmpty()) {
        return false;
    }
    return TtmlParser::parseTtml(ttml, outLyrics, trackDurationMs, outLimitExceeded);
}

} // namespace YrcParser
