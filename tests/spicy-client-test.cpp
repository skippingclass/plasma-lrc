// SPDX-License-Identifier: MIT
#include "spicylyrics.h"
#include "http-test-server.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

namespace {
int failures = 0;
void check(const char *name, bool ok)
{
    std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    failures += !ok;
}
const QByteArray synced = R"({"Body":{"Type":"Line","source":"spotify","Content":[{"Text":"missing","StartTime":1,"EndTime":3}]}})";
const QByteArray syllableSynced = R"({"Body":{"Type":"Syllable","source":"spicy_lyrics","Content":[{"Lead":{"Syllables":[{"Text":"hello","StartTime":1,"EndTime":3}]}}]}})";

bool setFixtureFileTime(const QString &path, const QDateTime &time)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    const bool ok = file.setFileTime(time, QFileDevice::FileModificationTime);
    file.close();
    return ok;
}

bool writeFixtureFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const qint64 written = file.write(data);
    file.close();
    return written == data.size();
}

bool writeFixtureWithTime(const QString &path, const QByteArray &data, const QDateTime &time)
{
    if (!writeFixtureFile(path, data)) {
        failures++;
        return false;
    }
    if (!setFixtureFileTime(path, time)) {
        failures++;
        return false;
    }
    return true;
}
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
        // When application_suspended is returned, still-valid cache is not served until fresh 200 recovery
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        int missing = 0;
        MissingReason lastReason = MissingReason::NotFound;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason r) { ++missing; lastReason = r; });

        const QString trackA(22, '1');
        const QString trackB(22, '2');

        // Seed cached track A
        server.responses.append({200, synced, 0, {}});
        client.request(trackA);
        check("seed cached track A", waitFor([&] { return loaded == 1; }));

        // Request uncached track B receives application_suspended
        server.responses.append({403, R"({"Body":{"error":"application_suspended","message":"suspended"}})", 0, {}});
        client.request(trackB);
        check("request uncached B receives application_suspended", waitFor([&] { return missing == 1; }) && lastReason == MissingReason::Unreachable);

        // Request track A must not emit cached loaded while suspended
        client.request(trackA);
        check("request A must not emit cached loaded while suspended", waitFor([&] { return missing == 2; }) && loaded == 1);
        check("cached bytes for track A remain on disk while suspended", QFile::exists(SpicyLyrics::cacheDirectory() + '/' + trackA + ".json"));

        // Explicit refresh + 200 recovers
        server.responses.append({200, synced, 0, {}});
        client.invalidate(trackB);
        client.request(trackB);
        check("explicit refresh with 200 recovers from suspension", waitFor([&] { return loaded == 2; }));

        // Then valid cache works
        const auto reqCount = server.requests.size();
        client.request(trackA);
        check("then valid cache works", waitFor([&] { return loaded == 3; }) && server.requests.size() == reqCount);
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
    {
        // Network responses are stamped with original acquisition time + applicable TTL
        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        Lyrics lineLyrics;
        Lyrics syllableLyrics;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &id, const Lyrics &l) {
            ++loaded;
            if (id == QString(22, 'G')) {
                lineLyrics = l;
            } else {
                syllableLyrics = l;
            }
        });

        // Test line-sync response (2-day TTL)
        const QString lineTrack(22, 'G');
        const qint64 lineReqStart = QDateTime::currentMSecsSinceEpoch();
        server.responses.append({200, synced, 0, {}});
        client.request(lineTrack);
        check("line sync loads from network", waitFor([&] { return loaded == 1; }));
        const qint64 lineReqEnd = QDateTime::currentMSecsSinceEpoch();
        const qint64 expectedLineMin = lineReqStart + 2LL * 24 * 60 * 60 * 1000;
        const qint64 expectedLineMax = lineReqEnd + 2LL * 24 * 60 * 60 * 1000;
        check("line sync stamped with 2-day validUntilMs",
              lineLyrics.validUntilMs >= expectedLineMin && lineLyrics.validUntilMs <= expectedLineMax);

        // Test syllable-sync response (30-day TTL)
        const QString sylTrack(22, 'H');
        const qint64 sylReqStart = QDateTime::currentMSecsSinceEpoch();
        server.responses.append({200, syllableSynced, 0, {}});
        client.request(sylTrack);
        check("syllable sync loads from network", waitFor([&] { return loaded == 2; }));
        const qint64 sylReqEnd = QDateTime::currentMSecsSinceEpoch();
        const qint64 expectedSylMin = sylReqStart + 30LL * 24 * 60 * 60 * 1000;
        const qint64 expectedSylMax = sylReqEnd + 30LL * 24 * 60 * 60 * 1000;
        check("syllable sync stamped with 30-day validUntilMs",
              syllableLyrics.validUntilMs >= expectedSylMin && syllableLyrics.validUntilMs <= expectedSylMax);
    }
    {
        // Original timestamp preserved: reads from disk must not slide original mtime or validUntilMs
        const QString track(22, 'G');
        const QString path = SpicyLyrics::cacheDirectory() + '/' + track + ".json";
        check("cache file exists on disk", QFile::exists(path));
        const QDateTime originalMtime = QFileInfo(path).lastModified();
        const qint64 originalValidUntil = originalMtime.toMSecsSinceEpoch() + 2LL * 24 * 60 * 60 * 1000;

        advanceEvents(20);

        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        Lyrics readLyrics;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &l) {
            ++loaded;
            readLyrics = l;
        });
        client.request(track);
        check("disk read loads cached lyrics", waitFor([&] { return loaded == 1; }));
        check("original file modification time preserved on read", QFileInfo(path).lastModified() == originalMtime);
        check("reads do not slide validUntilMs", readLyrics.validUntilMs == originalValidUntil);
    }
    {
        // Constructor eager cleanup: prunes old files and invalid future timestamps on startup
        const QString staleLineTrack(22, 'I');
        const QString staleLinePath = SpicyLyrics::cacheDirectory() + '/' + staleLineTrack + ".json";
        // Line sync older than 2 days (3 days ago)
        writeFixtureWithTime(staleLinePath, synced, QDateTime::currentDateTime().addDays(-3));

        const QString staleSylTrack(22, 'J');
        const QString staleSylPath = SpicyLyrics::cacheDirectory() + '/' + staleSylTrack + ".json";
        // Syllable sync older than 30 days (35 days ago)
        writeFixtureWithTime(staleSylPath, syllableSynced, QDateTime::currentDateTime().addDays(-35));

        const QString staleNegTrack(22, 'K');
        const QString staleNegPath = SpicyLyrics::cacheDirectory() + '/' + staleNegTrack + ".json";
        // Negative marker older than 1 day (26 hours ago)
        writeFixtureWithTime(staleNegPath, R"({"missing":true,"reason":"notfound"})", QDateTime::currentDateTime().addSecs(-26 * 3600));

        const QString futureTrack(22, 'L');
        const QString futurePath = SpicyLyrics::cacheDirectory() + '/' + futureTrack + ".json";
        // Invalid future timestamp (10 days in the future)
        writeFixtureWithTime(futurePath, syllableSynced, QDateTime::currentDateTime().addDays(10));

        const QString validSylTrack(22, 'M');
        const QString validSylPath = SpicyLyrics::cacheDirectory() + '/' + validSylTrack + ".json";
        // Valid syllable sync (10 days ago, valid for 30 days)
        writeFixtureWithTime(validSylPath, syllableSynced, QDateTime::currentDateTime().addDays(-10));

        // Construct client -> eager constructor cleanup runs
        SpicyLyrics client;
        check("constructor prunes expired line sync (>2d)", !QFile::exists(staleLinePath));
        check("constructor prunes expired syllable sync (>30d)", !QFile::exists(staleSylPath));
        check("constructor prunes expired negative cache marker (>1d)", !QFile::exists(staleNegPath));
        check("constructor prunes invalid future timestamp", !QFile::exists(futurePath));
        check("constructor preserves valid syllable sync (<=30d)", QFile::exists(validSylPath));
    }
    {
        // Expired offline data must not be served; stale file is removed
        const QString track(22, 'N');
        const QString path = SpicyLyrics::cacheDirectory() + '/' + track + ".json";
        writeFixtureWithTime(path, synced, QDateTime::currentDateTime().addDays(-3));

        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        int missing = 0;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason) { ++missing; });

        // No server response queued -> offline
        client.request(track);
        check("expired offline data is not served", waitFor([&] { return missing == 1; }) && loaded == 0);
        check("stale file deleted on access", !QFile::exists(path));
    }
    {
        // Expired memory entry is removed on access and not served
        const QString track(22, 'O');
        const QString path = SpicyLyrics::cacheDirectory() + '/' + track + ".json";
        // Modification time set so it expires in 40ms
        const QDateTime mtime = QDateTime::currentDateTime().addMSecs(- (2LL * 24 * 60 * 60 * 1000 - 40));
        writeFixtureWithTime(path, synced, mtime);

        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        int missing = 0;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason) { ++missing; });

        // First request loads into memory cache
        client.request(track);
        check("initial near-expiry loads into memory", waitFor([&] { return loaded == 1; }));

        // Delete disk file so only memory cache remains
        QFile::remove(path);
        check("disk file removed for memory-only test", !QFile::exists(path));

        // Advance events past the 40ms expiration
        advanceEvents(60);

        // Second request when expired in memory and offline
        client.request(track);
        check("expired memory entry is not served", waitFor([&] { return missing == 1; }) && loaded == 1);
    }
    {
        // Timer-driven near-expiry cleanup independent of 64 cache writes
        const QString track(22, 'P');
        const QString path = SpicyLyrics::cacheDirectory() + '/' + track + ".json";
        // Expiration in 60ms
        const QDateTime mtime = QDateTime::currentDateTime().addMSecs(- (2LL * 24 * 60 * 60 * 1000 - 60));
        writeFixtureWithTime(path, synced, mtime);

        SpicyLyrics client;
        check("near-expiry file initially on disk", QFile::exists(path));
        // Without any writes, the timer should clean up the expired file
        check("timer cleans up expired file without writes", waitFor([&] { return !QFile::exists(path); }, 1500));
    }
    {
        // Queued boundary handling: deadline crossing while callback is queued must not emit stale loaded
        const QString track(22, 'Q');
        const QString path = SpicyLyrics::cacheDirectory() + '/' + track + ".json";
        // Remaining lifetime of only 10ms
        const QDateTime mtime = QDateTime::currentDateTime().addMSecs(- (2LL * 24 * 60 * 60 * 1000 - 10));
        writeFixtureWithTime(path, synced, mtime);

        SpicyLyrics client;
        client.setKey("fixture-key");
        client.setEnabled(true);
        int loaded = 0;
        int missing = 0;
        QObject::connect(&client, &SpicyLyrics::loaded, [&](const QString &, const Lyrics &) { ++loaded; });
        QObject::connect(&client, &SpicyLyrics::missing, [&](const QString &, MissingReason) { ++missing; });

        // Request queues singleShot, but we sleep so deadline passes before event loop executes callback
        client.request(track);
        QThread::msleep(25);
        check("queued deadline crossing prevents stale loaded emission", waitFor([&] { return missing == 1; }) && loaded == 0);
    }
    {
        // Public parser leaves validUntilMs = 0
        Lyrics lyrics;
        check("default Lyrics validUntilMs == 0", lyrics.validUntilMs == 0);
        QString error;
        check("parse succeeds", SpicyLyrics::parse(synced, &lyrics, &error));
        check("parse leaves validUntilMs == 0", lyrics.validUntilMs == 0);
    }
    return failures ? 1 : 0;
}
