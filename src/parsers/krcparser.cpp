/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "krcparser.h"
#include "ttmlparser.h"
#include "zlibcodec.h"

#include <QRegularExpression>
#include <QStringList>

namespace KrcParser {

static const unsigned char kKrcXorKey[16] = {
    0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
    0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69
};

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

QString decode(const QByteArray &rawOrBase64Data)
{
    if (rawOrBase64Data.isEmpty()) {
        return QString();
    }

    QByteArray data;
    // Check if input is base64 (doesn't start with krc1 magic)
    if (!rawOrBase64Data.startsWith("krc1")) {
        data = QByteArray::fromBase64(rawOrBase64Data);
    }
    if (data.isEmpty() && rawOrBase64Data.startsWith("krc1")) {
        data = rawOrBase64Data;
    }

    // Skip 4-byte magic "krc1" if present
    if (data.startsWith("krc1")) {
        data = data.mid(4);
    }

    if (data.isEmpty()) {
        return QString();
    }

    // XOR decrypt
    QByteArray decrypted(data.size(), Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i) {
        decrypted[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ kKrcXorKey[i % 16]);
    }

    // Decompress via zlib
    const QByteArray inflated = ZlibCodec::decompress(decrypted);
    if (inflated.isEmpty()) {
        return QString();
    }

    return QString::fromUtf8(inflated);
}

QString toTtml(const QString &krcText)
{
    if (krcText.isEmpty()) {
        return QString();
    }

    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression wordRe(QStringLiteral("<(\\d+),(\\d+)(?:,\\d+)?>([^<]*)"));

    QStringList paragraphs;
    const QStringList lines = krcText.split(QLatin1Char('\n'));

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = lineRe.match(line);
        if (!m.hasMatch()) {
            continue;
        }

        const qint64 lineStart = m.captured(1).toLongLong();
        const qint64 lineDur = m.captured(2).toLongLong();
        const QString rest = m.captured(3);

        QStringList spans;
        QRegularExpressionMatchIterator it = wordRe.globalMatch(rest);
        while (it.hasNext()) {
            const QRegularExpressionMatch wm = it.next();
            const qint64 off = wm.captured(1).toLongLong();
            const qint64 len = wm.captured(2).toLongLong();
            const QString wordText = wm.captured(3);
            if (wordText.isEmpty()) {
                continue;
            }

            const qint64 wordStart = lineStart + off;
            const qint64 wordEnd = wordStart + len;
            spans.append(QStringLiteral("<span begin=\"%1\" end=\"%2\">%3</span>")
                             .arg(formatSeconds(wordStart), formatSeconds(wordEnd), escapeXml(wordText)));
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

bool parse(const QByteArray &rawOrBase64Data, Lyrics *outLyrics)
{
    const QString decoded = decode(rawOrBase64Data);
    if (decoded.isEmpty()) {
        return false;
    }
    const QString ttml = toTtml(decoded);
    if (ttml.isEmpty()) {
        return false;
    }
    return TtmlParser::parseTtml(ttml, outLyrics);
}

} // namespace KrcParser
