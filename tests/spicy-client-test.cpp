// SPDX-License-Identifier: MIT
#include "spicylyrics.h"
#include "http-test-server.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

namespace {
int failures = 0;
void check(const char *name, bool ok)
{
    std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    failures += !ok;
}
const QByteArray synced = R"({"Body":{"Type":"Line","source":"spotify","Content":[{"Text":"missing","StartTime":1,"EndTime":3}]}})";
}

int main(int argc, char **argv)
{
    QTemporaryDir storage;
    qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
    QCoreApplication app(argc, argv);
    HttpTestServer server;
    qputenv("SPICY_LYRICS_API_ROOT", server.apiBase().toEncoded());
    {
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int missing = 0;
        MissingReason reason = MissingReason::NotFound;
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason r) { ++missing; reason = r; });
        server.responses.append({403, R"({"Body":{"error":"application_suspended","message":"suspended"}})", 0, {}});
        const QString track(22, 'A');
        client.request(track);
        check("suspended app completes as unavailable", waitFor([&] { return missing == 1; }) && reason == MissingReason::Unreachable);
        check("suspension diagnosis retained", client.lastHttpStatus() == 403 && client.lastErrorCode() == "application_suspended");
        check("403 does not poison negative cache", !QFile::exists(SpicyLyrics::cacheDirectory() + '/' + track + ".json"));
        const auto requestCount = server.requests.size();
        client.request(QString(22, 'B'));
        check("next song also completes during cooldown", waitFor([&] { return missing == 2; }));
        check("suspended app is not retried on every track", server.requests.size() == requestCount);
        int loaded = 0;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        server.responses.append({200, synced, 0, {}});
        client.invalidate(track);
        client.request(track);
        check("explicit refresh can recover after suspension lifted", waitFor([&] { return loaded == 1; }));
    }
    {
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int missing = 0;
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason) { ++missing; });
        const QString track(22, 'C');
        server.responses.append({200, "{\"Body\":{\"error\":\"temporary_failure\"}}", 0, {}});
        client.request(track);
        check("malformed success completes", waitFor([&] { return missing == 1; }));
        check("malformed reply is not cached as unsynchronized", client.lastErrorCode() == "invalid_response"
              && !QFile::exists(SpicyLyrics::cacheDirectory() + '/' + track + ".json"));
        server.responses.append({200, R"({"Body":{"Type":"Static","Content":[]}})", 0, {}});
        client.request(track);
        check("untimed response completes", waitFor([&] { return missing == 2; }));
        check("genuinely untimed response is negatively cached", QFile::exists(SpicyLyrics::cacheDirectory() + '/' + track + ".json"));
    }
    {
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int missing = 0;
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason) { ++missing; });
        server.responses.append({429, "{}", 0, "Retry-After: 60\r\n"});
        client.request(QString(22, 'E'));
        check("rate limit completes with HTTP diagnosis", waitFor([&] { return missing == 1; }) && client.lastHttpStatus() == 429);
        const auto count = server.requests.size();
        client.request(QString(22, 'F'));
        check("rate-limit cooldown completes without new request", waitFor([&] { return missing == 2; }) && server.requests.size() == count);
    }
    {
        // A lyric containing the word "missing" is not a negative marker.
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        const QString track(22, 'D');
        server.responses.append({200, synced, 0, {}});
        client.request(track);
        check("fresh timed response loads", waitFor([&] { return loaded == 1; }));
        SpicyLyrics cached;
        cached.setKey("fixture-key");
        cached.setEnabled(true);
        QObject::connect(&cached, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        const auto count = server.requests.size();
        cached.request(track);
        cached.cancel();
        cached.request(track);
        check("cached response survives cancelled request of same track", waitFor([&] { return loaded == 2; }));
        advanceEvents(20);
        check("cancelled cache delivery is discarded", loaded == 2 && server.requests.size() == count);
    }
    return failures ? 1 : 0;
}
