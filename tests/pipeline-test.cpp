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
#include <QFile>
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

class MockProvider : public LyricsProvider
{
    Q_OBJECT

public:
    MockProvider(const QString &id, const QString &name, Capabilities caps, bool canHandleVal, bool succeed, QObject *parent = nullptr)
        : LyricsProvider(parent)
        , m_id(id)
        , m_name(name)
        , m_caps(caps)
        , m_canHandleVal(canHandleVal)
        , m_succeed(succeed)
    {
    }

    QString id() const override { return m_id; }
    QString displayName() const override { return m_name; }
    Capabilities capabilities() const override { return m_caps; }

    bool canHandle(const TrackMetadata &) const override { return m_canHandleVal; }

    void requestLyrics(const TrackMetadata &, quint64 generation) override
    {
        ++m_callCount;
        QTimer::singleShot(0, this, [this, generation] {
            if (m_succeed) {
                Lyrics l;
                l.type = LyricsType::Line;
                l.lines.append({QStringLiteral("Mock line"), 1000, 2000, {}});
                Q_EMIT lyricsLoaded(generation, l, m_id);
            } else {
                Q_EMIT lyricsFailed(generation, MissingReason::NotFound, m_id);
            }
        });
    }

    void cancel() override
    {
        ++m_cancelCount;
    }

    int callCount() const { return m_callCount; }
    int cancelCount() const { return m_cancelCount; }

private:
    QString m_id;
    QString m_name;
    Capabilities m_caps;
    bool m_canHandleVal;
    bool m_succeed;
    int m_callCount = 0;
    int m_cancelCount = 0;
};
} // namespace

#include "pipeline-test.moc"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Test 1: Priority order and successful fallback
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new MockProvider(QStringLiteral("prov1"), QStringLiteral("P1"),
                                    LyricsProvider::Capability::LineSync, true, false, &pipeline);
        auto *p2 = new MockProvider(QStringLiteral("prov2"), QStringLiteral("P2"),
                                    LyricsProvider::Capability::WordSync, true, true, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);
        pipeline.setProviderOrder({QStringLiteral("prov1"), QStringLiteral("prov2")});

        bool resolved = false;
        QString resolvedId;
        QObject::connect(&pipeline, &LyricsPipelineManager::lyricsResolved,
                         [&](quint64, const Lyrics &, const QString &providerId) {
            resolved = true;
            resolvedId = providerId;
            app.quit();
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Test Song");
        meta.artist = QStringLiteral("Test Artist");

        pipeline.requestLyrics(meta, 1);
        app.exec();

        check("pipeline: prov1 was called first", p1->callCount() == 1);
        check("pipeline: prov2 was called as fallback", p2->callCount() == 1);
        check("pipeline: resolved successfully", resolved);
        check("pipeline: resolved by prov2", resolvedId == QStringLiteral("prov2"), resolvedId);
    }

    // Test 2: Disabled provider is skipped
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new MockProvider(QStringLiteral("p1"), QStringLiteral("P1"),
                                    LyricsProvider::Capability::LineSync, true, true, &pipeline);
        auto *p2 = new MockProvider(QStringLiteral("p2"), QStringLiteral("P2"),
                                    LyricsProvider::Capability::WordSync, true, true, &pipeline);

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
            app.quit();
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Test Song");
        pipeline.requestLyrics(meta, 2);
        app.exec();

        check("disabled: p1 was not called", p1->callCount() == 0);
        check("disabled: p2 was called", p2->callCount() == 1);
        check("disabled: resolved by p2", resolvedId == QStringLiteral("p2"), resolvedId);
    }

    // Test 3: All fail
    {
        LyricsPipelineManager pipeline;
        auto *p1 = new MockProvider(QStringLiteral("p1"), QStringLiteral("P1"),
                                    LyricsProvider::Capability::LineSync, true, false, &pipeline);
        auto *p2 = new MockProvider(QStringLiteral("p2"), QStringLiteral("P2"),
                                    LyricsProvider::Capability::WordSync, true, false, &pipeline);

        pipeline.registerProvider(p1);
        pipeline.registerProvider(p2);

        bool failed = false;
        QObject::connect(&pipeline, &LyricsPipelineManager::allFailed,
                         [&](quint64, MissingReason) {
            failed = true;
            app.quit();
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Unknown");
        pipeline.requestLyrics(meta, 3);
        app.exec();

        check("all-fail: p1 called", p1->callCount() == 1);
        check("all-fail: p2 called", p2->callCount() == 1);
        check("all-fail: allFailed emitted", failed);
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

    if (s_failed == 0) {
        std::cout << "\nAll pipeline tests passed successfully.\n";
        return 0;
    }
    std::cout << "\n" << s_failed << " test(s) failed.\n";
    return 1;
}
