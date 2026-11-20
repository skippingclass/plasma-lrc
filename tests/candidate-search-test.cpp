/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "candidatesearchcontroller.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTemporaryDir>
#include <iostream>

class CandidateSearchControllerTest
{
public:
    static bool staleFlagsOnlyCoverDisplayedCandidates(const CandidateSearchController &controller)
    {
        return controller.m_candidateStaleMap.size() <= controller.rawCandidates().size();
    }
};

namespace
{
class ControlledProvider : public LyricsProvider
{
public:
    explicit ControlledProvider(QObject *parent, QString id = QStringLiteral("controlled"), bool synchronous = false)
        : LyricsProvider(parent)
        , m_id(id)
        , m_synchronous(synchronous)
    {
    }

    QString id() const override { return m_id; }
    QString displayName() const override { return id(); }
    Capabilities capabilities() const override { return Capability::SearchByTitleArtist; }
    bool canHandle(const TrackMetadata &) const override { return true; }
    bool supportsCandidateSearch() const override { return true; }

    void requestLyrics(const TrackMetadata &, quint64) override {}
    void cancel() override {}

    void searchCandidates(const TrackMetadata &, quint64 searchId) override
    {
        ++searches;
        activeSearch = searchId;
        if (m_synchronous) {
            reportCandidate(searchId);
        }
    }

    void reportCandidate(quint64 searchId, const QString &idSuffix = {})
    {
        LyricCandidate candidate;
        candidate.id = QStringLiteral("entry") + idSuffix;
        candidate.providerId = id();
        candidate.title = QStringLiteral("Track");
        Q_EMIT candidatesFound(searchId, {candidate}, id());
    }

    void cancelCandidateSearch(quint64 searchId) override
    {
        ++cancellations;
        // Aborting a provider operation can complete it synchronously.
        reportCandidate(searchId);
    }

    void fetchCandidateLyrics(const QString &, quint64 fetchId) override { activeFetch = fetchId; }

    void reportLyrics()
    {
        Lyrics lyrics;
        lyrics.type = LyricsType::Line;
        LyricLine line;
        line.text = QStringLiteral("Lyrics");
        line.startMs = 1000;
        line.endMs = 2000;
        lyrics.lines.append(line);
        Q_EMIT candidateLyricsLoaded(activeFetch, lyrics, id());
    }

    int searches = 0;
    int cancellations = 0;
    quint64 activeSearch = 0;
    quint64 activeFetch = 0;

private:
    QString m_id;
    bool m_synchronous;
};

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void start(CandidateSearchController &controller, const LyricsProvider &provider, const QString &title = QStringLiteral("Track"))
{
    TrackMetadata track;
    track.title = title;
    track.artist = QStringLiteral("Artist");
    controller.startSearch(track, {provider.id()}, {provider.id()});
}
}

