// SPDX-License-Identifier: MIT
#include "lrclib.h"
#include "http-test-server.h"
#include "lyriclimits.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

namespace {
int failures = 0;
void check(const char *name, bool ok)
{
    std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    failures += !ok;
}
void cacheMissing(const QString &title)
{
    QDir().mkpath(LrclibClient::cacheDirectory());
    QFile file(LrclibClient::cacheDirectory() + '/' + LrclibClient::makeCacheKey(title, "Artist", 200000) + ".json");
    if (!file.open(QIODevice::WriteOnly)) qFatal("Could not write fixture");
    file.write("{\"missing\":true}");
}
}

int main(int argc, char **argv)
{
    QTemporaryDir storage;
    qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
    QCoreApplication app(argc, argv);
    HttpTestServer server;

    // 1. Title-only request and empty title failure
    {
        LrclibClient client(nullptr, server.apiBase());
        QString missing;
        QObject::connect(&client, &LrclibClient::missing, [&](const QString &key) { missing = key; });
        const QString key = client.request("Solo Track Title", {}, 180000);
        check("title-only request returns key before completion", !key.isEmpty() && missing.isEmpty());
        check("title-only lookup completes", waitFor([&] { return missing == key; }));
        check("title-only lookup queries by title", server.requests.first().contains("track_name=Solo"));
        missing.clear();
        const QString emptyKey = client.request({}, {}, 0);
        check("empty title failure is asynchronous", !emptyKey.isEmpty() && missing.isEmpty());
        check("empty title failure uses returned key", waitFor([&] { return missing == emptyKey; }));
    }

    // 2. Candidate duration scoring & identity validation
    {
        QJsonObject candidate{{"duration", 600.0}, {"syncedLyrics", "[00:01.00] line"}, {"plainLyrics", "line"}};
        Lyrics lyrics;
        check("public static scoreSearchMatch without expected args is duration-only (reject 600s)",
              LrclibClient::scoreSearchMatch(candidate, 200000) < 0);
        check("public static parseCandidate without expected args is duration-only (reject 600s)",
              !LrclibClient::parseCandidate(candidate, &lyrics, 200000));
        candidate["duration"] = 202.0;
        check("public static parseCandidate allows small duration differences",
              LrclibClient::parseCandidate(candidate, &lyrics, 200000));

        // Identity validation (never bypassed for tests)
        QJsonObject wrongTrack{{"trackName", "Wrong Song"}, {"artistName", "Coldplay"}, {"duration", 200.0}, {"syncedLyrics", "[00:01.00] line"}};
        QJsonObject wrongArtist{{"trackName", "Yellow"}, {"artistName", "Different Artist"}, {"duration", 200.0}, {"syncedLyrics", "[00:01.00] line"}};
        QJsonObject missingTrack{{"artistName", "Coldplay"}, {"duration", 200.0}, {"syncedLyrics", "[00:01.00] line"}};
        QJsonObject missingArtist{{"trackName", "Yellow"}, {"duration", 200.0}, {"syncedLyrics", "[00:01.00] line"}};
        QJsonObject rightCand{{"trackName", "Yellow"}, {"artistName", "Coldplay"}, {"duration", 200.0}, {"syncedLyrics", "[00:01.00] line"}};

        check("validateCandidateIdentity: reject wrong track with same artist and duration",
              !LrclibClient::validateCandidateIdentity(wrongTrack, "Yellow", "Coldplay"));
        check("validateCandidateIdentity: reject wrong artist with same title and duration",
              !LrclibClient::validateCandidateIdentity(wrongArtist, "Yellow", "Coldplay"));
        check("validateCandidateIdentity: reject missing candidate trackName",
              !LrclibClient::validateCandidateIdentity(missingTrack, "Yellow", "Coldplay"));
        check("validateCandidateIdentity: reject missing candidate artistName",
              !LrclibClient::validateCandidateIdentity(missingArtist, "Yellow", "Coldplay"));
        check("validateCandidateIdentity: accept correct candidate",
              LrclibClient::validateCandidateIdentity(rightCand, "Yellow", "Coldplay"));
    }

    // 3. Reentrant cancel/request behavior
    {
        cacheMissing("Initial");
        cacheMissing("Replacement");
        LrclibClient client(nullptr, server.apiBase());
        const QString initial = LrclibClient::makeCacheKey("Initial", "Artist", 200000);
        int initialDeliveries = 0;
        bool replacementDelivered = false;
        QObject::connect(&client, &LrclibClient::missing, [&](const QString &key) {
            if (key == initial) {
                ++initialDeliveries;
                client.request("Replacement", "Artist", 200000);
            } else {
                replacementDelivered = true;
            }
        });
        client.request("Initial", "Artist", 200000);
        client.cancel();
        client.request("Initial", "Artist", 200000);
        check("cancelled callback cannot deliver twice for same key", waitFor([&] { return replacementDelivered; }) && initialDeliveries == 1);
        check("reentrant replacement survives outer completion", replacementDelivered);
    }

    // 4. Delayed search race condition with realistic fixture
    {
        HttpTestServer raceServer;
        raceServer.responses.append({404, "{}", 0, {}});
        raceServer.responses.append({200, R"({"trackName":"Race","artistName":"Artist","duration":200,"syncedLyrics":"[00:01.00] current"})", 500, {}});
        LrclibClient client(nullptr, raceServer.apiBase());
        int loaded = 0;
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        client.request("Race", "Artist", 200000);
        check("first request reaches local server", waitFor([&] { return raceServer.requests.size() == 1; }));
        advanceEvents(40);
        client.cancel();
        client.request("Race", "Artist", 200000);
        check("replacement reaches local server", waitFor([&] { return raceServer.requests.size() == 2; }));
        advanceEvents(300);
        check("old delayed search cannot interfere with replacement", raceServer.requests.size() == 2);
        check("replacement completes once", waitFor([&] { return loaded == 1; }));
    }

    // 5. Loopback search regression: wrong-title, wrong-artist, missing-identity, and correct candidate
    {
        HttpTestServer searchServer;
        searchServer.responses.append({404, "{}", 0, {}}); // /api/get: 404

        // /api/search: array containing candidates with identical duration
        QJsonArray cands;
        // (a) Wrong title, same artist, same duration -> MUST BE SKIPPED
        QJsonObject c1{
            {"id", 1},
            {"trackName", "Completely Different Song"},
            {"artistName", "Coldplay"},
            {"duration", 200.0},
            {"syncedLyrics", "[00:01.00] wrong song"}
        };
        // (b) Same title, wrong artist, same duration -> MUST BE SKIPPED
        QJsonObject c2{
            {"id", 2},
            {"trackName", "Yellow"},
            {"artistName", "Someone Else"},
            {"duration", 200.0},
            {"syncedLyrics", "[00:01.00] wrong artist"}
        };
        // (c) Missing identity fields -> MUST BE SKIPPED
        QJsonObject c3{
            {"id", 3},
            {"duration", 200.0},
            {"syncedLyrics", "[00:01.00] no identity lyrics"}
        };
        // (d) Right candidate -> MUST BE SELECTED
        QJsonObject c4{
            {"id", 4},
            {"trackName", "Yellow"},
            {"artistName", "Coldplay"},
            {"duration", 201.0},
            {"syncedLyrics", "[00:01.00] correct yellow lyrics"}
        };
        cands.append(c1);
        cands.append(c2);
        cands.append(c3);
        cands.append(c4);
        searchServer.responses.append({200, QJsonDocument(cands).toJson(QJsonDocument::Compact), 0, {}});

        LrclibClient client(nullptr, searchServer.apiBase());
        Lyrics resultLyrics;
        QString loadedKey;
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &l) {
            loadedKey = key;
            resultLyrics = l;
        });

        const QString reqKey = client.request("Yellow", "Coldplay", 200000);
        check("loopback search completes with loaded signal", waitFor([&] { return loadedKey == reqKey; }));
        check("loopback search rejects wrong-title, wrong-artist, missing-identity, selecting only correct candidate",
              !resultLyrics.lines.isEmpty() && resultLyrics.lines.first().text == QStringLiteral("correct yellow lyrics"));
    }

    // 6. YouTube raw title + uploader normalization query loopback
    {
        HttpTestServer ytServer;
        ytServer.responses.append({200, R"({"trackName":"Yellow","artistName":"Coldplay","duration":200,"syncedLyrics":"[00:01.00] yt yellow"})", 0, {}});

        LrclibClient client(nullptr, ytServer.apiBase());
        QString loadedKey;
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &) {
            loadedKey = key;
        });

        client.invalidate("Yellow", "Coldplay", 200000);
        client.request("Coldplay - Yellow (Official Music Video) [4K]", "ColdplayVEVO", 200000, /*fromWebPage=*/true);
        check("YouTube raw request completes with normalized query", waitFor([&] { return !loadedKey.isEmpty() && !ytServer.requests.isEmpty(); }));
        check("YouTube raw metadata normalized in outgoing request query",
              !ytServer.requests.isEmpty() && ytServer.requests.first().contains("track_name=Yellow") && ytServer.requests.first().contains("artist_name=Coldplay"));
    }

    // 7. Disk bad cache rejection: mismatched cached candidate purged and fresh server data fetched
    {
        const QString badKey = LrclibClient::makeCacheKey("Target Title", "Target Artist", 200000);
        const QString diskPath = LrclibClient::cacheDirectory() + '/' + badKey + ".json";
        QDir().mkpath(LrclibClient::cacheDirectory());
        QFile file(diskPath);
        if (file.open(QIODevice::WriteOnly)) {
            QJsonObject badCachedCand{
                {"trackName", "Wrong Cached Title"},
                {"artistName", "Target Artist"},
                {"duration", 200.0},
                {"syncedLyrics", "[00:01.00] bad cached lyrics"}
            };
            file.write(QJsonDocument(badCachedCand).toJson(QJsonDocument::Compact));
            file.close();
        }

        HttpTestServer purgeServer;
        QJsonObject freshObj{
            {"trackName", "Target Title"},
            {"artistName", "Target Artist"},
            {"duration", 200.0},
            {"syncedLyrics", "[00:01.00] fresh server lyrics"}
        };
        purgeServer.responses.append({200, QJsonDocument(freshObj).toJson(QJsonDocument::Compact), 0, {}});

        LrclibClient client(nullptr, purgeServer.apiBase());
        Lyrics loadedLyrics;
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &, const Lyrics &l) {
            loadedLyrics = l;
        });

        client.request("Target Title", "Target Artist", 200000);
        check("disk bad cache entry with mismatched title is rejected and fresh data loaded",
              waitFor([&] { return !loadedLyrics.lines.isEmpty() && loadedLyrics.lines.first().text == QStringLiteral("fresh server lyrics"); }));
        check("bad cache file was replaced on disk", QFile::exists(diskPath));
    }

    // 8. Pipeline cancel() must NOT abort active manual candidate search
    {
        HttpTestServer candServer;
        candServer.responses.append({200, R"([{"id":77,"trackName":"Manual Song","artistName":"Artist","duration":200,"syncedLyrics":"[00:01.00] manual line"}])", 150, {}});

        LrclibClient client(nullptr, candServer.apiBase());
        QList<LyricCandidate> foundCands;
        bool searchFailed = false;
        QObject::connect(&client, &LrclibClient::candidatesFound, [&](quint64, const QList<LyricCandidate> &c) {
            foundCands = c;
        });
        QObject::connect(&client, &LrclibClient::candidateSearchFailed, [&](quint64, MissingReason, int) {
            searchFailed = true;
        });

        TrackMetadata meta;
        meta.title = QStringLiteral("Manual Song");
        meta.artist = QStringLiteral("Artist");
        meta.durationMs = 200000;

        client.searchCandidates(meta, 999);
        advanceEvents(30);
        // Simulate main pipeline calling cancel() while ChooseMatchWindow candidate search is in flight
        client.cancel();

        check("pipeline cancel() does not abort in-flight searchCandidates()",
              waitFor([&] { return !foundCands.isEmpty(); }, 600) && !searchFailed);
    }

    {
        HttpTestServer limitServer;
        LrclibClient client(nullptr, limitServer.apiBase());
        QString failedKey;
        QString loadedKey;
        MissingReason reason = MissingReason::NotFound;
        QObject::connect(&client, &LrclibClient::missing, [&](const QString &key, MissingReason value) {
            failedKey = key;
            reason = value;
        });
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &) { loadedKey = key; });
        QJsonObject result{{"trackName", "Parser limit fixture"}, {"syncedLyrics", QStringLiteral("[00:01.00]")
            + QString(LyricsLimits::kMaxLineLengthChars, QLatin1Char('x'))}};
        limitServer.responses.append({200, QJsonDocument(QJsonArray{result}).toJson(), 0, {}});
        limitServer.responses.append({200, "[]", 0, {}});
        const QString key = client.request("Parser limit fixture", {}, 0);
        const QString path = LrclibClient::cacheDirectory() + '/' + key + ".json";
        check("search parser limit is terminal when no later attempt loads", waitFor([&] { return failedKey == key; })
              && reason == MissingReason::SizeLimitExceeded && !QFile::exists(path));

        failedKey.clear();
        limitServer.responses.append({200, QJsonDocument(QJsonArray{result}).toJson(), 0, {}});
        limitServer.responses.append({503, {}, 0, {}});
        client.request("Parser limit fixture", {}, 0);
        check("search parser limit takes precedence over a later transient server error", waitFor([&] { return failedKey == key; })
              && reason == MissingReason::SizeLimitExceeded && !QFile::exists(path));

        result["syncedLyrics"] = "[00:01.00] recovered";
        result["plainLyrics"] = "recovered";
        limitServer.responses.append({200, QJsonDocument(QJsonArray{result}).toJson(), 0, {}});
        client.request("Parser limit fixture", {}, 0);
        check("search parser limit permits an immediate successful retry", waitFor([&] { return loadedKey == key; })
              && limitServer.requests.size() == 5 && QFile::exists(path));
    }

    {
        HttpTestServer malformedServer;
        LrclibClient client(nullptr, malformedServer.apiBase());
        QString failedKey;
        QString loadedKey;
        MissingReason reason = MissingReason::NotFound;
        QObject::connect(&client, &LrclibClient::missing, [&](const QString &key, MissingReason value) {
            failedKey = key;
            reason = value;
        });
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &) { loadedKey = key; });
        malformedServer.responses.append({200, "<html>Service temporarily unavailable</html>", 0, {}});
        malformedServer.responses.append({200, "[]", 0, {}});
        const QString key = client.request("Malformed search fixture", {}, 0);
        const QString path = LrclibClient::cacheDirectory() + '/' + key + ".json";
        check("malformed search response is a transient failure", waitFor([&] { return failedKey == key; })
              && reason == MissingReason::Unreachable && !QFile::exists(path));

        malformedServer.responses.append({200, R"([{"trackName":"Malformed search fixture","syncedLyrics":"[00:01.00]recovered","plainLyrics":"recovered"}])", 0, {}});
        client.request("Malformed search fixture", {}, 0);
        check("malformed search response permits immediate recovery", waitFor([&] { return loadedKey == key; })
              && malformedServer.requests.size() == 3 && QFile::exists(path));
    }

    {
        const int statuses[] = {429, 503, 403};
        for (const int status : statuses) {
            HttpTestServer errorServer;
            LrclibClient client(nullptr, errorServer.apiBase());
            QString failedKey;
            QString loadedKey;
            MissingReason reason = MissingReason::NotFound;
            QObject::connect(&client, &LrclibClient::missing, [&](const QString &key, MissingReason value) {
                failedKey = key;
                reason = value;
            });
            QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &) { loadedKey = key; });

            const QString title = QStringLiteral("Unavailable status %1 fixture").arg(status);
            errorServer.responses.append({status, {}, 0, {}});
            if (status == 403) {
                errorServer.responses.append({200, "[]", 0, {}});
            }
            const QString key = client.request(title, {}, 0);
            const QString cachePath = LrclibClient::cacheDirectory() + '/' + key + ".json";
            check(qPrintable(QStringLiteral("HTTP %1 stays unreachable and uncached after final attempt").arg(status)),
                  waitFor([&] { return failedKey == key; }) && reason == MissingReason::Unreachable && !QFile::exists(cachePath));

            const QJsonObject response{{"trackName", title}, {"syncedLyrics", "[00:01.00] recovered"}, {"plainLyrics", "recovered"}};
            errorServer.responses.append({200, QJsonDocument(QJsonArray{response}).toJson(), 0, {}});
            client.request(title, {}, 0);
            check(qPrintable(QStringLiteral("HTTP %1 permits immediate successful retry").arg(status)),
                  waitFor([&] { return loadedKey == key; }) && QFile::exists(cachePath));
        }
    }

    {
        HttpTestServer candidateServer;
        LrclibClient client(nullptr, candidateServer.apiBase());
        TrackMetadata meta;
        meta.title = QStringLiteral("Replacement fixture");
        QList<quint64> searches;
        QList<quint64> fetches;
        QList<quint64> obsoleteFailures;
        QObject::connect(&client, &LrclibClient::candidatesFound, [&](quint64 searchId, const QList<LyricCandidate> &candidates) {
            if (candidates.size() == 1 && candidates.front().id == QStringLiteral("78")) {
                searches.append(searchId);
            }
        });
        QObject::connect(&client, &LrclibClient::candidateSearchFailed, [&](quint64 id, MissingReason, int) { obsoleteFailures.append(id); });
        QObject::connect(&client, &LrclibClient::candidateLyricsLoaded, [&](quint64 fetchId, const Lyrics &lyrics) {
            if (lyrics.lines.size() == 1 && lyrics.lines.front().text == QStringLiteral("new line")) {
                fetches.append(fetchId);
            }
        });
        QObject::connect(&client, &LrclibClient::candidateLyricsFailed, [&](quint64 id, MissingReason) { obsoleteFailures.append(id); });
        candidateServer.responses.append({200, "[]", 120, {}});
        candidateServer.responses.append({200, R"([{"id":78,"trackName":"Replacement fixture","syncedLyrics":"[00:01.00]new line"}])", 0, {}});
        client.searchCandidates(meta, 81);
        check("candidate search in flight before replacement", waitFor([&] { return candidateServer.requests.size() == 1; }));
        client.searchCandidates(meta, 82);
        check("candidate search replacement emits only the current result", waitFor([&] { return searches.size() == 1; })
              && searches == QList<quint64>{82} && obsoleteFailures.isEmpty());

        candidateServer.responses.append({200, R"({"syncedLyrics":"[00:02.00]old line"})", 120, {}});
        candidateServer.responses.append({200, R"({"syncedLyrics":"[00:01.00]new line"})", 0, {}});
        client.fetchCandidate(QStringLiteral("77"), 83);
        check("candidate fetch in flight before replacement", waitFor([&] { return candidateServer.requests.size() == 3; }));
        client.fetchCandidate(QStringLiteral("78"), 84);
        check("candidate fetch replacement emits only the current result", waitFor([&] { return fetches.size() == 1; })
              && fetches == QList<quint64>{84} && obsoleteFailures.isEmpty());
        advanceEvents(140);
        check("candidate operations never create an automatic cache entry", searches.size() == 1 && fetches.size() == 1
              && obsoleteFailures.isEmpty() && !QFile::exists(LrclibClient::cacheDirectory() + '/'
                  + LrclibClient::makeCacheKey(meta) + ".json"));
    }

    return failures ? 1 : 0;
}
