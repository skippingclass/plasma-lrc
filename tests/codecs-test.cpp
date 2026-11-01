/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "krcparser.h"
#include "yrcparser.h"
#include "qrcdecoder.h"
#include "lrcparser.h"
#include "ttmlparser.h"
#include "lyricfilter.h"
#include "zlibcodec.h"

#include <iostream>
#include <QString>
#include <QTemporaryDir>
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
    QTemporaryDir testCache;
    QTemporaryDir testConfig;
    qputenv("XDG_CACHE_HOME", testCache.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", testConfig.path().toUtf8());
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

    // 5. YRC CJK Spacing & Pause Tokens (Regression: NetEase pause tokens creating extra spaces)
    {
        const QString yrc = QStringLiteral(
            "[7350,3000](7350,210,0)海(7560,240,0)屿(7800,240,0)你 (8040,150,0)我(8190,210,0)的(8400,300,0)心(8700,1650,0)事\n"
            "[13010,4780](13010,660,0)从(13670,670,0)不(14340,30,0) (14370,480,0)主(14850,610,0)动(15460,30,0) (15490,330,0)示(15820,1070,0)弱(16890,900,0) \n");

        Lyrics lyrics;
        const bool ok = YrcParser::parse(yrc, &lyrics);
        check("yrc cjk: parse ok", ok);
        check("yrc cjk: 2 lines", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            check("yrc cjk: line 0 no extra spaces", lyrics.lines[0].text == QStringLiteral("海屿你我的心事"), lyrics.lines[0].text);
            check("yrc cjk: line 1 no extra spaces", lyrics.lines[1].text == QStringLiteral("从不主动示弱"), lyrics.lines[1].text);
        }
    }

    // 6. YRC English Glued Words & Punctuation
    {
        const QString yrc = QStringLiteral(
            "[17100,2760](17100,30,0)I (17130,180,0)can(17970,30,0)’(18000,150,0)t (18150,270,0)find (18420,540,0)nobody\n"
            "[38430,2130](38430,240,0)I'm(38670,300,0)going (38970,180,0)through (39150,1410,0)withdrawals\n"
            "[41370,2430](41370,240,0)have(42420,240,0)to (42660,270,0)do\n");

        Lyrics lyrics;
        const bool ok = YrcParser::parse(yrc, &lyrics);
        check("yrc en: parse ok", ok);
        check("yrc en: 3 lines", lyrics.lines.size() == 3, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 3) {
            check("yrc en: can’t find", lyrics.lines[0].text == QStringLiteral("I can’t find nobody"), lyrics.lines[0].text);
            check("yrc en: I'm going separated", lyrics.lines[1].text == QStringLiteral("I'm going through withdrawals"), lyrics.lines[1].text);
            check("yrc en: have to separated", lyrics.lines[2].text == QStringLiteral("have to do"), lyrics.lines[2].text);
        }
    }

    // 7. YRC Fullwidth Parentheses Backing Vocals
    {
        const QString yrc = QStringLiteral(
            "[0,11460](0,10860,0)（(10860,60,0)The (10920,240,0)Kid (11160,300,0)LAROI(11460,0,0)）\n"
            "[50760,2880](50760,420,0)Sin (51180,540,0)city's (51720,480,0)cold (52200,330,0)and (52530,870,0)empty (53400,0,0)（(53400,240,0)Oh(53640,0,0)）\n");

        Lyrics lyrics;
        const bool ok = YrcParser::parse(yrc, &lyrics);
        check("yrc bg: parse ok", ok);
        check("yrc bg: 2 lines", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            check("yrc bg: line 0 text", lyrics.lines[0].text == QStringLiteral("（The Kid LAROI）"), lyrics.lines[0].text);
            check("yrc bg: line 0 is whole line background", lyrics.lines[0].isBackground);

            check("yrc bg: line 1 text", lyrics.lines[1].text == QStringLiteral("Sin city's cold and empty （Oh）"), lyrics.lines[1].text);
            check("yrc bg: line 1 not whole line bg", !lyrics.lines[1].isBackground);
            // Check that words inside （Oh） are background
            bool foundBgWord = false;
            for (const auto &w : lyrics.lines[1].words) {
                if (w.text.contains(QStringLiteral("Oh"))) {
                    foundBgWord = true;
                    check("yrc bg: Oh marked as background", w.isBackground && w.backgroundGroup >= 0);
                }
            }
            check("yrc bg: found Oh word", foundBgWord);
        }
    }

    // 8. Credit Lines & Notice Filtering
    {
        const QString yrcWithCredits = QStringLiteral(
            "[0,1000](0,1000,0) 作词 : Billie Eilish/FINNEAS\n"
            "[1000,1000](1000,1000,0) 作曲 : Billie Eilish/FINNEAS\n"
            "[2000,1000](2000,1000,0) 制作人 : FINNEAS\n"
            "[3000,1000](3000,1000,0) 吉他 : 高飞\n"
            "[4000,1000](4000,1000,0) 混音 : 周天澈\n"
            "[5000,1000](5000,1000,0) Mastering Engineer : John Greenham\n"
            "[6000,1000](6000,1000,0) Vocals : Billie Eilish\n"
            "[7000,3000](7000,1500,0)Real (8500,1500,0)Lyrics\n");

        Lyrics lyrics;
        const bool ok = YrcParser::parse(yrcWithCredits, &lyrics);
        check("yrc credits: parse ok", ok);
        check("yrc credits: only real line remains", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 1) {
            check("yrc credits: real line text", lyrics.lines[0].text == QStringLiteral("Real Lyrics"), lyrics.lines[0].text);
        }
    }

    // 9. Pure Music (纯音乐，请欣赏) Instrumental Notices
    {
        // 9a. LRC Pure Music
        const QString lrcPure = QStringLiteral(
            "[id:$00000000]\n"
            "[ar:Artist]\n"
            "[ti:Canon in D]\n"
            "[00:01.58]纯音乐，请欣赏\n");

        Lyrics lrcLyrics;
        const bool lrcParsed = LrcParser::parseLrc(lrcPure, &lrcLyrics);
        check("notice lrc: pure music rejected", !lrcParsed || !lrcLyrics.isUsable());

        // 9b. KRC Pure Music (Kugou)
        const QString krcPure = QStringLiteral(
            "[id:$00000000]\n"
            "[1589,320317]<0,354,0>纯<354,505,0>音<859,406,0>乐<1265,304,0>，<1569,252,0>请<1821,405,0>欣<2226,303,0>赏\n");
        const QString ttmlKrc = KrcParser::toTtml(krcPure);
        check("notice krc: pure music rejected in toTtml", ttmlKrc.isEmpty());

        // 9c. YRC Pure Music
        const QString yrcPure = QStringLiteral("[0,1000](0,1000,0)纯音乐，请欣赏\n");
        Lyrics yrcLyrics;
        const bool yrcParsed = YrcParser::parse(yrcPure, &yrcLyrics);
        check("notice yrc: pure music rejected", !yrcParsed || !yrcLyrics.isUsable());

        // 9d. Detailed LyricFilter checks
        check("filter: 纯音乐，请欣赏", LyricFilter::isCreditOrNotice(QStringLiteral("纯音乐，请欣赏")));
        check("filter: 此歌曲为没有填词的纯音乐，请您欣赏", LyricFilter::isCreditOrNotice(QStringLiteral("此歌曲为没有填词的纯音乐，请您欣赏")));
        check("filter: [Instrumental]", LyricFilter::isCreditOrNotice(QStringLiteral("[Instrumental]")));
        check("filter: Vocals : Artist", LyricFilter::isCreditOrNotice(QStringLiteral("Vocals : Artist")));
        check("filter: 吉他 : 孔朝晖", LyricFilter::isCreditOrNotice(QStringLiteral("吉他 : 孔朝晖")));
        check("filter: real lyric is not notice", !LyricFilter::isCreditOrNotice(QStringLiteral("从不主动示弱")));
        check("filter: real lyric with 制作", !LyricFilter::isCreditOrNotice(QStringLiteral("制作属于我们的回忆")));
    }

    // 10. KRC & QRC Latin Spacing, Backing Vocals (x-bg), and trackDurationMs Validation
    {
        const QString krcGluedAndBg = QStringLiteral(
            "[1000,3000]<0,500,0>Hello<500,500,0>world<1000,1000,0>（<2000,500,0>Oh<2500,500,0>）\n"
            "[5000,2000]<0,500,0>(Backing<500,1500,0>only)\n");
        const QString krcTtml = KrcParser::toTtml(krcGluedAndBg);
        Lyrics krcLyrics;
        const bool krcOk = TtmlParser::parseTtml(krcTtml, &krcLyrics);
        check("krc spacing & bg: parse ok", krcOk && krcLyrics.lines.size() == 2);
        if (krcLyrics.lines.size() == 2) {
            check("krc spacing & bg: line 0 glued words separated",
                  krcLyrics.lines[0].text == QStringLiteral("Hello world （Oh）"), krcLyrics.lines[0].text);
            bool foundKrcBg = false;
            for (const auto &w : krcLyrics.lines[0].words) {
                if (w.text.contains(QStringLiteral("Oh"))) {
                    foundKrcBg = w.isBackground;
                }
            }
            check("krc spacing & bg: inline Oh marked as background", foundKrcBg);
            check("krc spacing & bg: line 1 whole-line background", krcLyrics.lines[1].isBackground);
        }

        const QString qrcGluedAndBg = QStringLiteral(
            "[1000,3000]Hello(1000,500)world(1500,500)(Yeah)(2000,1000)\n");
        Lyrics qrcLyrics;
        const bool qrcOk = QrcDecoder::parse(qrcGluedAndBg, &qrcLyrics);
        check("qrc spacing & bg: parse ok", qrcOk && qrcLyrics.lines.size() == 1);
        if (qrcLyrics.lines.size() == 1) {
            check("qrc spacing & bg: line 0 glued words separated",
                  qrcLyrics.lines[0].text == QStringLiteral("Hello world (Yeah)"), qrcLyrics.lines[0].text);
            check("qrc spacing & bg: (Yeah) marked as background",
                  qrcLyrics.lines[0].words.last().isBackground);
        }

        // QRC punctuation, commas, quotes, and standalone apostrophes spacing
        {
            const QString qrcPunct = QStringLiteral(
                "[1000,4000]Hello,(1000,500)world(1500,500)fuckin'(2000,500)God(2500,500)\n"
                "[5000,4000]Fuck(5000,500)'em(5500,500)you(6000,500)'re(6500,500)it(7000,500)'s(7500,500)\n"
                "[9000,3000]Fuck(9000,300)'(9300,100)em(9400,300)\n"
                "[12000,2000]Hello\"(12000,500)world(12500,500)\n"
                "[14000,2000]can(14000,300)'(14300,100)t(14400,300)\n"
                "[16000,3000]fuckin(16000,300)'(16300,100)God(16400,300)\n"
                "[19000,3000]girls(19000,300)'(19300,100)night(19400,300)\n"
                "[22000,4000]\"(22000,100)Hello(22100,300)\"(22400,100)world(22500,300)\n");
            Lyrics pLyrics;
            const bool pOk = QrcDecoder::parse(qrcPunct, &pLyrics);
            check("qrc punctuation & apostrophe: parse ok", pOk && pLyrics.lines.size() == 8);
            if (pLyrics.lines.size() == 8) {
                check("qrc: space after comma and apostrophe",
                      pLyrics.lines[0].text == QStringLiteral("Hello, world fuckin' God"), pLyrics.lines[0].text);
                check("qrc: space before slang apostrophe, preserve contractions",
                      pLyrics.lines[1].text == QStringLiteral("Fuck 'em you're it's"), pLyrics.lines[1].text);
                check("qrc: standalone apostrophe with slang suffix: Fuck + ' + em -> Fuck 'em",
                      pLyrics.lines[2].text == QStringLiteral("Fuck 'em"), pLyrics.lines[2].text);
                check("qrc: closing quote before word: Hello\" + world -> Hello\" world",
                      pLyrics.lines[3].text == QStringLiteral("Hello\" world"), pLyrics.lines[3].text);
                check("qrc: standalone apostrophe with contraction suffix: can + ' + t -> can't",
                      pLyrics.lines[4].text == QStringLiteral("can't"), pLyrics.lines[4].text);
                check("qrc: standalone trailing apostrophe: fuckin + ' + God -> fuckin' God",
                      pLyrics.lines[5].text == QStringLiteral("fuckin' God"), pLyrics.lines[5].text);
                check("qrc: standalone plural possessive: girls + ' + night -> girls' night",
                      pLyrics.lines[6].text == QStringLiteral("girls' night"), pLyrics.lines[6].text);
                check("qrc: standalone straight quotes: \" + Hello + \" + world -> \"Hello\" world",
                      pLyrics.lines[7].text == QStringLiteral("\"Hello\" world"), pLyrics.lines[7].text);
            }
        }

        // KRC punctuation, quotes, and standalone apostrophes spacing
        {
            const QString krcPunct = QStringLiteral(
                "[1000,3000]<0,300,0>Fuck<300,100,0>'<400,300,0>em\n"
                "[4000,2000]<0,500,0>Hello\"<500,500,0>world\n"
                "[7000,2000]<0,300,0>can<300,100,0>'<400,300,0>t\n"
                "[10000,3000]<0,300,0>fuckin<300,100,0>'<400,300,0>God\n"
                "[13000,3000]<0,300,0>girls<300,100,0>'<400,300,0>night\n"
                "[16000,4000]<0,100,0>\"<100,300,0>Hello<400,100,0>\"<500,300,0>world\n");
            const QString ttml = KrcParser::toTtml(krcPunct);
            Lyrics kLyrics;
            const bool kOk = TtmlParser::parseTtml(ttml, &kLyrics);
            check("krc punct & apostrophe: parse ok", kOk && kLyrics.lines.size() == 6);
            if (kLyrics.lines.size() == 6) {
                check("krc: Fuck + ' + em -> Fuck 'em",
                      kLyrics.lines[0].text == QStringLiteral("Fuck 'em"), kLyrics.lines[0].text);
                check("krc: Hello\" + world -> Hello\" world",
                      kLyrics.lines[1].text == QStringLiteral("Hello\" world"), kLyrics.lines[1].text);
                check("krc: can + ' + t -> can't",
                      kLyrics.lines[2].text == QStringLiteral("can't"), kLyrics.lines[2].text);
                check("krc: fuckin + ' + God -> fuckin' God",
                      kLyrics.lines[3].text == QStringLiteral("fuckin' God"), kLyrics.lines[3].text);
                check("krc: girls + ' + night -> girls' night",
                      kLyrics.lines[4].text == QStringLiteral("girls' night"), kLyrics.lines[4].text);
                check("krc: \" + Hello + \" + world -> \"Hello\" world",
                      kLyrics.lines[5].text == QStringLiteral("\"Hello\" world"), kLyrics.lines[5].text);
            }
        }

        // YRC punctuation, quotes, and standalone apostrophes spacing
        {
            const QString yrcPunct = QStringLiteral(
                "[1000,3000](1000,300,0)Fuck(1300,100,0)'(1400,300,0)em\n"
                "[4000,2000](4000,500,0)Hello\"(4500,500,0)world\n"
                "[7000,2000](7000,300,0)can(7300,100,0)'(7400,300,0)t\n"
                "[10000,3000](10000,300,0)fuckin(10300,100,0)'(10400,300,0)God\n"
                "[13000,3000](13000,300,0)girls(13300,100,0)'(13400,300,0)night\n"
                "[16000,4000](16000,100,0)\"(16100,300,0)Hello(16400,100,0)\"(16500,300,0)world\n");
            Lyrics yLyrics;
            const bool yOk = YrcParser::parse(yrcPunct, &yLyrics);
            check("yrc punct & apostrophe: parse ok", yOk && yLyrics.lines.size() == 6);
            if (yLyrics.lines.size() == 6) {
                check("yrc: Fuck + ' + em -> Fuck 'em",
                      yLyrics.lines[0].text == QStringLiteral("Fuck 'em"), yLyrics.lines[0].text);
                check("yrc: Hello\" + world -> Hello\" world",
                      yLyrics.lines[1].text == QStringLiteral("Hello\" world"), yLyrics.lines[1].text);
                check("yrc: can + ' + t -> can't",
                      yLyrics.lines[2].text == QStringLiteral("can't"), yLyrics.lines[2].text);
                check("yrc: fuckin + ' + God -> fuckin' God",
                      yLyrics.lines[3].text == QStringLiteral("fuckin' God"), yLyrics.lines[3].text);
                check("yrc: girls + ' + night -> girls' night",
                      yLyrics.lines[4].text == QStringLiteral("girls' night"), yLyrics.lines[4].text);
                check("yrc: \" + Hello + \" + world -> \"Hello\" world",
                      yLyrics.lines[5].text == QStringLiteral("\"Hello\" world"), yLyrics.lines[5].text);
            }
        }

        // Duration bounds validation (> 1.15x trackDurationMs rejected)
        const QString yrcOverDur = QStringLiteral("[300000,2000](300000,2000,0)Too late\n");
        Lyrics yrcDurLyrics;
        check("duration check: YrcParser rejects >1.15x trackDurationMs",
              !YrcParser::parse(yrcOverDur, &yrcDurLyrics, 180000));

        const QString qrcOverDur = QStringLiteral("[300000,2000]Too late(300000,2000)\n");
        Lyrics qrcDurLyrics;
        check("duration check: QrcDecoder rejects >1.15x trackDurationMs",
              !QrcDecoder::parse(qrcOverDur, &qrcDurLyrics, 180000));

        // --- Stage 1 & Stage 2 Limit and Filter Tests ---

        // 1. Decompression bomb test (> 16 MiB decompressed)
        {
            // Compress 17 MiB of zeroes
            const QByteArray bigZeros(17 * 1024 * 1024, 0);
            const QByteArray bomb = compressZlib(bigZeros);
            const QByteArray decompressed = ZlibCodec::decompress(bomb);
            check("limits: decompression bomb (>16 MiB) rejected by ZlibCodec",
                  decompressed.isEmpty(), QStringLiteral("decompressed size: %1").arg(decompressed.size()));
        }

        // 2. Line count limit (> 10 000 lines)
        {
            QString hugeLrc;
            hugeLrc.reserve(10005 * 25);
            for (int i = 0; i < 10005; ++i) {
                hugeLrc += QStringLiteral("[%1:%2.%3] line\n")
                    .arg(i / 60, 2, 10, QLatin1Char('0'))
                    .arg(i % 60, 2, 10, QLatin1Char('0'))
                    .arg(0, 2, 10, QLatin1Char('0'));
            }
            Lyrics hugeLyrics;
            check("limits: LrcParser rejects >10000 lines", !LrcParser::parseLrc(hugeLrc, &hugeLyrics));
        }

        // 3. Line length limit (> 16 384 chars)
        {
            QString longLineLrc = QStringLiteral("[00:01.00] ");
            longLineLrc.append(QString(17000, QLatin1Char('x')));
            longLineLrc.append(QLatin1Char('\n'));
            Lyrics longLineLyrics;
            check("limits: LrcParser rejects line >16384 chars", !LrcParser::parseLrc(longLineLrc, &longLineLyrics));
        }

        // 4. Word count limit (> 100 000 words)
        {
            QString manyWordsLrc;
            QString wordsPerLine;
            for (int w = 0; w < 200; ++w) {
                wordsPerLine += QStringLiteral(" word");
            }
            wordsPerLine += QLatin1Char('\n');
            manyWordsLrc.reserve(600 * wordsPerLine.size() + 100);
            for (int l = 0; l < 600; ++l) { // 600 * 200 = 120 000 words
                manyWordsLrc += QStringLiteral("[%1:%2.%3]%4")
                    .arg(l / 60, 2, 10, QLatin1Char('0'))
                    .arg(l % 60, 2, 10, QLatin1Char('0'))
                    .arg(0, 2, 10, QLatin1Char('0'))
                    .arg(wordsPerLine);
            }
            Lyrics manyWordsLyrics;
            check("limits: LrcParser rejects >100000 words", !LrcParser::parseLrc(manyWordsLrc, &manyWordsLyrics));
        }

        // 5. Service Line Filtering tests (LyricFilter::isCreditOrNotice)
        {
            // Standard section tags
            check("filter: [Intro]", LyricFilter::isCreditOrNotice(QStringLiteral("[Intro]")));
            check("filter: [Verse 1]", LyricFilter::isCreditOrNotice(QStringLiteral("[Verse 1]")));
            check("filter: [Chorus]", LyricFilter::isCreditOrNotice(QStringLiteral("[Chorus]")));
            check("filter: [Chorus: Artist]", LyricFilter::isCreditOrNotice(QStringLiteral("[Chorus: Eminem]")));
            check("filter: [Pre-Chorus]", LyricFilter::isCreditOrNotice(QStringLiteral("[Pre-Chorus]")));
            check("filter: [Hook]", LyricFilter::isCreditOrNotice(QStringLiteral("[Hook]")));
            check("filter: [Bridge]", LyricFilter::isCreditOrNotice(QStringLiteral("[Bridge]")));
            check("filter: [Instrumental]", LyricFilter::isCreditOrNotice(QStringLiteral("[Instrumental]")));

            // Russian section tags
            check("filter: [Куплет 1]", LyricFilter::isCreditOrNotice(QStringLiteral("[Куплет 1]")));
            check("filter: [Припев]", LyricFilter::isCreditOrNotice(QStringLiteral("[Припев]")));
            check("filter: [Бридж]", LyricFilter::isCreditOrNotice(QStringLiteral("[Бридж]")));
            check("filter: [Интро]", LyricFilter::isCreditOrNotice(QStringLiteral("[Интро]")));
            check("filter: [Аутро]", LyricFilter::isCreditOrNotice(QStringLiteral("[Аутро]")));
            check("filter: [Вступление]", LyricFilter::isCreditOrNotice(QStringLiteral("[Вступление]")));

            // Explicit credits with colon
            check("filter: 作词：Имя", LyricFilter::isCreditOrNotice(QStringLiteral("作词：张三")));
            check("filter: 作曲: Name", LyricFilter::isCreditOrNotice(QStringLiteral("作曲: 李四")));
            check("filter: Written by: Name", LyricFilter::isCreditOrNotice(QStringLiteral("Written by: John Doe")));
            check("filter: Produced by: Name", LyricFilter::isCreditOrNotice(QStringLiteral("Produced by: Dr. Dre")));
            check("filter: 演唱：Name", LyricFilter::isCreditOrNotice(QStringLiteral("演唱：王五")));
            check("filter: 词：Name", LyricFilter::isCreditOrNotice(QStringLiteral("词：赵六")));
            check("filter: 曲：Name", LyricFilter::isCreditOrNotice(QStringLiteral("曲：钱七")));
            check("filter: OP: Company", LyricFilter::isCreditOrNotice(QStringLiteral("OP: Music Corp")));
            check("filter: SP: Company", LyricFilter::isCreditOrNotice(QStringLiteral("SP: Music Corp")));

            // Explicit credits without colon (separated role + value)
            check("filter: 作词 Name", LyricFilter::isCreditOrNotice(QStringLiteral("作词 张三")));
            check("filter: Written by Name", LyricFilter::isCreditOrNotice(QStringLiteral("Written by John Doe")));
            check("filter: Produced by Name", LyricFilter::isCreditOrNotice(QStringLiteral("Produced by Metro Boomin")));

            // Negative tests: sung lyrics must NOT be filtered!
            check("filter: (yeah) backing preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("(yeah)")));
            check("filter: (ah) backing preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("(ah)")));
            check("filter: embedded intro preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("intro to my life")));
            check("filter: embedded chorus preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("sing the chorus again")));
            check("filter: stop preserved (not op)", !LyricFilter::isCreditOrNotice(QStringLiteral("please don't stop the music")));
            check("filter: hope preserved (not op)", !LyricFilter::isCreditOrNotice(QStringLiteral("I have a hope")));
            check("filter: special preserved (not sp)", !LyricFilter::isCreditOrNotice(QStringLiteral("something special")));
            check("filter: name without role preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("Michael Jackson")));
            check("filter: Chinese lyrics preserved", !LyricFilter::isCreditOrNotice(QStringLiteral("海阔天空 在我心中")));
        }

        // 6. LRC Time Boundary Preservation
        {
            // Lyric on 1s, credit on 3s, next lyric on 10s.
            // Line 1 must end at 3s, NOT 10s!
            const QString lrcWithCredit = QStringLiteral(
                "[00:01.00]First line of song\n"
                "[00:03.00]作词：Some Lyricist\n"
                "[00:10.00]Second line of song\n"
            );
            Lyrics lrcRes;
            check("boundary: parse LRC with credit", LrcParser::parseLrc(lrcWithCredit, &lrcRes, 30000));
            check("boundary: exactly 2 lines in result (credit omitted)", lrcRes.lines.size() == 2);
            if (lrcRes.lines.size() == 2) {
                check("boundary: line 1 text matches", lrcRes.lines[0].text == QStringLiteral("First line of song"));
                check("boundary: line 1 starts at 1000 ms", lrcRes.lines[0].startMs == 1000);
                check("boundary: line 1 ends at 3000 ms (invisible boundary preserved)",
                      lrcRes.lines[0].endMs == 3000, QString::number(lrcRes.lines[0].endMs));
                check("boundary: line 2 starts at 10000 ms", lrcRes.lines[1].startMs == 10000);
            }
        }
    }

    if (s_failed > 0) {
        std::cerr << s_failed << " tests failed\n";
        return 1;
    }

    std::cout << "All codec tests passed.\n";
    return 0;
}