int main(int argc, char **argv)
{
    QTemporaryDir testCache;
    qputenv("XDG_CACHE_HOME", testCache.path().toUtf8());
    QCoreApplication app(argc, argv);

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        start(controller, *provider);
        const quint64 cancelledSearch = provider->activeSearch;
        controller.cancel();
        provider->reportCandidate(cancelledSearch);

        check(provider->cancellations == 1, "cancel: forwards a single abort");
        check(controller.status() == QStringLiteral("idle"), "cancel: returns to idle");
        check(!controller.hasUsableResults(), "cancel: abort completion cannot publish a candidate");
        start(controller, *provider);
        check(provider->searches == 2, "cancel: abort completion cannot seed the next search from cache");
        provider->reportCandidate(provider->activeSearch);
        check(controller.status() == QStringLiteral("ready") && controller.rawCandidates().size() == 1,
              "cancel: a successful subsequent search still publishes its candidate");
    }

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        start(controller, *provider);
        check(QMetaObject::invokeMethod(&controller, "onDeadlineTimeout", Qt::DirectConnection),
              "deadline: invoke the active deadline handler");
        check(provider->cancellations == 1, "deadline: forwards a single abort");
        check(controller.status() == QStringLiteral("no_response"), "deadline: reports the absence of a response");
        check(!controller.hasUsableResults(), "deadline: abort completion cannot publish a candidate");
    }

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        QObject::connect(&controller, &CandidateSearchController::candidatesChanged, &controller, [&] {
            if (controller.hasUsableResults()) {
                controller.close();
            }
        });
        start(controller, *provider);
        provider->reportCandidate(provider->activeSearch);
        check(controller.status() == QStringLiteral("idle"), "close: a candidate notification cannot reopen the search");
    }

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        start(controller, *provider);
        provider->reportCandidate(provider->activeSearch);
        QObject::connect(&controller, &CandidateSearchController::candidatesChanged, &controller, [&] {
            controller.close();
        });
        start(controller, *provider);
        check(provider->searches == 1, "cache: reopening a fresh search does not query the provider");
        check(controller.status() == QStringLiteral("idle"), "close: a cached candidate notification cannot reopen the search");
    }

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        for (int track = 0; track < 3; ++track) {
            const QString suffix = QString::number(track);
            start(controller, *provider, QStringLiteral("Track ") + suffix);
            provider->reportCandidate(provider->activeSearch, suffix);
        }
        check(CandidateSearchControllerTest::staleFlagsOnlyCoverDisplayedCandidates(controller),
              "cache: stale flags do not retain candidates from previous tracks");
    }

    {
        CandidateSearchController controller;
        auto *cached = new ControlledProvider(&controller);
        auto *uncached = new ControlledProvider(&controller, QStringLiteral("uncached"));
        controller.registerProvider(cached);
        controller.registerProvider(uncached);
        start(controller, *cached);
        cached->reportCandidate(cached->activeSearch);
        QObject::connect(&controller, &CandidateSearchController::candidatesChanged, &controller, [&] {
            if (controller.hasUsableResults()) {
                controller.close();
            }
        });
        TrackMetadata track;
        track.title = QStringLiteral("Track");
        track.artist = QStringLiteral("Artist");
        const QStringList order{cached->id(), uncached->id()};
        controller.startSearch(track, order, order);
        check(controller.status() == QStringLiteral("idle") && uncached->searches == 0,
              "close: a partial cache hit cannot restart the search");
    }

    {
        CandidateSearchController controller;
        auto *sync = new ControlledProvider(&controller, QStringLiteral("sync"), true);
        auto *first = new ControlledProvider(&controller, QStringLiteral("first"));
        auto *second = new ControlledProvider(&controller, QStringLiteral("second"));
        auto *third = new ControlledProvider(&controller, QStringLiteral("third"));
        const QList sources{sync, first, second, third};
        QStringList order;
        for (auto *source : sources) {
            controller.registerProvider(source);
            order.append(source->id());
        }
        TrackMetadata track;
        track.title = QStringLiteral("Concurrent track");
        controller.startSearch(track, order, order);
        check(sync->searches == 1 && first->searches == 1 && second->searches == 1 && third->searches == 0,
              "scheduler: synchronous results preserve the two-request limit");
        first->reportCandidate(first->activeSearch);
        check(third->searches == 1, "scheduler: a freed slot starts the remaining provider");
    }

    {
        CandidateSearchController controller;
        auto *provider = new ControlledProvider(&controller);
        controller.registerProvider(provider);
        start(controller, *provider);
        provider->reportCandidate(provider->activeSearch);
        controller.useSelected();
        check(controller.isLoading() && provider->activeFetch != 0, "fetch: the selected candidate starts downloading");
        QObject::connect(&controller, &CandidateSearchController::statusChanged, &controller, [&] {
            if (controller.status() == QStringLiteral("ready")) {
                controller.close();
            }
        });
        int loaded = 0;
        QObject::connect(&controller, &CandidateSearchController::candidateLyricsLoaded, &controller, [&] { ++loaded; });
        provider->reportLyrics();
        check(controller.status() == QStringLiteral("idle") && loaded == 0,
              "fetch: closing on the terminal status prevents applying stale lyrics");
    }

    {
        CandidateSearchController controller;
        QStringList order;
        for (int index = 0; index < 3; ++index) {
            auto *provider = new ControlledProvider(&controller, QString::number(index));
            order.append(provider->id());
            controller.registerProvider(provider);
        }
        TrackMetadata track;
        track.title = QStringLiteral("Refresh track");
        controller.startSearch(track, order, order);
        controller.searchAgain();
        check(QMetaObject::invokeMethod(&controller, "onDeadlineTimeout", Qt::DirectConnection),
              "refresh deadline: invoke the active deadline handler");
        check(controller.status() == QStringLiteral("no_response"),
              "refresh deadline: waiting providers do not count as successful responses");
    }

    return failures == 0 ? 0 : 1;
}
