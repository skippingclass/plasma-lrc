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

    if (s_failed == 0) {
        std::cout << "\nAll TTML parser tests passed successfully.\n";
        return 0;
    }
    std::cout << "\n" << s_failed << " test(s) failed.\n";
    return 1;
}
