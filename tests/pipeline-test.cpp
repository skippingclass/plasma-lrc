/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lyricspipelinemanager.h"
#include "lyricsprovider.h"
#include "localprovider.h"

#include <iostream>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

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

int s_concurrentInFlight = 0;
int s_maxObservedConcurrency = 0;

class FakeDelayedProvider : public LyricsProvider
{
    Q_OBJECT

public:
    FakeDelayedProvider(const QString &id,
                        Capabilities caps,
                        int delayMs,
                        bool succeed,
                        LyricsType lyricsType = LyricsType::Line,
                        QObject *parent = nullptr)
        : LyricsProvider(parent)
        , m_id(id)
        , m_caps(caps)
        , m_delayMs(delayMs)
        , m_succeed(succeed)
        , m_type(lyricsType)
    {
    }

    QString id() const override { return m_id; }
    QString displayName() const override { return m_id; }
    Capabilities capabilities() const override { return m_caps; }

    bool canHandle(const TrackMetadata &) const override { return m_canHandle; }
    void setCanHandle(bool val) { m_canHandle = val; }

    void requestLyrics(const TrackMetadata &meta, quint64 generation) override
    {
        Q_UNUSED(meta);
        ++m_callCount;
        m_inFlight = true;
        m_activeGeneration = generation;

        ++s_concurrentInFlight;
        if (s_concurrentInFlight > s_maxObservedConcurrency) {
            s_maxObservedConcurrency = s_concurrentInFlight;
        }

        if (m_delayMs < 0) {
            finishRequest(generation);
            return;
        }

        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this, generation] {
            finishRequest(generation);
        });
        m_timer->start(m_delayMs);
    }

    void finishRequest(quint64 generation)
    {
        if (!m_inFlight || generation != m_activeGeneration) {
            return;
        }
        m_inFlight = false;
        --s_concurrentInFlight;

        if (m_succeed) {
            Lyrics l;
            l.type = m_type;
            LyricLine line;
            line.text = QStringLiteral("Lyrics from ") + m_id;
            line.startMs = 1000;
            line.endMs = 3000;
            if (m_type == LyricsType::Syllable) {
                const int idLen = static_cast<int>(m_id.length());
                line.words.append({QStringLiteral("Lyrics"), 1000, 1500, 0, 6, 0, 6});
                line.words.append({QStringLiteral("from"), 1600, 2000, 7, 11, 7, 11});
                line.words.append({m_id, 2100, 3000, 12, 12 + idLen, 12, 12 + idLen});
            }
            l.lines.append(line);
            Q_EMIT lyricsLoaded(generation, l, m_id);
        } else {
            Q_EMIT lyricsFailed(generation, m_failReason, m_id);
        }
    }

    void cancel() override
    {
        ++m_cancelCount;
        if (m_inFlight) {
            m_inFlight = false;
            --s_concurrentInFlight;
        }
        if (m_timer) {
            m_timer->stop();
            m_timer->deleteLater();
            m_timer = nullptr;
        }
    }

    void invalidateCache(const TrackMetadata &meta) override
    {
        Q_UNUSED(meta);
        ++m_invalidateCount;
    }

    bool isInFlight() const { return m_inFlight; }
    int callCount() const { return m_callCount; }
    int cancelCount() const { return m_cancelCount; }
    int invalidateCount() const { return m_invalidateCount; }
    void setFailReason(MissingReason r) { m_failReason = r; }

private:
    QString m_id;
    Capabilities m_caps;
    int m_delayMs = 0;
    bool m_succeed = true;
    LyricsType m_type = LyricsType::Line;
    bool m_canHandle = true;
    bool m_inFlight = false;
    quint64 m_activeGeneration = 0;
    int m_callCount = 0;
    int m_cancelCount = 0;
    int m_invalidateCount = 0;
    MissingReason m_failReason = MissingReason::NotFound;
    QPointer<QTimer> m_timer;
};

