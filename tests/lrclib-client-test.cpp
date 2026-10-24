// SPDX-License-Identifier: MIT
#include "lrclib.h"
#include "http-test-server.h"
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

    return failures ? 1 : 0;
}
