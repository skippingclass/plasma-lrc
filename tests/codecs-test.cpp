/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "krcparser.h"
#include "yrcparser.h"
#include "qrcdecoder.h"
#include "zlibcodec.h"

#include <iostream>
#include <QString>
#include <cstring>
#include <zlib.h>

namespace
{
int s_failed = 0;

void check(const char *name, bool condition, const QString &detail = QString())
{
    if (condition) {
        std::cout << name << " ok\n";
    } else {
        std::cout << name << " FAILED";
        if (!detail.isEmpty()) {
            std::cout << " (" << detail.toStdString() << ")";
        }
        std::cout << "\n";
        ++s_failed;
    }
}

QByteArray compressZlib(const QByteArray &data)
{
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    deflateInit(&strm, Z_DEFAULT_COMPRESSION);

    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = static_cast<uInt>(data.size());

    QByteArray out;
    char buffer[4096];
    do {
        strm.next_out = reinterpret_cast<Bytef *>(buffer);
        strm.avail_out = sizeof(buffer);
        deflate(&strm, Z_FINISH);
        out.append(buffer, static_cast<int>(sizeof(buffer) - strm.avail_out));
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
    return out;
}
} // namespace

int main()
{
    // 1. ZlibCodec
    {
        const QByteArray original = "The quick brown fox jumps over the lazy dog. Plasma LRC test.";
        const QByteArray compressed = compressZlib(original);
        const QByteArray decompressed = ZlibCodec::decompress(compressed);
        check("zlib: roundtrip", decompressed == original, QString::fromUtf8(decompressed));
    }

    // 2. KrcParser
    {
        const QString krc = QStringLiteral(
            "[1000,4000]<0,1000,0>Hello <1000,2000,0>World\n"
            "[6000,3000]<0,1500,0>Second <1500,1000,0>Line\n");

        const QString ttml = KrcParser::toTtml(krc);
        check("krc: toTtml not empty", !ttml.isEmpty());
        check("krc: contains hello", ttml.contains(QStringLiteral("Hello")));

        // Compress and encrypt into KRC format
        const QByteArray plainBytes = krc.toUtf8();
        const QByteArray deflated = compressZlib(plainBytes);
        static const unsigned char kKrcXorKey[16] = {
            0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
            0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69
        };
        QByteArray encrypted(deflated.size(), Qt::Uninitialized);
        for (int i = 0; i < deflated.size(); ++i) {
            encrypted[i] = static_cast<char>(static_cast<unsigned char>(deflated[i]) ^ kKrcXorKey[i % 16]);
        }
        QByteArray fullKrc = "krc1" + encrypted;

        Lyrics lyrics;
        const bool parsed = KrcParser::parse(fullKrc.toBase64(), &lyrics);
        check("krc: parse full krc payload", parsed);
        check("krc: syllable sync type", lyrics.type == LyricsType::Syllable);
        check("krc: line count", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            check("krc: line 1 text", lyrics.lines[0].text == QStringLiteral("Hello World"), lyrics.lines[0].text);
            check("krc: line 1 words count", lyrics.lines[0].words.size() == 2);
            check("krc: word 1 start", lyrics.lines[0].words[0].startMs == 1000);
            check("krc: word 1 end", lyrics.lines[0].words[0].endMs == 2000);
            check("krc: word 2 start", lyrics.lines[0].words[1].startMs == 2000);
            check("krc: word 2 end", lyrics.lines[0].words[1].endMs == 4000);
        }
    }

    // 3. YrcParser
    {
        const QString yrc = QStringLiteral(
            "[ti:Test Song]\n"
            "[ar:Artist]\n"
            "[1500,4500](1500,1000,0)Good (2500,2000,0)morning (4500,1000,0)world\n"
            "[7000,3000](7000,1500,0)Plasma (8500,1500,0)LRC\n");

        Lyrics lyrics;
        const bool ok = YrcParser::parse(yrc, &lyrics);
        check("yrc: parse ok", ok);
        check("yrc: type is syllable", lyrics.type == LyricsType::Syllable);
        check("yrc: 2 lines", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            check("yrc: line 0 text", lyrics.lines[0].text == QStringLiteral("Good morning world"), lyrics.lines[0].text);
            check("yrc: line 0 startMs", lyrics.lines[0].startMs == 1500);
            check("yrc: line 0 words count", lyrics.lines[0].words.size() == 3);
            check("yrc: line 0 word 1 start", lyrics.lines[0].words[1].startMs == 2500);
        }
    }

    // 4. QrcDecoder
    {
        const QString qrc = QStringLiteral(
            "[offset:100]\n"
            "[2000,5000]Testing(2000,1000) QQ(3000,1500) Music(4500,2000)\n");

        Lyrics lyrics;
        const bool ok = QrcDecoder::parse(qrc, &lyrics);
        check("qrc: parse plain qrc text", ok);
        check("qrc: type is syllable", lyrics.type == LyricsType::Syllable);
        check("qrc: 1 line", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 1) {
            // with offset 100
            check("qrc: line start with offset", lyrics.lines[0].startMs == 2100, QString::number(lyrics.lines[0].startMs));
            check("qrc: words count", lyrics.lines[0].words.size() == 3);
            check("qrc: word 0 text", lyrics.lines[0].words[0].text == QStringLiteral("Testing"));
            check("qrc: word 0 start with offset", lyrics.lines[0].words[0].startMs == 2100);
        }
    }

    if (s_failed > 0) {
        std::cerr << s_failed << " tests failed\n";
        return 1;
    }

    std::cout << "All codec tests passed.\n";
    return 0;
}
