/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "yrcparser.h"
#include "ttmlparser.h"

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

static bool isCreditOrNotice(const QString &text)
{
    const QString t = text.trimmed();
    if (t.contains(QStringLiteral("作词")) || t.contains(QStringLiteral("作曲"))
        || t.contains(QStringLiteral("编曲")) || t.contains(QStringLiteral("制作人"))
        || t.contains(QStringLiteral("版权")) || t.contains(QStringLiteral("纯音乐"))) {
        return true;
    }
    return false;
}

QString toTtml(const QString &yrcText)
{
    if (yrcText.isEmpty()) {
        return QString();
    }

    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression wordRe(QStringLiteral("\\((\\d+),(\\d+),[^)]*\\)([^()]*)"));

    QStringList paragraphs;
    const QStringList lines = yrcText.split(QLatin1Char('\n'));

    for (const QString &rawLine : lines) {
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
            if (!text.isEmpty()) {
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
        if (isCreditOrNotice(fullText)) {
            continue;
        }

        // Relative slack: check if words timestamps are relative to line start or absolute
        constexpr qint64 kRelativeSlackMs = 1000;
        const qint64 base = (tokens.first().at + kRelativeSlackMs < lineStart) ? lineStart : 0;

        QStringList spans;
        for (const auto &tok : tokens) {
            const qint64 wordStart = base + tok.at;
            const qint64 wordEnd = wordStart + tok.len;
            spans.append(QStringLiteral("<span begin=\"%1\" end=\"%2\">%3</span>")
                             .arg(formatSeconds(wordStart), formatSeconds(wordEnd), escapeXml(tok.text)));
        }

        if (!spans.isEmpty()) {
            paragraphs.append(QStringLiteral("<p begin=\"%1\" end=\"%2\">%3</p>")
                                  .arg(formatSeconds(lineStart), formatSeconds(lineStart + lineDur), spans.join(QString())));
        }
    }

    if (paragraphs.isEmpty()) {
        return QString();
    }

    return QStringLiteral("<tt xmlns=\"http://www.w3.org/ns/ttml\" "
                          "xmlns:itunes=\"http://music.apple.com/lyric-ttml-internal\" "
                          "itunes:timing=\"word\"><body><div>%1</div></body></tt>")
        .arg(paragraphs.join(QString()));
}

bool parse(const QString &yrcText, Lyrics *outLyrics)
{
    const QString ttml = toTtml(yrcText);
    if (ttml.isEmpty()) {
        return false;
    }
    return TtmlParser::parseTtml(ttml, outLyrics);
}

} // namespace YrcParser