void runEventLoopWithTimeout(int timeoutMs)
{
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

#include "pipeline-test.moc"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Test 1: Priority order and successful fallback
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("prov1"),
                                           LyricsProvider::Capability::LineSync, 0, false, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("prov2"),
                                           LyricsProvider::Capability::WordSync, 0, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("prov1"), QStringLiteral("prov2")});

        bool resolved = false;
        QString resolvedId;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &providerId) {
            resolved = true;
            resolvedId = providerId;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Test Song");
        meta.artist = QStringLiteral("Test Artist");

        pipeline.requestLyrics(meta, 1);
        runEventLoopWithTimeout(50);

        check("pipeline: prov1 was called first", p1->callCount() == 1);
        check("pipeline: prov2 was called as fallback", p2->callCount() == 1);
        check("pipeline: resolved successfully", resolved);
        check("pipeline: resolved by prov2", resolvedId == QStringLiteral("prov2"), resolvedId);
    }

    // Test 2: Disabled provider is skipped
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"),
                                           LyricsProvider::Capability::LineSync, 0, true, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"),
                                           LyricsProvider::Capability::WordSync, 0, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("p1"), QStringLiteral("p2")});
        pipeline.setProviderEnabled(QStringLiteral("p1"), false);

        bool resolved = false;
        QString resolvedId;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &providerId) {
            resolved = true;
            resolvedId = providerId;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Test Song");
        pipeline.requestLyrics(meta, 2);
        runEventLoopWithTimeout(50);

        check("disabled: p1 was not called", p1->callCount() == 0);
        check("disabled: p2 was called", p2->callCount() == 1);
        check("disabled: resolved by p2", resolvedId == QStringLiteral("p2"), resolvedId);
    }

    // Test 3: All fail
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"),
                                           LyricsProvider::Capability::LineSync, 0, false, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"),
                                           LyricsProvider::Capability::WordSync, 0, false, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);

        bool failed = false;
        bool finished = false;
        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            failed = true;
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::lookupFinished,
                         [&](quint64) {
            finished = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Unknown");
        pipeline.requestLyrics(meta, 3);
        runEventLoopWithTimeout(50);

        check("all-fail: p1 called", p1->callCount() == 1);
        check("all-fail: p2 called", p2->callCount() == 1);
        check("all-fail: allFailed emitted", failed);
        check("all-fail: lookupFinished emitted", finished);
    }

    // Test 4: LocalProvider
    {
        QTemporaryDir tempDir;
        check("local: temp dir valid", tempDir.isValid());

        const QString lrcPath = tempDir.filePath(QStringLiteral("The Artist - The Song.lrc"));
        QFile file(lrcPath);
        check("local: create lrc file", file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("[00:01.00]Line one\n[00:04.00]Line two\n");
        file.close();

        LocalProvider local;
        local.setCustomDirectory(tempDir.path());

        TrackMetadata meta;
        meta.artist = QStringLiteral("The Artist");
        meta.title = QStringLiteral("The Song");
        check("local: can handle", local.canHandle(meta));

        Lyrics loadedLyrics;
        QString loadedProviderId;
        QObject::connect(&local, &LyricsProvider::lyricsLoaded,
                         [&](quint64, const Lyrics &lyrics, const QString &pId) {
            loadedLyrics = lyrics;
            loadedProviderId = pId;
        });

        local.requestLyrics(meta, 4);

        check("local: loaded provider id", loadedProviderId == QStringLiteral("local"), loadedProviderId);
        check("local: lyrics is usable", loadedLyrics.isUsable());
        check("local: lines count", loadedLyrics.lines.size() == 2, QString::number(loadedLyrics.lines.size()));
        if (loadedLyrics.lines.size() == 2) {
            check("local: line 0 text", loadedLyrics.lines[0].text == QStringLiteral("Line one"));
            check("local: line 0 startMs", loadedLyrics.lines[0].startMs == 1000);
            check("local: line 1 text", loadedLyrics.lines[1].text == QStringLiteral("Line two"));
            check("local: line 1 startMs", loadedLyrics.lines[1].startMs == 4000);
        }
    }

    // Test 5: Provisional Line -> WordSync upgrade
    {
        LyricsPipelineManager pipeline;
        // Prov1 fast Line provider (10ms)
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1_fast_line"),
                                           LyricsProvider::Capability::LineSync,
                                           10, true, LyricsType::Line, &pipeline);
        // Prov2 slower Word provider (40ms)
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2_slow_word"),
                                           LyricsProvider::Capability::WordSync,
                                           40, true, LyricsType::Syllable, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("p1_fast_line"), QStringLiteral("p2_slow_word")});

        QList<QPair<QString, LyricsType>> resolutions;
        bool allFailedEmitted = false;
        bool finishedEmitted = false;

        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &l, const QString &pId) {
            resolutions.append({pId, l.type});
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            allFailedEmitted = true;
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::lookupFinished,
                         [&](quint64) {
            finishedEmitted = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 100);

        runEventLoopWithTimeout(100);

        check("upgrade: resolutions count is 2", resolutions.size() == 2, QString::number(resolutions.size()));
        if (resolutions.size() >= 2) {
            check("upgrade: first is provisional line",
                  resolutions[0].first == QStringLiteral("p1_fast_line") && resolutions[0].second == LyricsType::Line);
            check("upgrade: second is syllable upgrade",
                  resolutions[1].first == QStringLiteral("p2_slow_word") && resolutions[1].second == LyricsType::Syllable);
        }
        check("upgrade: allFailed never emitted", !allFailedEmitted);
        check("upgrade: lookupFinished emitted", finishedEmitted);
    }

    // Test 6: Out of order priority (higher priority late result supersedes lower priority same quality)
    {
        LyricsPipelineManager pipeline;
        // p1 has higher priority but slower (40ms)
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1_high_slow"),
                                           LyricsProvider::Capability::LineSync,
                                           40, true, LyricsType::Line, &pipeline);
        // p2 has lower priority but faster (10ms)
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2_low_fast"),
                                           LyricsProvider::Capability::LineSync,
                                           10, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("p1_high_slow"), QStringLiteral("p2_low_fast")});

        QList<QString> resolutionIds;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &pId) {
            resolutionIds.append(pId);
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 101);

        runEventLoopWithTimeout(100);

        check("out-of-order: 2 resolutions emitted", resolutionIds.size() == 2, QString::number(resolutionIds.size()));
        if (resolutionIds.size() >= 2) {
            check("out-of-order: first is fast lower-priority p2", resolutionIds[0] == QStringLiteral("p2_low_fast"));
            check("out-of-order: second supersedes with higher-priority p1", resolutionIds[1] == QStringLiteral("p1_high_slow"));
        }
        check("out-of-order: currentProviderId is p1", pipeline.currentProviderId() == QStringLiteral("p1_high_slow"));
    }

    // Test 7: Bounded concurrency (max 2 in flight)
    {
        LyricsPipelineManager pipeline;
        s_concurrentInFlight = 0;
        s_maxObservedConcurrency = 0;

        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 20, false, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"), LyricsProvider::Capability::LineSync, 20, false, LyricsType::Line, &pipeline);
        auto *p3 = new FakeDelayedProvider(QStringLiteral("p3"), LyricsProvider::Capability::LineSync, 20, false, LyricsType::Line, &pipeline);
        auto *p4 = new FakeDelayedProvider(QStringLiteral("p4"), LyricsProvider::Capability::LineSync, 20, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.registerProvider(p3);
        pipeline.registerProvider(p4);
        pipeline.setProviderOrder({QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4")});
        pipeline.setMaxConcurrentLookups(2);

        bool resolved = false;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &) {
            resolved = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 102);

        runEventLoopWithTimeout(150);

        check("concurrency: max concurrent in flight <= 2", s_maxObservedConcurrency <= 2,
              QString::number(s_maxObservedConcurrency));
        check("concurrency: max concurrent observed was 2", s_maxObservedConcurrency == 2,
              QString::number(s_maxObservedConcurrency));
        check("concurrency: all providers were called eventually",
              p1->callCount() == 1 && p2->callCount() == 1 && p3->callCount() == 1 && p4->callCount() == 1);
        check("concurrency: resolved successfully", resolved);
    }

    // Test 8: Deadline timeout with provisional result retained and in-flight cancelled
    {
        LyricsPipelineManager pipeline;
        pipeline.setLookupTimeoutMs(30);

        // p1 delivers Line at 10ms
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 10, true, LyricsType::Line, &pipeline);
        // p2 is a slow WordSync provider taking 200ms
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"), LyricsProvider::Capability::WordSync, 200, true, LyricsType::Syllable, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("p1"), QStringLiteral("p2")});

        QList<QString> resolutions;
        bool allFailedEmitted = false;
        bool finishedEmitted = false;

        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &id) {
            resolutions.append(id);
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            allFailedEmitted = true;
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::lookupFinished,
                         [&](quint64) {
            finishedEmitted = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 103);

        runEventLoopWithTimeout(80);

        check("deadline-provisional: line was resolved", resolutions.size() == 1 && resolutions[0] == QStringLiteral("p1"));
        check("deadline-provisional: allFailed NEVER emitted when usable result exists", !allFailedEmitted);
        check("deadline-provisional: lookupFinished emitted", finishedEmitted);
        check("deadline-provisional: slow provider p2 was cancelled", p2->cancelCount() >= 1);
    }

    // Test 9: Deadline timeout with all failure
    {
        LyricsPipelineManager pipeline;
        pipeline.setLookupTimeoutMs(30);

        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 200, true, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"), LyricsProvider::Capability::WordSync, 200, true, LyricsType::Syllable, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);

        bool allFailedEmitted = false;
        bool finishedEmitted = false;

        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            allFailedEmitted = true;
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::lookupFinished,
                         [&](quint64) {
            finishedEmitted = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 104);

        runEventLoopWithTimeout(80);

        check("deadline-allfailed: allFailed emitted on timeout when no result", allFailedEmitted);
        check("deadline-allfailed: lookupFinished emitted", finishedEmitted);
        check("deadline-allfailed: p1 cancelled", p1->cancelCount() >= 1);
        check("deadline-allfailed: p2 cancelled", p2->cancelCount() >= 1);
    }

    // Test 10: Stale generation callbacks are ignored
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 40, true, LyricsType::Line, &pipeline);
        pipeline.registerProvider(p1);

        quint64 lastResolvedGen = 0;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64 gen, const Lyrics &, const QString &) {
            lastResolvedGen = gen;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 1);

        // Immediately supersede with generation 2 before gen 1 finishes
        pipeline.requestLyrics(meta, 2);

        runEventLoopWithTimeout(100);

        check("stale-gen: gen 1 was ignored, gen 2 was resolved", lastResolvedGen == 2, QString::number(lastResolvedGen));
    }

    // Test 11: Explicit cancel halts lookups and suppresses callbacks
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 30, true, LyricsType::Line, &pipeline);
        pipeline.registerProvider(p1);

        bool resolved = false;
        bool failed = false;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &) {
            resolved = true;
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            failed = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 200);

        // Cancel mid-flight
        pipeline.cancel();
        check("cancel: provider cancel() called", p1->cancelCount() >= 1);

        runEventLoopWithTimeout(60);

        check("cancel: no lyricsResolved emitted", !resolved);
        check("cancel: no allFailed emitted", !failed);
    }

    // Test 12: Reentrant requestLyrics call within lyricsResolved signal handler (delay 10ms)
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 10, true, LyricsType::Line, &pipeline);
        pipeline.registerProvider(p1);

        TrackMetadata meta2;
        meta2.title = QStringLiteral("Song 2");

        quint64 highestGenResolved = 0;
        int resolvedCount = 0;

        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64 gen, const Lyrics &, const QString &) {
            ++resolvedCount;
            highestGenResolved = gen;
            if (gen == 301) {
                // Reentrant call!
                pipeline.requestLyrics(meta2, 302);
            }
        });

        TrackMetadata meta1;
        meta1.title = QStringLiteral("Song 1");
        pipeline.requestLyrics(meta1, 301);

        runEventLoopWithTimeout(100);

        check("reentrant: handles reentrant request cleanly", resolvedCount == 2 && highestGenResolved == 302,
              QStringLiteral("resolvedCount=%1 highestGen=%2").arg(resolvedCount).arg(highestGenResolved));
    }

    // Test 13: Reentrant requestLyrics call with SYNCHRONOUS provider (delayMs = -1)
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1_sync"), LyricsProvider::Capability::LineSync, -1, true, LyricsType::Line, &pipeline);
        pipeline.registerProvider(p1);

        TrackMetadata meta2;
        meta2.title = QStringLiteral("Sync Song 2");

        quint64 highestGenResolved = 0;
        int resolvedCount = 0;

        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64 gen, const Lyrics &, const QString &) {
            ++resolvedCount;
            highestGenResolved = gen;
            if (gen == 501) {
                // Synchronous reentrant call!
                pipeline.requestLyrics(meta2, 502);
            }
        });

        TrackMetadata meta1;
        meta1.title = QStringLiteral("Sync Song 1");
        pipeline.requestLyrics(meta1, 501);

        check("sync-reentrant: handles synchronous reentrant request cleanly",
              resolvedCount == 2 && highestGenResolved == 502,
              QStringLiteral("resolvedCount=%1 highestGen=%2").arg(resolvedCount).arg(highestGenResolved));
    }

    // Test 14: Synchronous failure and fallback
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1_sync_fail"),
                                           LyricsProvider::Capability::LineSync, -1, false, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2_sync_succ"),
                                           LyricsProvider::Capability::LineSync, -1, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("p1_sync_fail"), QStringLiteral("p2_sync_succ")});

        bool resolved = false;
        QString resolvedId;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &id) {
            resolved = true;
            resolvedId = id;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Sync Fallback Song");
        pipeline.requestLyrics(meta, 503);

        check("sync-fallback: p1 called", p1->callCount() == 1);
        check("sync-fallback: p2 called", p2->callCount() == 1);
        check("sync-fallback: resolved by p2", resolved && resolvedId == QStringLiteral("p2_sync_succ"), resolvedId);
    }

    // Test 15: Empty candidates emits allFailed and lookupFinished, guarded on reentrant new generation
    {
        LyricsPipelineManager pipeline;
        // No providers registered

        QList<quint64> failedGens;
        QList<quint64> finishedGens;

        TrackMetadata meta2;
        meta2.title = QStringLiteral("Second Empty");

        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64 gen, MissingReason) {
            failedGens.append(gen);
            if (gen == 601) {
                pipeline.requestLyrics(meta2, 602);
            }
        });
        QObject::connect(&pipeline, &LyricsPipelineManager::lookupFinished,
                         [&](quint64 gen) {
            finishedGens.append(gen);
        });

        TrackMetadata meta1;
        meta1.title = QStringLiteral("First Empty");
        pipeline.requestLyrics(meta1, 601);

        check("empty-candidates: both failed properly", failedGens.size() == 2 && failedGens.contains(601) && failedGens.contains(602));
        check("empty-candidates: 601 lookupFinished was suppressed by reentrant 602",
              !finishedGens.contains(601) && finishedGens.contains(602),
              QStringLiteral("finished count: %1").arg(finishedGens.size()));
    }

    // Test 16: Invalidate caches calls all providers
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1"), LyricsProvider::Capability::LineSync, 0, true, LyricsType::Line, &pipeline);
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2"), LyricsProvider::Capability::WordSync, 0, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);

        TrackMetadata meta;
        meta.title = QStringLiteral("Song to Invalidate");

        pipeline.invalidateCaches(meta);

        check("invalidateCaches: p1 called", p1->invalidateCount() == 1);
        check("invalidateCaches: p2 called", p2->invalidateCount() == 1);
    }

    // Test 17: Cancel irrelevant lower-priority requests on word success
    {
        LyricsPipelineManager pipeline;
        // p1 (priority 0) WordSync fast (10ms)
        auto *p1 = new FakeDelayedProvider(QStringLiteral("p1_word"),
                                           LyricsProvider::Capability::WordSync,
                                           10, true, LyricsType::Syllable, &pipeline);
        // p2 (priority 1) WordSync slow (50ms)
        auto *p2 = new FakeDelayedProvider(QStringLiteral("p2_word_slow"),
                                           LyricsProvider::Capability::WordSync,
                                           50, true, LyricsType::Syllable, &pipeline);
        // p3 (priority 2) LineSync slow (50ms)
        auto *p3 = new FakeDelayedProvider(QStringLiteral("p3_line_slow"),
                                           LyricsProvider::Capability::LineSync,
                                           50, true, LyricsType::Line, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.registerProvider(p3);
        pipeline.setProviderOrder({QStringLiteral("p1_word"), QStringLiteral("p2_word_slow"), QStringLiteral("p3_line_slow")});

        QList<QString> resolvedIds;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &id) {
            resolvedIds.append(id);
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Song");
        pipeline.requestLyrics(meta, 400);

        runEventLoopWithTimeout(100);

        check("word-success-cancel: only p1 resolved", resolvedIds.size() == 1 && resolvedIds[0] == QStringLiteral("p1_word"));
        check("word-success-cancel: p2 was cancelled", p2->cancelCount() >= 1);
        check("word-success-cancel: p3 was not even called or cancelled", p3->callCount() == 0 || p3->cancelCount() >= 1);
    }

    if (s_failed == 0) {
        std::cout << "\nAll pipeline tests passed successfully.\n";
        return 0;
    }
    std::cout << "\n" << s_failed << " test(s) failed.\n";
    return 1;
}
