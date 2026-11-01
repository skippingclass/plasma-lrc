/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "yrcparser.h"
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

static bool isCjkChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3040 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x4DBF)
        || (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
}

static bool isApostropheChar(QChar c)
{
    return c == QLatin1Char('\'') || c == QChar(0x2019) || c == QChar(0x02BC) || c == QChar(0xFF07);
}

static bool isContractionSuffix(const QString &text)
{
    QString s = text.trimmed();
    if (s.isEmpty()) {
        return false;
    }
    if (isApostropheChar(s.front())) {
        s = s.mid(1);
    }
    while (!s.isEmpty() && (s.back() == QLatin1Char(',') || s.back() == QLatin1Char('.')
                            || s.back() == QLatin1Char('!') || s.back() == QLatin1Char('?')
                            || s.back() == QLatin1Char(';') || s.back() == QLatin1Char(':'))) {
        s.chop(1);
    }
    const QString lower = s.toLower();
    return (lower == QLatin1String("t")
            || lower == QLatin1String("s")
            || lower == QLatin1String("d")
            || lower == QLatin1String("m")
            || lower == QLatin1String("re")
            || lower == QLatin1String("ve")
            || lower == QLatin1String("ll"));
}

static bool isLeadingSlangApostropheWord(const QString &text)
{
    QString s = text.trimmed();
    if (s.isEmpty()) {
        return false;
    }
    if (isApostropheChar(s.front())) {
        s = s.mid(1);
    }
    while (!s.isEmpty() && (s.back() == QLatin1Char(',') || s.back() == QLatin1Char('.')
                            || s.back() == QLatin1Char('!') || s.back() == QLatin1Char('?')
                            || s.back() == QLatin1Char(';') || s.back() == QLatin1Char(':'))) {
        s.chop(1);
    }
    const QString lower = s.toLower();
    return (lower == QLatin1String("em")
            || lower == QLatin1String("cause")
            || lower == QLatin1String("cuz")
            || lower == QLatin1String("bout")
            || lower == QLatin1String("til")
            || lower == QLatin1String("till")
            || lower == QLatin1String("round")
            || lower == QLatin1String("n"));
}

enum class StandaloneQuoteRole {
    None,
    Opening,
    Closing,
};

template<typename TokenList>
static QVector<StandaloneQuoteRole> classifyQuoteRoles(const TokenList &tokens)
{
    QVector<StandaloneQuoteRole> roles(tokens.size(), StandaloneQuoteRole::None);
    bool inQuotes = false;
    for (int i = 0; i < tokens.size(); ++i) {
        const QString trimmed = tokens[i].text.trimmed();
        if (trimmed == QLatin1String("\"")) {
            if (!inQuotes) {
                const bool hasWordBefore = (i > 0 && !tokens[i - 1].text.trimmed().isEmpty()
                                            && tokens[i - 1].text.trimmed().back().isLetterOrNumber());
                const bool isLastToken = (i == tokens.size() - 1);
                if (isLastToken && hasWordBefore) {
                    roles[i] = StandaloneQuoteRole::Closing;
                    inQuotes = false;
                } else {
                    roles[i] = StandaloneQuoteRole::Opening;
                    inQuotes = true;
                }
            } else {
                roles[i] = StandaloneQuoteRole::Closing;
                inQuotes = false;
            }
        } else {
            for (const QChar c : trimmed) {
                if (c == QLatin1Char('"')) {
                    inQuotes = !inQuotes;
                }
            }
        }
    }
    return roles;
}

static bool isClosingPunctuation(QChar c)
{
    return c == QChar(0x201D) // ”
        || c == QChar(0x00BB) // »
        || c == QLatin1Char(')')
        || c == QLatin1Char(']')
        || c == QLatin1Char('}')
        || c == QChar(0xFF09); // ）
}

static bool isOpeningPunctuation(QChar c)
{
    return c == QChar(0x201C) // “
        || c == QChar(0x00AB) // «
        || c == QLatin1Char('(')
        || c == QLatin1Char('[')
        || c == QLatin1Char('{')
        || c == QChar(0xFF08); // （
}

