/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lyricselection.h"
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
    // 1. Existing bands fixture word selection:
    // word 0 (main): bands, 110279..110743
    // word 1 (bg): (Blegh,, 109134..109538
    // word 2 (bg): blegh), 109890..110345
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
        check("bands-fixture: parsed", ok);
        check("bands-fixture: 1 line", lyrics.lines.size() == 1);
        if (lyrics.lines.size() == 1) {
            const LyricLine &line = lyrics.lines.at(0);
            check("bands-fixture: 3 words", line.words.size() == 3);

            // Forward selection
            const int at109950 = LyricSelection::wordAt(line, 109950, LyricSelection::Voice::Any);
            check("bands: at 109950 selects bg index 2", at109950 == 2, QString::number(at109950));

            const int at110400 = LyricSelection::wordAt(line, 110400, LyricSelection::Voice::Any);
            check("bands: at 110400 selects main index 0", at110400 == 0, QString::number(at110400));

            // Reverse seek: jump back to 109950, result must be identical regardless of previous cursor
            const int rev109950 = LyricSelection::wordAt(line, 109950, LyricSelection::Voice::Any);
            check("bands: reverse seek to 109950 still selects bg index 2", rev109950 == 2, QString::number(rev109950));

            const int rev110400 = LyricSelection::wordAt(line, 110400, LyricSelection::Voice::Any);
            check("bands: forward again to 110400 selects main index 0", rev110400 == 0, QString::number(rev110400));

            // Voice::Main filter
            const int mainAt109950 = LyricSelection::wordAt(line, 109950, LyricSelection::Voice::Main);
            check("bands: Voice::Main at 109950 is -1", mainAt109950 == -1, QString::number(mainAt109950));

            const int mainAt110400 = LyricSelection::wordAt(line, 110400, LyricSelection::Voice::Main);
            check("bands: Voice::Main at 110400 is 0", mainAt110400 == 0, QString::number(mainAt110400));

            // Voice::Background filter
            const int bgAt109950 = LyricSelection::wordAt(line, 109950, LyricSelection::Voice::Background);
            check("bands: Voice::Background at 109950 is 2", bgAt109950 == 2, QString::number(bgAt109950));

            const int bgAt110400 = LyricSelection::wordAt(line, 110400, LyricSelection::Voice::Background);
            check("bands: Voice::Background at 110400 is -1", bgAt110400 == -1, QString::number(bgAt110400));
        }
    }

    // 2. Non-monotonic / overlapping timings within same voice: latest start deterministic
    {
        LyricLine line;
        LyricWord w0;
        w0.text = QStringLiteral("w0");
        w0.startMs = 1000;
        w0.endMs = 3000;
        w0.isBackground = false;

        LyricWord w1;
        w1.text = QStringLiteral("w1");
        w1.startMs = 2000;
        w1.endMs = 3500;
        w1.isBackground = false;

        line.words.append(w0);
        line.words.append(w1);

        // At 2500, both w0 and w1 are active. w1 has later startMs (2000 > 1000), so w1 must be selected
        const int chosen = LyricSelection::wordAt(line, 2500, LyricSelection::Voice::Main);
        check("overlap: latest start selected", chosen == 1, QString::number(chosen));

        // Simultaneous start deterministic tie-breaker: higher index wins
        LyricWord w2;
        w2.text = QStringLiteral("w2");
        w2.startMs = 4000;
        w2.endMs = 5000;
        w2.isBackground = false;

        LyricWord w3;
        w3.text = QStringLiteral("w3");
        w3.startMs = 4000;
        w3.endMs = 5000;
        w3.isBackground = false;

        line.words.append(w2);
        line.words.append(w3);

        const int tieChosen = LyricSelection::wordAt(line, 4500, LyricSelection::Voice::Main);
        check("overlap: simultaneous start deterministic tie-breaker", tieChosen == 3, QString::number(tieChosen));
    }

    // 3. Active main over active background for Voice::Any
    {
        LyricLine line;
        LyricWord mainWord;
        mainWord.text = QStringLiteral("main");
        mainWord.startMs = 5000;
        mainWord.endMs = 7000;
        mainWord.isBackground = false;

        LyricWord bgWord;
        bgWord.text = QStringLiteral("bg");
        bgWord.startMs = 5500;
        bgWord.endMs = 6500;
        bgWord.isBackground = true;
        bgWord.backgroundGroup = 0;

        line.words.append(bgWord);   // index 0: bg
        line.words.append(mainWord); // index 1: main

        // At 6000 both are active: Any must select active main over active background
        const int anyChosen = LyricSelection::wordAt(line, 6000, LyricSelection::Voice::Any);
        check("active-priority: Any selects active main over bg", anyChosen == 1, QString::number(anyChosen));

        // When requesting Background voice explicitly, bg is selected
        const int bgChosen = LyricSelection::wordAt(line, 6000, LyricSelection::Voice::Background);
        check("active-priority: Background selects bg", bgChosen == 0, QString::number(bgChosen));

        // When main is inactive (at 5200 bg not started yet, only main active)
        const int mainOnly = LyricSelection::wordAt(line, 5200, LyricSelection::Voice::Any);
        check("active-priority: main active", mainOnly == 1, QString::number(mainOnly));

        // When main is inactive and only bg is active (at 6600 main ended? no, main ends at 7000)
    }

    // 4. Line-level isBackground (explicit background paragraph)
    {
        LyricLine line;
        line.isBackground = true;

        LyricWord w0;
        w0.text = QStringLiteral("chorus");
        w0.startMs = 1000;
        w0.endMs = 2000;
        w0.isBackground = false; // word-level flag false, but line.isBackground is true
        line.words.append(w0);

        // Voice::Main must reject words in background line
        const int mainRes = LyricSelection::wordAt(line, 1500, LyricSelection::Voice::Main);
        check("line-bg: Main voice returns -1 on bg line", mainRes == -1, QString::number(mainRes));

        // Voice::Background accepts it
        const int bgRes = LyricSelection::wordAt(line, 1500, LyricSelection::Voice::Background);
        check("line-bg: Background voice selects word on bg line", bgRes == 0, QString::number(bgRes));

        // Voice::Any accepts it as background
        const int anyRes = LyricSelection::wordAt(line, 1500, LyricSelection::Voice::Any);
        check("line-bg: Any voice selects word on bg line", anyRes == 0, QString::number(anyRes));
    }

    // 5. Gap hold: only when NO matching active word, by most recent end, never future/zero intervals
    {
        LyricLine line;
        LyricWord w0;
        w0.text = QStringLiteral("first");
        w0.startMs = 1000;
        w0.endMs = 2000;
        w0.isBackground = false;

        LyricWord w1;
        w1.text = QStringLiteral("second");
        w1.startMs = 4000;
        w1.endMs = 5000;
        w1.isBackground = false;

        line.words.append(w0);
        line.words.append(w1);

        // In gap at 2200, holdMs=0 -> -1
        check("hold: no hold when holdMs=0", LyricSelection::wordAt(line, 2200, LyricSelection::Voice::Any, 0) == -1);

        // In gap at 2200, holdMs=500 -> w0 held (ended at 2000, 200ms ago <= 500)
        check("hold: w0 held during gap", LyricSelection::wordAt(line, 2200, LyricSelection::Voice::Any, 500) == 0);

        // In gap at 2600, holdMs=500 -> expired (600ms > 500)
        check("hold: expired after holdMs", LyricSelection::wordAt(line, 2600, LyricSelection::Voice::Any, 500) == -1);

        // At 4100, w1 is active: active matching word has priority over any hold
        check("hold: active word has priority over hold", LyricSelection::wordAt(line, 4100, LyricSelection::Voice::Any, 5000) == 1);

        // Most recent end selection among hold candidates
        LyricLine lineMulti;
        LyricWord m0;
        m0.text = QStringLiteral("m0");
        m0.startMs = 1000;
        m0.endMs = 1800;

        LyricWord m1;
        m1.text = QStringLiteral("m1");
        m1.startMs = 1200;
        m1.endMs = 2000;

        lineMulti.words.append(m0);
        lineMulti.words.append(m1);

        // At 2100 with holdMs=500: both ended within hold window, m1 ended more recently (2000 > 1800)
        const int recentEnd = LyricSelection::wordAt(lineMulti, 2100, LyricSelection::Voice::Any, 500);
        check("hold: selects most recent end", recentEnd == 1, QString::number(recentEnd));

        // Never future intervals
        LyricLine lineFuture;
        LyricWord fut;
        fut.text = QStringLiteral("future");
        fut.startMs = 3000;
        fut.endMs = 4000;
        lineFuture.words.append(fut);
        check("hold: future word never held", LyricSelection::wordAt(lineFuture, 2500, LyricSelection::Voice::Any, 1000) == -1);

        // Never indefinite zero intervals
        LyricLine lineZero;
        LyricWord zeroWord;
        zeroWord.text = QStringLiteral("zero");
        zeroWord.startMs = 2000;
        zeroWord.endMs = 2000;
        lineZero.words.append(zeroWord);
        check("hold: zero interval word never held", LyricSelection::wordAt(lineZero, 2100, LyricSelection::Voice::Any, 500) == -1);

        // Hold primary priority over background
        LyricLine lineBoth;
        LyricWord mainH;
        mainH.text = QStringLiteral("mainH");
        mainH.startMs = 1000;
        mainH.endMs = 2000;
        mainH.isBackground = false;

        LyricWord bgH;
        bgH.text = QStringLiteral("bgH");
        bgH.startMs = 1200;
        bgH.endMs = 2050;
        bgH.isBackground = true;

        lineBoth.words.append(mainH);
        lineBoth.words.append(bgH);

        // At 2100 with holdMs=500: main is held under primary priority
        check("hold: primary priority in hold", LyricSelection::wordAt(lineBoth, 2100, LyricSelection::Voice::Any, 500) == 0);

        // At 2600 with holdMs=500: main hold expired (600 > 500), bg hold expired too (550 > 500) -> -1
        check("hold: both expired", LyricSelection::wordAt(lineBoth, 2600, LyricSelection::Voice::Any, 500) == -1);

        // Background fallback when main never existed in line
        LyricLine lineBgOnly;
        lineBgOnly.words.append(bgH);
        check("hold: bg fallback when no main", LyricSelection::wordAt(lineBgOnly, 2100, LyricSelection::Voice::Any, 500) == 0);
    }

    // 6. Empty line handling
    {
        LyricLine emptyLine;
        check("empty: wordAt returns -1", LyricSelection::wordAt(emptyLine, 1000) == -1);
    }

    if (s_failed == 0) {
        std::cout << "\nAll lyric selection tests passed successfully.\n";
        return 0;
    }
    std::cout << "\n" << s_failed << " test(s) failed.\n";
    return 1;
}
