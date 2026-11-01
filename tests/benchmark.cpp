// SPDX-License-Identifier: MIT
#include "lrcapplet.h"
#include "ttmlparser.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cstdio>
#include <vector>

namespace
{
QString generateNormalTtml(int lineCount = 40)
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
                                 "  <body>\n"
                                 "    <div>\n");
    for (int i = 0; i < lineCount; ++i) {
        const int startSec = i * 4;
        const int endSec = startSec + 3;
        xml += QStringLiteral("      <p begin=\"%1:%2.000\" end=\"%3:%4.000\">\n")
                   .arg(startSec / 60, 2, 10, QLatin1Char('0'))
                   .arg(startSec % 60, 2, 10, QLatin1Char('0'))
                   .arg(endSec / 60, 2, 10, QLatin1Char('0'))
                   .arg(endSec % 60, 2, 10, QLatin1Char('0'));
        for (int w = 0; w < 5; ++w) {
            const double wStart = startSec + w * 0.6;
            const double wEnd = wStart + 0.55;
            xml += QStringLiteral("        <span begin=\"%1s\" end=\"%2s\">word%3</span>\n")
                       .arg(wStart, 0, 'f', 2)
                       .arg(wEnd, 0, 'f', 2)
                       .arg(w + 1);
        }
        xml += QStringLiteral("      </p>\n");
    }
    xml += QStringLiteral("    </div>\n  </body>\n</tt>\n");
    return xml;
}

QString generateLongTtml(int lineCount = 600)
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
                                 "  <body>\n"
                                 "    <div>\n");
    for (int i = 0; i < lineCount; ++i) {
        const int startSec = i * 3;
        const int endSec = startSec + 2;
        xml += QStringLiteral("      <p begin=\"%1:%2.000\" end=\"%3:%4.000\">\n")
                   .arg(startSec / 60, 2, 10, QLatin1Char('0'))
                   .arg(startSec % 60, 2, 10, QLatin1Char('0'))
                   .arg(endSec / 60, 2, 10, QLatin1Char('0'))
                   .arg(endSec % 60, 2, 10, QLatin1Char('0'));
        for (int w = 0; w < 4; ++w) {
            const double wStart = startSec + w * 0.5;
            const double wEnd = wStart + 0.45;
            xml += QStringLiteral("        <span begin=\"%1s\" end=\"%2s\">syllable%3</span>\n")
                       .arg(wStart, 0, 'f', 2)
                       .arg(wEnd, 0, 'f', 2)
                       .arg(w + 1);
        }
        xml += QStringLiteral("      </p>\n");
    }
    xml += QStringLiteral("    </div>\n  </body>\n</tt>\n");
    return xml;
}

QString generateOverlappingTtml(int pairCount = 50)
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\">\n"
                                 "  <body>\n"
                                 "    <div>\n");
    for (int i = 0; i < pairCount; ++i) {
        const int mainStart = i * 6;
        const int mainEnd = mainStart + 5;
        // Main line
        xml += QStringLiteral("      <p begin=\"%1:%2.000\" end=\"%3:%4.000\">\n")
                   .arg(mainStart / 60, 2, 10, QLatin1Char('0'))
                   .arg(mainStart % 60, 2, 10, QLatin1Char('0'))
                   .arg(mainEnd / 60, 2, 10, QLatin1Char('0'))
                   .arg(mainEnd % 60, 2, 10, QLatin1Char('0'));
        for (int w = 0; w < 4; ++w) {
            const double wStart = mainStart + w * 1.0;
            const double wEnd = wStart + 0.9;
            xml += QStringLiteral("        <span begin=\"%1s\" end=\"%2s\">mainWord%3</span>\n")
                       .arg(wStart, 0, 'f', 2)
                       .arg(wEnd, 0, 'f', 2)
                       .arg(w + 1);
        }
        // In-line backing words with non-monotonic timings
        xml += QStringLiteral("        <span ttm:role=\"x-bg\" begin=\"%1s\" end=\"%2s\">(yeah)</span>\n")
                   .arg(mainStart + 2.2, 0, 'f', 2)
                   .arg(mainStart + 3.8, 0, 'f', 2);
        xml += QStringLiteral("      </p>\n");

        // Overlapping distinct background line
        const double bgStart = mainStart + 2.0;
        const double bgEnd = mainStart + 4.5;
        xml += QStringLiteral("      <p ttm:role=\"x-bg\" begin=\"%1s\" end=\"%2s\">\n")
                   .arg(bgStart, 0, 'f', 2)
                   .arg(bgEnd, 0, 'f', 2);
        xml += QStringLiteral("        <span begin=\"%1s\" end=\"%2s\">echo</span>\n")
                   .arg(bgStart + 0.2, 0, 'f', 2)
                   .arg(bgStart + 1.2, 0, 'f', 2);
        xml += QStringLiteral("        <span begin=\"%1s\" end=\"%2s\">harmony</span>\n")
                   .arg(bgStart + 1.3, 0, 'f', 2)
                   .arg(bgEnd - 0.2, 0, 'f', 2);
        xml += QStringLiteral("      </p>\n");
    }
    xml += QStringLiteral("    </div>\n  </body>\n</tt>\n");
    return xml;
}
} // namespace