static bool shouldAddSpaceBetweenTokens(const QString &tokText,
                                        const QString &nextTokText,
                                        const QString &afterNextTokText = QString(),
                                        const QString &prevTokText = QString(),
                                        StandaloneQuoteRole currentQuoteRole = StandaloneQuoteRole::None,
                                        StandaloneQuoteRole nextQuoteRole = StandaloneQuoteRole::None)
{
    if (tokText.isEmpty() || nextTokText.isEmpty()) {
        return false;
    }
    if (tokText.endsWith(QLatin1Char(' ')) || nextTokText.startsWith(QLatin1Char(' '))) {
        return false;
    }

    const QChar lastC = tokText.back();
    const QChar firstC = nextTokText.front();

    if (isCjkChar(lastC) || isCjkChar(firstC)) {
        return false;
    }

    const QString trimmedTok = tokText.trimmed();
    const QString trimmedNext = nextTokText.trimmed();
    const QString trimmedPrev = prevTokText.trimmed();
    const QString trimmedAfter = afterNextTokText.trimmed();

    const bool tokIsStandaloneApostrophe = (trimmedTok.length() == 1 && isApostropheChar(trimmedTok.front()));
    const bool nextIsStandaloneApostrophe = (trimmedNext.length() == 1 && isApostropheChar(trimmedNext.front()));

    // 1. Standalone double quote handling
    if (currentQuoteRole == StandaloneQuoteRole::Opening) {
        return false;
    }
    if (nextQuoteRole == StandaloneQuoteRole::Closing) {
        return false;
    }
    if (currentQuoteRole == StandaloneQuoteRole::Closing) {
        if (firstC.isLetterOrNumber() || isOpeningPunctuation(firstC) || isApostropheChar(firstC)
            || nextQuoteRole == StandaloneQuoteRole::Opening) {
            return true;
        }
        return false;
    }
    if (nextQuoteRole == StandaloneQuoteRole::Opening) {
        if (lastC.isLetterOrNumber() || isClosingPunctuation(lastC) || lastC == QLatin1Char(',')
            || lastC == QLatin1Char(';') || lastC == QLatin1Char(':')) {
            return true;
        }
        return false;
    }

    // 2. Standalone apostrophe as current token:
    //    e.g. "can" + "'" + "t" -> no space after "'"
    //    e.g. "Fuck" + "'" + "em" -> no space after "'"
    //    e.g. "fuckin" + "'" + "God" -> space after "'": it's a trailing apostrophe on "fuckin'"
    //    e.g. "'" + "Hello" -> no space (opening single quote at start)
    if (tokIsStandaloneApostrophe) {
        if (!firstC.isLetterOrNumber()) {
            return false;
        }
        if (isContractionSuffix(trimmedNext) || isLeadingSlangApostropheWord(trimmedNext)) {
            return false;
        }
        if (!trimmedPrev.isEmpty() && trimmedPrev.back().isLetterOrNumber()) {
            return true;
        }
        return false;
    }

    // 3. Next token is a standalone apostrophe:
    //    e.g. "Fuck" + "'" + "em" -> add space before "'" (since "'em" is leading slang)
    //    e.g. "can" + "'" + "t" -> no space
    //    e.g. "fuckin" + "'" + "God" -> no space before "'"
    if (nextIsStandaloneApostrophe) {
        if (lastC.isLetterOrNumber() && isLeadingSlangApostropheWord(trimmedAfter)) {
            return true;
        }
        return false;
    }

    // 4. Non-standalone quote attached to token:
    //    e.g. "Hello\"" + "world" (closing quote attached to token)
    if (lastC == QLatin1Char('"') && trimmedTok.length() > 1) {
        if (firstC.isLetterOrNumber() || isOpeningPunctuation(firstC) || isApostropheChar(firstC)
            || nextQuoteRole == StandaloneQuoteRole::Opening) {
            return true;
        }
    }
    //    e.g. "said" + "\"Hello\"" (opening quote attached to next token)
    if (firstC == QLatin1Char('"') && trimmedNext.length() > 1) {
        if (lastC.isLetterOrNumber() || isClosingPunctuation(lastC) || lastC == QLatin1Char(',')
            || lastC == QLatin1Char(';') || lastC == QLatin1Char(':')) {
            return true;
        }
    }

    // 5. Token ends with an apostrophe: "fuckin'" + "God", "girls'" + "night", "holdin'" + "on"
    if (isApostropheChar(lastC)) {
        if (!isContractionSuffix(trimmedNext) && (firstC.isLetterOrNumber() || isOpeningPunctuation(firstC)
                                                  || firstC == QLatin1Char('"') || nextQuoteRole == StandaloneQuoteRole::Opening)) {
            return true;
        }
    }

    // 6. Next token begins with an apostrophe (multi-character like "'em", "'cause"):
    //    "Fuck" + "'em" -> space
    //    "you" + "'re" -> no space
    if (isApostropheChar(firstC) && lastC.isLetterOrNumber()) {
        if (trimmedNext.length() > 1 && !isContractionSuffix(trimmedNext)) {
            return true;
        }
    }

    // 7. Normal word to word: "Hello" + "world"
    if (lastC.isLetterOrNumber() && firstC.isLetterOrNumber()) {
        return true;
    }

    // 8. Word before opening punctuation: "word" + "(backing)"
    if (lastC.isLetterOrNumber() && isOpeningPunctuation(firstC)) {
        return true;
    }

    // 9. Closing quote or closing bracket on previous token: "intro)" + "word"
    if (isClosingPunctuation(lastC)) {
        if (firstC.isLetterOrNumber() || isOpeningPunctuation(firstC) || isApostropheChar(firstC)) {
            return true;
        }
    }

    // 10. Comma or standard punctuation on previous token: "Hello," + "world" or "Wait!" + "Don't"
    if (lastC == QLatin1Char(',') || lastC == QLatin1Char(';') || lastC == QLatin1Char(':')
        || lastC == QLatin1Char('!') || lastC == QLatin1Char('?') || lastC == QLatin1Char('.')
        || lastC == QChar(0xFF0C)) {
        if (firstC.isLetterOrNumber() || isApostropheChar(firstC) || isOpeningPunctuation(firstC)
            || firstC == QLatin1Char('"') || nextQuoteRole == StandaloneQuoteRole::Opening) {
            return true;
        }
    }

    return false;
}

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
