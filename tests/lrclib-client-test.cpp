// SPDX-License-Identifier: MIT
#include "lrclib.h"
#include "http-test-server.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
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
    {
        QJsonObject candidate{{"duration", 600.0}, {"syncedLyrics", "[00:01.00] line"}, {"plainLyrics", "line"}};
        Lyrics lyrics;
        check("reject ten-minute version of 200-second track", LrclibClient::scoreSearchMatch(candidate, 200000) < 0);
        check("reject incompatible exact and cached results", !LrclibClient::parseCandidate(candidate, &lyrics, 200000));
        candidate["duration"] = 202.0;
        check("allow small duration differences", LrclibClient::parseCandidate(candidate, &lyrics, 200000));
    }
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
    {
        HttpTestServer raceServer;
        raceServer.responses.append({404, "{}", 0, {}});
        raceServer.responses.append({200, R"({"duration":200,"syncedLyrics":"[00:01.00] current"})", 500, {}});
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
    return failures ? 1 : 0;
}