struct BenchResult {
    double parseMs = 0;
    double projectUs = 0;
    double positionUpdateUs = 0;
};

class LrcBenchmark {
public:
    static BenchResult run(const QString &name, const QString &xml, LrcApplet &widget)
    {
        std::printf("========================================\n");
        std::printf("Benchmark: %s\n", qPrintable(name));
        std::printf("========================================\n");

        BenchResult result;
        QElapsedTimer timer;

        // 1. TTML Parsing
        const int parseIters = 200;
        Lyrics parsedLyrics;
        timer.start();
        for (int i = 0; i < parseIters; ++i) {
            Lyrics tmp;
            TtmlParser::parseTtml(xml, &tmp);
            if (i == 0) parsedLyrics = tmp;
        }
        const qint64 parseElapsedUs = timer.nsecsElapsed() / 1000;
        result.parseMs = static_cast<double>(parseElapsedUs) / (parseIters * 1000.0);
        std::printf("1. TTML Parsing (%d iters, %lld lines):\n", parseIters, static_cast<long long>(parsedLyrics.lines.size()));
        std::printf("   Total: %.2f ms | Avg per parse: %.3f ms\n", parseElapsedUs / 1000.0, result.parseMs);

        // 2. Projection preparation (projectLineForSeparateRendering)
        const int projIters = 500;
        timer.restart();
        for (int iter = 0; iter < projIters; ++iter) {
            for (const LyricLine &l : parsedLyrics.lines) {
                LrcApplet::projectLineForSeparateRendering(l);
            }
        }
        const qint64 projElapsedUs = timer.nsecsElapsed() / 1000;
        const int totalLinesProjected = projIters * parsedLyrics.lines.size();
        result.projectUs = totalLinesProjected > 0 ? static_cast<double>(projElapsedUs) / totalLinesProjected : 0.0;
        std::printf("2. Projection Preparation (%d line projections):\n", totalLinesProjected);
        std::printf("   Total: %.2f ms | Avg per line: %.3f us\n", projElapsedUs / 1000.0, result.projectUs);

        // 3. Position update (updateWord through playback timeline)
        widget.clearLyrics();
        widget.clearBacking();
        widget.m_lyrics = parsedLyrics;
        widget.m_wordSynced = (parsedLyrics.type == LyricsType::Syllable);
        widget.setAvailable(true);
        widget.setPlaying(true, true);
        widget.m_showBackingVocals = true;
        widget.m_positionValid = true;
        widget.m_gaplessLeadMs = 0;
        widget.m_lyricOffset = 0;
        widget.m_lineIndex = -1;
        widget.m_wordCursor = 0;

        const qint64 totalDurMs = parsedLyrics.lines.isEmpty() ? 0 : parsedLyrics.lines.last().endMs + 1000;
        const int stepMs = 50;
        int tickCount = 0;
        int linesHit = 0;
        int activeLineTransitions = 0;
        int lastLine = -1;

        timer.restart();
        for (qint64 t = 0; t <= totalDurMs; t += stepMs) {
            widget.m_positionClock.invalidate();
            widget.m_positionBaseMs = t + LrcApplet::kAudioLeadCompensationMs;
            widget.m_positionValid = true;
            widget.m_positionChecks.start();
            widget.updateWord();
            if (widget.lineIndex() >= 0) {
                ++linesHit;
                if (widget.lineIndex() != lastLine) {
                    ++activeLineTransitions;
                    lastLine = widget.lineIndex();
                }
            }
            ++tickCount;
        }
        const qint64 posElapsedUs = timer.nsecsElapsed() / 1000;
        result.positionUpdateUs = tickCount > 0 ? static_cast<double>(posElapsedUs) / tickCount : 0.0;
        std::printf("3. Position Updates (%d ticks, %d active hits, %d transitions at 50ms):\n", tickCount, linesHit, activeLineTransitions);
        std::printf("   Total: %.2f ms | Avg per tick: %.3f us\n", posElapsedUs / 1000.0, result.positionUpdateUs);
        std::printf("\n");

        return result;
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("PATH", runtime.path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", runtime.path().toUtf8());
    qputenv("XDG_CACHE_HOME", runtime.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-plasma-lrc-test-bus");
    qunsetenv("SPICY_LYRICS_SECRET_KEY");

    QApplication app(argc, argv);
    LrcApplet widget(nullptr, KPluginMetaData(), {});

    const QString normalTtml = generateNormalTtml(40);
    const QString longTtml = generateLongTtml(600);
    const QString overlappingTtml = generateOverlappingTtml(50);

    LrcBenchmark::run(QStringLiteral("Normal Song (~40 lines)"), normalTtml, widget);
    LrcBenchmark::run(QStringLiteral("Long Lyrics (600 lines)"), longTtml, widget);
    LrcBenchmark::run(QStringLiteral("Overlapping Main/Backing (100 lines)"), overlappingTtml, widget);

    return 0;
}
