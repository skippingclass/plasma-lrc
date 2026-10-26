/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "qrcdecoder.h"
#include "ttmlparser.h"
#include "zlibcodec.h"

#include <QRegularExpression>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace QrcDecoder {

static const char kQrcKey[24] = {
    '!', '@', '#', ')', '(', '*', '$', '%',
    '1', '2', '3', 'Z', 'X', 'C', '!', '@',
    '!', '@', '#', ')', '(', 'N', 'H', 'L'
};

static const int SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

static const int KEY_C[28] = {
    56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35
};

static const int KEY_D[28] = {
    62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 60, 52, 44, 36, 28, 20, 12, 4, 27, 19, 11, 3
};

static const int KEY_COMP[48] = {
    13, 16, 10, 23, 0, 4, 2, 27, 14, 5, 20, 9, 22, 18, 11, 3, 25, 7, 15, 6, 26, 19, 12, 1,
    40, 51, 30, 36, 46, 54, 29, 39, 50, 44, 32, 47, 43, 48, 38, 55, 33, 52, 45, 41, 49, 35, 28, 31
};

static const int IPL[32] = {
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};

static const int IPR[32] = {
    56, 48, 40, 32, 24, 16, 8, 0, 58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6
};

static const std::pair<int, int> PPAIRS[32] = {
    {15, 0}, {6, 1}, {19, 2}, {20, 3}, {28, 4}, {11, 5}, {27, 6}, {16, 7},
    {0, 8}, {14, 9}, {22, 10}, {25, 11}, {4, 12}, {17, 13}, {30, 14}, {9, 15},
    {1, 16}, {7, 17}, {23, 18}, {13, 19}, {31, 20}, {26, 21}, {2, 22}, {8, 23},
    {18, 24}, {12, 25}, {29, 26}, {5, 27}, {21, 28}, {10, 29}, {3, 30}, {24, 31}
};

static const int INV_ORDER[8] = {3, 2, 1, 0, 7, 6, 5, 4};

