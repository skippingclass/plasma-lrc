/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "ttmlparser.h"

#include <iostream>
#include <QString>

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
} // namespace

int main()
{
    // 1. Timestamp parsing
    {
        qint64 ms = 0;
        check("ts: mm:ss.xxx", TtmlParser::parseTimestamp(QStringLiteral("01:23.456"), &ms) && ms == 83456, QString::number(ms));
        check("ts: hh:mm:ss.xxx", TtmlParser::parseTimestamp(QStringLiteral("01:02:03.450"), &ms) && ms == 3723450, QString::number(ms));
        check("ts: seconds with s", TtmlParser::parseTimestamp(QStringLiteral("45.67s"), &ms) && ms == 45670, QString::number(ms));
        check("ts: milliseconds", TtmlParser::parseTimestamp(QStringLiteral("2500ms"), &ms) && ms == 2500, QString::number(ms));
        check("ts: raw float", TtmlParser::parseTimestamp(QStringLiteral("12.5"), &ms) && ms == 12500, QString::number(ms));
        check("ts: invalid string", !TtmlParser::parseTimestamp(QStringLiteral("invalid"), &ms));
        check("ts: empty string", !TtmlParser::parseTimestamp(QStringLiteral(""), &ms));
    }

    // 2. Line-synced TTML
    {
        const QString ttml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:05.000\" end=\"00:10.000\">First line of text</p>\n"
            "      <p begin=\"00:12.000\" end=\"00:18.000\">Second line of text</p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 30000);
        check("line-ttml: parsed", ok);
        check("line-ttml: type is Line", lyrics.type == LyricsType::Line);
        check("line-ttml: 2 lines", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            check("line-ttml: line 1 text", lyrics.lines.at(0).text == QStringLiteral("First line of text"));
            check("line-ttml: line 1 start", lyrics.lines.at(0).startMs == 5000);
            check("line-ttml: line 1 end", lyrics.lines.at(0).endMs == 10000);
            check("line-ttml: line 2 text", lyrics.lines.at(1).text == QStringLiteral("Second line of text"));
            check("line-ttml: line 2 start", lyrics.lines.at(1).startMs == 12000);
            check("line-ttml: line 2 end", lyrics.lines.at(1).endMs == 18000);
        }
    }

    // 3. Word/syllable-synced Apple Music TTML
    {
        const QString ttml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:itunes=\"http://music.apple.com/lyric-ttml-extensions\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:03.000\" end=\"00:08.000\">\n"
            "        <span begin=\"00:03.000\" end=\"00:03.500\">Hel</span>\n"
            "        <span begin=\"00:03.500\" end=\"00:04.200\">lo </span>\n"
            "        <span begin=\"00:04.500\" end=\"00:07.800\">world</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 20000);
        check("word-ttml: parsed", ok);
        check("word-ttml: type is Syllable", lyrics.type == LyricsType::Syllable);
        check("word-ttml: 1 line", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("word-ttml: line text", line.text == QStringLiteral("Hello world"), line.text);
            check("word-ttml: word count", line.words.size() == 3, QString::number(line.words.size()));
            if (line.words.size() == 3) {
                // "Hel" and "lo" belong to the same word group "Hello" (indices 0..5)
                check("word-ttml: word 0 text", line.words.at(0).text == QStringLiteral("Hel"));
                check("word-ttml: word 0 group bounds", line.words.at(0).groupStart == 0 && line.words.at(0).groupEnd == 5,
                      QStringLiteral("%1..%2").arg(line.words.at(0).groupStart).arg(line.words.at(0).groupEnd));
                check("word-ttml: word 1 text", line.words.at(1).text == QStringLiteral("lo"));
                check("word-ttml: word 1 group bounds", line.words.at(1).groupStart == 0 && line.words.at(1).groupEnd == 5,
                      QStringLiteral("%1..%2").arg(line.words.at(1).groupStart).arg(line.words.at(1).groupEnd));
                // "world" is its own group (indices 6..11)
                check("word-ttml: word 2 text", line.words.at(2).text == QStringLiteral("world"));
                check("word-ttml: word 2 group bounds", line.words.at(2).groupStart == 6 && line.words.at(2).groupEnd == 11,
                      QStringLiteral("%1..%2").arg(line.words.at(2).groupStart).arg(line.words.at(2).groupEnd));
            }
        }
    }

    // 4. Malformed XML handling
    {
        Lyrics lyrics;
        check("bad-xml: rejected safely", !TtmlParser::parseTtml(QStringLiteral("not xml <><>"), &lyrics));
        check("empty: rejected safely", !TtmlParser::parseTtml(QStringLiteral(""), &lyrics));
    }

    // 5. Apple Music TTML with spaces outside spans, background vocals and translations
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"03:46.441\" end=\"03:48.457\">\n"
            "        <span begin=\"03:46.441\" end=\"03:46.694\">Will</span> <span begin=\"03:46.694\" end=\"03:46.909\">not</span> <span begin=\"03:46.909\" end=\"03:47.109\">let</span> <span begin=\"03:47.109\" end=\"03:47.292\">you</span> <span begin=\"03:47.292\" end=\"03:47.484\">go</span> <span ttm:role=\"x-bg\"><span begin=\"03:47.395\" end=\"03:47.710\">(Let</span> <span begin=\"03:47.710\" end=\"03:47.893\">me</span> <span begin=\"03:47.893\" end=\"03:48.457\">go)</span></span><span ttm:role=\"x-translation\" xml:lang=\"zh-CN\">不会放你走</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 240000);
        check("am-ttml: parsed", ok);
        check("am-ttml: 1 line", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("am-ttml: line text with spaces", line.text == QStringLiteral("Will not let you go (Let me go)"), line.text);
            check("am-ttml: word count is 8", line.words.size() == 8, QString::number(line.words.size()));
            if (line.words.size() == 8) {
                check("am-ttml: word 0 'Will'", line.words.at(0).text == QStringLiteral("Will") && line.words.at(0).groupStart == 0 && line.words.at(0).groupEnd == 4);
                check("am-ttml: word 1 'not'", line.words.at(1).text == QStringLiteral("not") && line.words.at(1).groupStart == 5 && line.words.at(1).groupEnd == 8);
                check("am-ttml: word 5 '(Let'", line.words.at(5).text == QStringLiteral("(Let") && line.words.at(5).groupStart == 20 && line.words.at(5).groupEnd == 24);
                check("am-ttml: word 7 'go)'", line.words.at(7).text == QStringLiteral("go)") && line.words.at(7).groupStart == 28 && line.words.at(7).groupEnd == 31);
            }
        }
    }

    // 6. Apple Music / Unison TTML with timed x-bg container immediately following word without space
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"01:48.750\" end=\"01:50.743\">\n"
            "        <span begin=\"01:50.279\" end=\"01:50.743\">bands</span><span ttm:role=\"x-bg\" begin=\"01:49.134\" end=\"01:50.345\"><span begin=\"01:49.134\" end=\"01:49.538\">(Blegh,</span> <span begin=\"01:49.890\" end=\"01:50.345\">blegh)</span></span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 120000);
        check("x-bg-timed: parsed", ok);
        check("x-bg-timed: 1 line", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("x-bg-timed: space separates word and bracket", line.text == QStringLiteral("bands (Blegh, blegh)"), line.text);
            check("x-bg-timed: word count is 3", line.words.size() == 3, QString::number(line.words.size()));
            if (line.words.size() == 3) {
                check("x-bg-timed: word 0 'bands'", line.words.at(0).text == QStringLiteral("bands") && line.words.at(0).groupStart == 0 && line.words.at(0).groupEnd == 5);
                check("x-bg-timed: word 0 not background", !line.words.at(0).isBackground && line.words.at(0).backgroundGroup == -1);
                check("x-bg-timed: word 1 '(Blegh,'", line.words.at(1).text == QStringLiteral("(Blegh,") && line.words.at(1).groupStart == 6 && line.words.at(1).groupEnd == 13);
                check("x-bg-timed: word 1 is background", line.words.at(1).isBackground && line.words.at(1).backgroundGroup == 0);
                check("x-bg-timed: word 2 'blegh)'", line.words.at(2).text == QStringLiteral("blegh)") && line.words.at(2).groupStart == 14 && line.words.at(2).groupEnd == 20);
                check("x-bg-timed: word 2 is background same group", line.words.at(2).isBackground && line.words.at(2).backgroundGroup == 0);
            }
        }
    }

    // 7. Explicit background paragraph (<p ttm:role="x-bg">) and role inheritance
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:10.000\" end=\"00:15.000\">\n"
            "        <span begin=\"00:10.000\" end=\"00:12.000\">Lead</span> <span begin=\"00:12.000\" end=\"00:14.000\">vocals</span>\n"
            "      </p>\n"
            "      <p ttm:role=\"x-bg\" begin=\"00:11.000\" end=\"00:16.000\">\n"
            "        <span begin=\"00:11.000\" end=\"00:13.000\">Backing</span> <span begin=\"00:13.000\" end=\"00:15.500\">vocals</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 30000);
        check("bg-paragraph: parsed", ok);
        check("bg-paragraph: 2 lines", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 2) {
            const LyricLine &lead = lyrics.lines.at(0);
            const LyricLine &bg = lyrics.lines.at(1);
            check("bg-paragraph: lead line is not bg", !lead.isBackground);
            check("bg-paragraph: bg line isBackground", bg.isBackground);
            check("bg-paragraph: bg words count", bg.words.size() == 2);
            if (bg.words.size() == 2) {
                check("bg-paragraph: bg word 0 isBackground", bg.words.at(0).isBackground && bg.words.at(0).backgroundGroup == 0);
                check("bg-paragraph: bg word 1 isBackground", bg.words.at(1).isBackground && bg.words.at(1).backgroundGroup == 0);
            }

            // Test lineAt priority: at 11500 both lead (10000..15000) and bg (11000..16000) are active.
            // Active primary lead line must be prioritized over simultaneous background line.
            int nextLine = -1;
            const int chosen = lyrics.lineAt(11500, &nextLine);
            check("lineAt: prioritize active primary over bg", chosen == 0, QString::number(chosen));

            // At 15500 lead has ended, bg is still active: bg fallback
            const int bgChosen = lyrics.lineAt(15500, &nextLine);
            check("lineAt: fallback to active bg line", bgChosen == 1, QString::number(bgChosen));

            // nextLineIndex check before any line
            lyrics.lineAt(5000, &nextLine);
            check("lineAt: nextLineIndex before lines", nextLine == 0, QString::number(nextLine));

            // nextLineIndex check during lead line
            lyrics.lineAt(10500, &nextLine);
            check("lineAt: nextLineIndex during lead line points to future bg", nextLine == 1, QString::number(nextLine));

            // nextLineIndex check after all lines
            lyrics.lineAt(20000, &nextLine);
            check("lineAt: nextLineIndex after all lines is -1", nextLine == -1, QString::number(nextLine));
        }
    }

    // 8. Multiple distinct x-bg containers get stable distinct backgroundGroups
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:01.000\" end=\"00:10.000\">\n"
            "        <span begin=\"00:01.000\" end=\"00:02.000\">A</span>\n"
            "        <span ttm:role=\"x-bg\"><span begin=\"00:02.000\" end=\"00:03.000\">B</span> <span begin=\"00:03.000\" end=\"00:04.000\">C</span></span>\n"
            "        <span begin=\"00:04.000\" end=\"00:05.000\">D</span>\n"
            "        <span ttm:role=\"x-bg\"><span begin=\"00:05.000\" end=\"00:06.000\">E</span></span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 20000);
        check("multi-bg: parsed", ok);
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("multi-bg: 5 words", line.words.size() == 5, QString::number(line.words.size()));
            if (line.words.size() == 5) {
                check("multi-bg: word 0 'A' main", !line.words.at(0).isBackground && line.words.at(0).backgroundGroup == -1);
                check("multi-bg: word 1 'B' bg group 0", line.words.at(1).isBackground && line.words.at(1).backgroundGroup == 0);
                check("multi-bg: word 2 'C' bg group 0", line.words.at(2).isBackground && line.words.at(2).backgroundGroup == 0);
                check("multi-bg: word 3 'D' main", !line.words.at(3).isBackground && line.words.at(3).backgroundGroup == -1);
                check("multi-bg: word 4 'E' bg group 1", line.words.at(4).isBackground && line.words.at(4).backgroundGroup == 1);
            }
        }
    }

    // 9. Parentheses without explicit x-bg role do NOT infer background
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:01.000\" end=\"00:05.000\">\n"
            "        <span begin=\"00:01.000\" end=\"00:02.500\">(Hello</span> <span begin=\"00:02.500\" end=\"00:04.000\">world)</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 10000);
        check("no-bg-inference: parsed", ok);
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("no-bg-inference: line is not bg", !line.isBackground);
            if (line.words.size() == 2) {
                check("no-bg-inference: word 0 not bg", !line.words.at(0).isBackground && line.words.at(0).backgroundGroup == -1);
                check("no-bg-inference: word 1 not bg", !line.words.at(1).isBackground && line.words.at(1).backgroundGroup == -1);
            }
        }
    }

    // 10. Regression: long main line (0..10000) followed by 10 background lines (starting 100..1000)
    // At 1500, active main index 0 must be selected, not lost due to any lookback window.
    // Also test gap and after-all nextLineIndex semantics.
    {
        Lyrics lyrics;
        lyrics.type = LyricsType::Line;

        LyricLine mainLine;
        mainLine.text = QStringLiteral("Main lead line");
        mainLine.startMs = 0;
        mainLine.endMs = 10000;
        mainLine.isBackground = false;
        lyrics.lines.append(mainLine);

        // 10 background lines starting from 100 to 1000, ending at 1200
        for (int i = 1; i <= 10; ++i) {
            LyricLine bgLine;
            bgLine.text = QStringLiteral("bg %1").arg(i);
            bgLine.startMs = i * 100;
            bgLine.endMs = 1200;
            bgLine.isBackground = true;
            lyrics.lines.append(bgLine);
        }

        // At 1500: all 10 bg lines have ended, but mainLine (0..10000) is still active!
        int nextLine = -1;
        const int at1500 = lyrics.lineAt(1500, &nextLine);
        check("regression: at 1500 selects main index 0 despite 10 intervening bg lines", at1500 == 0, QString::number(at1500));
        // All lines in lyrics have started before 1500, so nextLine must be -1
        check("regression: nextLine at 1500 (no future lines) is -1", nextLine == -1, QString::number(nextLine));

        // After all lines have ended (e.g. 15000): lineAt returns -1 and nextLineIndex is -1
        nextLine = 999;
        const int at15000 = lyrics.lineAt(15000, &nextLine);
        check("regression: after all lines lineAt is -1", at15000 == -1, QString::number(at15000));
        check("regression: after all lines nextLine is -1", nextLine == -1, QString::number(nextLine));

        // In gap before future line: add a line at 20000..25000
        LyricLine futureLine;
        futureLine.text = QStringLiteral("Future line");
        futureLine.startMs = 20000;
        futureLine.endMs = 25000;
        lyrics.lines.append(futureLine);

        // In gap between 10000 and 20000 (e.g. at 15000):
        nextLine = -1;
        const int inGap = lyrics.lineAt(15000, &nextLine);
        check("regression: in gap lineAt is -1", inGap == -1, QString::number(inGap));
        check("regression: in gap nextLine points to future line", nextLine == 11, QString::number(nextLine));
    }

    // 11. TTML metadata namespace URI support with arbitrary prefix
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:meta=\"http://www.w3.org/ns/ttml#metadata\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p meta:role=\"x-bg\" begin=\"00:01.000\" end=\"00:04.000\">\n"
            "        <span begin=\"00:01.000\" end=\"00:03.000\">Backing vocal</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 10000);
        check("meta-ns: parsed", ok);
        if (lyrics.lines.size() == 1) {
            check("meta-ns: line recognized as background via namespace URI", lyrics.lines.at(0).isBackground);
        }
    }

    // 12. Apostrophe spacing regression (fuckin' God, "Fuck 'em", can’t)
    {
        const QString ttml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:00.000\" end=\"00:05.000\">\n"
            "        <span begin=\"00:00.000\" end=\"00:00.500\">Wa,</span> <span begin=\"00:00.500\" end=\"00:01.000\">slayr,</span> <span begin=\"00:01.000\" end=\"00:01.200\">oh</span> <span begin=\"00:01.200\" end=\"00:01.400\">my</span> <span begin=\"00:01.400\" end=\"00:01.800\">fuckin'</span> <span begin=\"00:01.800\" end=\"00:02.000\">God</span>\n"
            "      </p>\n"
            "      <p begin=\"00:05.000\" end=\"00:10.000\">\n"
            "        <span begin=\"00:05.000\" end=\"00:05.500\">\"Fuck</span> <span begin=\"00:05.500\" end=\"00:06.000\">'em\"</span>\n"
            "      </p>\n"
            "      <p begin=\"00:10.000\" end=\"00:15.000\">\n"
            "        <span begin=\"00:10.000\" end=\"00:10.500\">can</span><span begin=\"00:10.500\" end=\"00:11.000\">’t</span> <span begin=\"00:11.000\" end=\"00:11.500\">stop</span>\n"
            "      </p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics;
        const bool ok = TtmlParser::parseTtml(ttml, &lyrics, 20000);
        check("apostrophe: parsed", ok);
        check("apostrophe: 3 lines", lyrics.lines.size() == 3, QString::number(lyrics.lines.size()));
        if (lyrics.lines.size() == 3) {
            // Line 1: Word ending with apostrophe followed by space
            const LyricLine &line1 = lyrics.lines.at(0);
            check("apostrophe: fuckin' God has space", line1.text == QStringLiteral("Wa, slayr, oh my fuckin' God"), line1.text);
            check("apostrophe: line 1 has 6 words", line1.words.size() == 6, QString::number(line1.words.size()));
            if (line1.words.size() == 6) {
                check("apostrophe: word 4 is fuckin'", line1.words.at(4).text == QStringLiteral("fuckin'"));
                check("apostrophe: word 5 is God", line1.words.at(5).text == QStringLiteral("God"));
                // Ensure distinct word group bounds
                check("apostrophe: fuckin' groupEnd != God groupStart", line1.words.at(4).groupEnd < line1.words.at(5).groupStart,
                      QStringLiteral("%1 vs %2").arg(line1.words.at(4).groupEnd).arg(line1.words.at(5).groupStart));
            }

            // Line 2: Word followed by space and leading apostrophe
            const LyricLine &line2 = lyrics.lines.at(1);
            check("apostrophe: Fuck 'em has space", line2.text == QStringLiteral("\"Fuck 'em\""), line2.text);
            check("apostrophe: line 2 has 2 words", line2.words.size() == 2, QString::number(line2.words.size()));

            // Line 3: Syllable spans for contraction without space remain joined
            const LyricLine &line3 = lyrics.lines.at(2);
            check("apostrophe: can’t remains joined", line3.text == QStringLiteral("can’t stop"), line3.text);
            check("apostrophe: line 3 has 3 words", line3.words.size() == 3, QString::number(line3.words.size()));
            if (line3.words.size() == 3) {
                check("apostrophe: can and ’t share same word group",
                      line3.words.at(0).groupStart == line3.words.at(1).groupStart && line3.words.at(0).groupEnd == line3.words.at(1).groupEnd);
            }
        }
    }

    // 14. Unsynced TTML detection (lrc.red timing="None" and untimed <p> tags)
    {
        const QString unsyncedTimingNone = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:lrc=\"http://lrc.red/lyric-ttml-internal\" lrc:timing=\"None\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p>Unsynced line one</p>\n"
            "      <p>Unsynced line two</p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics1;
        const bool ok1 = TtmlParser::parseTtml(unsyncedTimingNone, &lyrics1, 100000);
        check("unsynced timing=None: not ok", !ok1);
        check("unsynced timing=None: type is Static", lyrics1.type == LyricsType::Static);
        check("unsynced timing=None: not usable", !lyrics1.isUsable());
        check("unsynced timing=None: lines parsed", lyrics1.lines.size() == 2, QString::number(lyrics1.lines.size()));

        const QString untimedParagraphs = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p>Static line one</p>\n"
            "      <p>Static line two</p>\n"
            "      <p>Static line three</p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics2;
        const bool ok2 = TtmlParser::parseTtml(untimedParagraphs, &lyrics2, 100000);
        check("untimed <p>: not ok", !ok2);
        check("untimed <p>: type is Static", lyrics2.type == LyricsType::Static);
        check("untimed <p>: not usable", !lyrics2.isUsable());

        const QString zeroTimedParagraphs = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\">\n"
            "  <body>\n"
            "    <div>\n"
            "      <p begin=\"00:00.000\">First line at zero</p>\n"
            "      <p begin=\"00:00.000\">Second line at zero</p>\n"
            "      <p begin=\"00:00.000\">Third line at zero</p>\n"
            "    </div>\n"
            "  </body>\n"
            "</tt>");

        Lyrics lyrics3;
        const bool ok3 = TtmlParser::parseTtml(zeroTimedParagraphs, &lyrics3, 100000);
        check("all-zero timestamps: not ok", !ok3);
        check("all-zero timestamps: type is Static", lyrics3.type == LyricsType::Static);
        check("all-zero timestamps: not usable", !lyrics3.isUsable());
    }

    if (s_failed == 0) {
        std::cout << "\nAll TTML parser tests passed successfully.\n";
        return 0;
    }
    std::cout << "\n" << s_failed << " test(s) failed.\n";
    return 1;
}