static const int SBOX[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,15,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,10,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

static inline uint32_t bitNum(const uint8_t *bytes, int b, int c)
{
    const int index = (b / 32) * 4 + 3 - (b % 32) / 8;
    return (((static_cast<uint32_t>(bytes[index])) >> (7 - (b % 8))) & 1U) << c;
}

static inline uint32_t bitR(uint32_t value, int b, int c)
{
    return ((value >> (31 - b)) & 1U) << c;
}

static inline uint32_t bitL(uint32_t value, int b, int c)
{
    return ((value << b) & 0x80000000U) >> c;
}

static inline int sboxBit(int value)
{
    return (value & 0x20) | ((value & 0x1f) >> 1) | ((value & 1) << 4);
}

static void donorSchedule(const uint8_t *key, bool encrypt, uint8_t schedule[16][6])
{
    std::memset(schedule, 0, 16 * 6);
    uint32_t c = 0;
    uint32_t d = 0;
    for (int i = 0; i < 28; ++i) {
        c |= bitNum(key, KEY_C[i], 31 - i);
        d |= bitNum(key, KEY_D[i], 31 - i);
    }

    for (int round = 0; round < 16; ++round) {
        const int shift = SHIFTS[round];
        c = ((c << shift) | (c >> (28 - shift))) & 0xfffffff0U;
        d = ((d << shift) | (d >> (28 - shift))) & 0xfffffff0U;

        const int target = encrypt ? round : 15 - round;
        for (int j = 0; j < 24; ++j) {
            schedule[target][j >> 3] |= static_cast<uint8_t>(bitR(c, KEY_COMP[j], 7 - (j & 7)));
        }
        for (int j = 24; j < 48; ++j) {
            schedule[target][j >> 3] |= static_cast<uint8_t>(bitR(d, KEY_COMP[j] - 27, 7 - (j & 7)));
        }
    }
}

static uint32_t donorF(uint32_t state, const uint8_t *key)
{
    uint32_t t1 = bitL(state, 31, 0) | ((state & 0xf0000000U) >> 1) | bitL(state, 4, 5) | bitL(state, 3, 6)
        | ((state & 0x0f000000U) >> 3) | bitL(state, 8, 11) | bitL(state, 7, 12) | ((state & 0x00f00000U) >> 5)
        | bitL(state, 12, 17) | bitL(state, 11, 18) | ((state & 0x000f0000U) >> 7) | bitL(state, 16, 23);

    uint32_t t2 = bitL(state, 15, 0) | ((state & 0x0000f000U) << 15) | bitL(state, 20, 5) | bitL(state, 19, 6)
        | ((state & 0x00000f00U) << 13) | bitL(state, 24, 11) | bitL(state, 23, 12) | ((state & 0x000000f0U) << 11)
        | bitL(state, 28, 17) | bitL(state, 27, 18) | ((state & 0x0000000fU) << 9) | bitL(state, 0, 23);

    const int b0 = ((t1 >> 24) ^ key[0]) & 0xff;
    const int b1 = ((t1 >> 16) ^ key[1]) & 0xff;
    const int b2 = ((t1 >> 8) ^ key[2]) & 0xff;
    const int b3 = ((t2 >> 24) ^ key[3]) & 0xff;
    const int b4 = ((t2 >> 16) ^ key[4]) & 0xff;
    const int b5 = ((t2 >> 8) ^ key[5]) & 0xff;

    const uint32_t boxed = (static_cast<uint32_t>(SBOX[0][sboxBit(b0 >> 2)]) << 28)
        | (static_cast<uint32_t>(SBOX[1][sboxBit(((b0 & 3) << 4) | (b1 >> 4))]) << 24)
        | (static_cast<uint32_t>(SBOX[2][sboxBit(((b1 & 15) << 2) | (b2 >> 6))]) << 20)
        | (static_cast<uint32_t>(SBOX[3][sboxBit(b2 & 63)]) << 16)
        | (static_cast<uint32_t>(SBOX[4][sboxBit(b3 >> 2)]) << 12)
        | (static_cast<uint32_t>(SBOX[5][sboxBit(((b3 & 3) << 4) | (b4 >> 4))]) << 8)
        | (static_cast<uint32_t>(SBOX[6][sboxBit(((b4 & 15) << 2) | (b5 >> 6))]) << 4)
        | (static_cast<uint32_t>(SBOX[7][sboxBit(b5 & 63)]));

    uint32_t out = 0;
    for (const auto &pair : PPAIRS) {
        out |= bitL(boxed, pair.first, pair.second);
    }
    return out;
}

static void donorJoin(uint32_t left, uint32_t right, uint8_t *out)
{
    for (int k = 0; k < 8; ++k) {
        const int base = 7 - k;
        uint32_t v = 0;
        for (int t = 0; t < 4; ++t) {
            v |= bitR(right, base + 8 * t, 7 - 2 * t) | bitR(left, base + 8 * t, 6 - 2 * t);
        }
        out[INV_ORDER[k]] = static_cast<uint8_t>(v);
    }
}

static void cryptBlock(const uint8_t *input, int length, const uint8_t *key, bool encrypt, uint8_t *output)
{
    uint8_t schedule[16][6];
    donorSchedule(key, encrypt, schedule);

    for (int offset = 0; offset < length; offset += 8) {
        const uint8_t *block = input + offset;
        uint32_t left = 0;
        uint32_t right = 0;
        for (int i = 0; i < 32; ++i) {
            left |= bitNum(block, IPL[i], 31 - i);
            right |= bitNum(block, IPR[i], 31 - i);
        }

        for (int round = 0; round < 15; ++round) {
            const uint32_t previous = right;
            right = donorF(right, schedule[round]) ^ left;
            left = previous;
        }
        left = donorF(right, schedule[15]) ^ left;

        donorJoin(left, right, output + offset);
    }
}

static QByteArray decrypt3Des(const QByteArray &cipher)
{
    if (cipher.size() % 8 != 0) {
        return QByteArray();
    }

    QByteArray buf1(cipher.size(), Qt::Uninitialized);
    QByteArray buf2(cipher.size(), Qt::Uninitialized);
    QByteArray buf3(cipher.size(), Qt::Uninitialized);

    const auto *k = reinterpret_cast<const uint8_t *>(kQrcKey);
    const auto *in = reinterpret_cast<const uint8_t *>(cipher.constData());

    // 3DES: Decrypt with K3, Encrypt with K2, Decrypt with K1
    cryptBlock(in, cipher.size(), k + 16, false, reinterpret_cast<uint8_t *>(buf1.data()));
    cryptBlock(reinterpret_cast<const uint8_t *>(buf1.constData()), cipher.size(), k + 8, true, reinterpret_cast<uint8_t *>(buf2.data()));
    cryptBlock(reinterpret_cast<const uint8_t *>(buf2.constData()), cipher.size(), k, false, reinterpret_cast<uint8_t *>(buf3.data()));

    return buf3;
}

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

QString decryptHex(const QString &hexStr)
{
    QString clean;
    clean.reserve(hexStr.size());
    for (const QChar c : hexStr) {
        if (!c.isSpace()) {
            clean.append(c);
        }
    }

    if (clean.size() < 16 || clean.size() % 2 != 0) {
        return QString();
    }

    QByteArray cipher;
    cipher.reserve(clean.size() / 2);
    for (int i = 0; i < clean.size(); i += 2) {
        bool ok = false;
        const int val = clean.mid(i, 2).toInt(&ok, 16);
        if (!ok) {
            return QString();
        }
        cipher.append(static_cast<char>(val));
    }

    const QByteArray plainBytes = decrypt3Des(cipher);
    if (plainBytes.isEmpty()) {
        return QString();
    }

    const QByteArray inflated = ZlibCodec::decompress(plainBytes);
    if (inflated.isEmpty()) {
        return QString();
    }

    return QString::fromUtf8(inflated);
}

QString toTtml(const QString &qrcText)
{
    if (qrcText.isEmpty()) {
        return QString();
    }

    QString content = qrcText;
    static const QRegularExpression xmlContentRe(QStringLiteral("LyricContent=\"(.*?)\""), QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch xmlMatch = xmlContentRe.match(content);
    if (xmlMatch.hasMatch()) {
        content = xmlMatch.captured(1);
        content.replace(QLatin1String("&quot;"), QLatin1String("\""));
        content.replace(QLatin1String("&#39;"), QLatin1String("'"));
        content.replace(QLatin1String("&lt;"), QLatin1String("<"));
        content.replace(QLatin1String("&gt;"), QLatin1String(">"));
        content.replace(QLatin1String("&amp;"), QLatin1String("&"));
    }

    static const QRegularExpression offsetRe(QStringLiteral("^\\[offset:(-?\\d+)\\]"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+),(\\d+)\\](.*)$"));
    static const QRegularExpression tokenRe(QStringLiteral("(.*?)\\((\\d+),(\\d+)\\)"));

    qint64 offset = 0;
    QStringList paragraphs;
    const QStringList lines = content.split(QLatin1Char('\n'));

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch offM = offsetRe.match(line);
        if (offM.hasMatch()) {
            offset = offM.captured(1).toLongLong();
            continue;
        }

        const QRegularExpressionMatch lm = lineRe.match(line);
        if (!lm.hasMatch()) {
            continue;
        }

        const qint64 start = qMax<qint64>(0, lm.captured(1).toLongLong() + offset);
        const qint64 duration = lm.captured(2).toLongLong();
        const QString rest = lm.captured(3);

        QString fullText;
        struct QrcTok {
            qint64 start;
            qint64 dur;
            QString text;
        };
        QVector<QrcTok> tokens;

        QRegularExpressionMatchIterator it = tokenRe.globalMatch(rest);
        while (it.hasNext()) {
            const QRegularExpressionMatch tm = it.next();
            const QString text = tm.captured(1);
            if (!text.trimmed().isEmpty()) {
                const qint64 wordStart = qMax<qint64>(0, tm.captured(2).toLongLong() + offset);
                const qint64 wordDur = tm.captured(3).toLongLong();
                tokens.append({wordStart, wordDur, text});
                fullText.append(text);
            }
        }

        if (tokens.isEmpty() || TtmlParser::isCreditOrNotice(fullText)) {
            continue;
        }

        QStringList spans;
        for (const auto &tok : tokens) {
            spans.append(QStringLiteral("<span begin=\"%1\" end=\"%2\">%3</span>")
                             .arg(formatSeconds(tok.start), formatSeconds(tok.start + tok.dur), escapeXml(tok.text)));
        }

        if (!spans.isEmpty()) {
            paragraphs.append(QStringLiteral("<p begin=\"%1\" end=\"%2\">%3</p>")
                                  .arg(formatSeconds(start), formatSeconds(start + duration), spans.join(QString())));
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

bool parse(const QString &hexOrQrcText, Lyrics *outLyrics)
{
    QString qrc = hexOrQrcText.trimmed();
    if (!qrc.startsWith(QLatin1Char('[')) && !qrc.startsWith(QLatin1Char('<'))) {
        qrc = decryptHex(qrc);
    }
    if (qrc.isEmpty()) {
        return false;
    }
    const QString ttml = toTtml(qrc);
    if (ttml.isEmpty()) {
        return false;
    }
    return TtmlParser::parseTtml(ttml, outLyrics);
}

} // namespace QrcDecoder
