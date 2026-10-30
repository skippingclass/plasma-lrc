// SPDX-License-Identifier: MIT
#include "amllprovider.h"
#include "http-test-server.h"
#include "lrclib.h"
#include "lrclibprovider.h"
#include "lrcredprovider.h"
#include "spicylyricsprovider.h"
#include "ttmlparser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <cstdio>

namespace
{
int s_failures = 0;

void check(const char *name, bool ok)
{
    std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) {
        ++s_failures;
    }
}

const char *kValidTtml = R"(<?xml version="1.0" encoding="utf-8"?>
<tt xmlns="http://www.w3.org/ns/ttml">
  <body>
    <div>
      <p begin="00:01.000" end="00:03.000"><span>Hello</span> <span>world</span></p>
      <p begin="00:04.000" end="00:06.000"><span>Second</span> <span>line</span></p>
    </div>
  </body>
</tt>)";

const char *kValidLrc = "[00:01.00]Hello world\n[00:04.00]Second line\n";
} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir storage;
    qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
    QCoreApplication app(argc, argv);

    // =========================================================================
    // Suite 1: AMLL Provider
    // =========================================================================
    {
        HttpTestServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        AmllProvider amll(nullptr, base);

        TrackMetadata meta;
        meta.spotifyTrackId = QStringLiteral("spotify_track_transient_1");
        meta.title = QStringLiteral("Transient Test");
        meta.artist = QStringLiteral("Artist");
        meta.durationMs = 180000;

        // 1. Transient 500 error must NOT block subsequent request (no 24h negative cache)
        server.responses.append({500, "Internal Server Error"});

        bool failed = false;
        MissingReason failReason = MissingReason::NotFound;
        QObject::connect(&amll, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason r, const QString &) {
            failed = true;
            failReason = r;
        });

        amll.requestLyrics(meta, 1);
        check("amll: transient 500 error fails with Unreachable", waitFor([&] { return failed; }) && failReason == MissingReason::Unreachable);

        // Immediate retry: server returns 200 OK -> must succeed without being blocked by negative cache
        failed = false;
        bool loaded = false;
        QObject::connect(&amll, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &, const QString &) {
            loaded = true;
        });

        server.responses.append({200, kValidTtml});
        amll.requestLyrics(meta, 2);
        check("amll: immediate retry after transient error succeeds without 24h block", waitFor([&] { return loaded; }));

        // 2. Positive cache invalidation
        // File is currently in disk cache.
        const QString cachedFile = AmllProvider::cacheDirectory() + QStringLiteral("/") + meta.spotifyTrackId + QStringLiteral(".ttml");
        check("amll: disk cache file exists after successful load", QFile::exists(cachedFile));

        // Invalidate cache for current track
        amll.invalidateCache(meta);
        check("amll: invalidateCache removes disk cache file", !QFile::exists(cachedFile));

        // 3. 404 sets negative cache, and invalidateCache clears it
        TrackMetadata meta404;
        meta404.spotifyTrackId = QStringLiteral("spotify_track_404_test");
        meta404.title = QStringLiteral("Not Found Song");

        server.responses.append({404, "Not Found"}); // get by spotifyId
        server.responses.append({404, "Not Found"}); // fallback search by title
        failed = false;
        failReason = MissingReason::Unreachable;

        amll.requestLyrics(meta404, 3);
        check("amll: 404 fails with NotFound", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);

        // Subsequent request without invalidate fails immediately without contacting server
        failed = false;
        const int reqCountBefore = server.requests.size();
        amll.requestLyrics(meta404, 4);
        check("amll: negative cache hit fails immediately without network query", waitFor([&] { return failed; }) && server.requests.size() == reqCountBefore);

        // Invalidate negative cache
        amll.invalidateCache(meta404);

        // Next request contacts server and succeeds
        loaded = false;
        server.responses.append({200, kValidTtml});
        amll.requestLyrics(meta404, 5);
        check("amll: request after negative cache invalidation queries network and succeeds", waitFor([&] { return loaded; }));

        // 4. Do not clear unrelated tracks
        TrackMetadata metaA;
        metaA.spotifyTrackId = QStringLiteral("spotify_track_keep_a");
        TrackMetadata metaB;
        metaB.spotifyTrackId = QStringLiteral("spotify_track_keep_b");

        server.responses.append({200, kValidTtml});
        loaded = false;
        amll.requestLyrics(metaA, 6);
        waitFor([&] { return loaded; });

        server.responses.append({200, kValidTtml});
        loaded = false;
        amll.requestLyrics(metaB, 7);
        waitFor([&] { return loaded; });

        const QString fileA = AmllProvider::cacheDirectory() + QStringLiteral("/") + metaA.spotifyTrackId + QStringLiteral(".ttml");
        const QString fileB = AmllProvider::cacheDirectory() + QStringLiteral("/") + metaB.spotifyTrackId + QStringLiteral(".ttml");
        check("amll: both track A and B cached", QFile::exists(fileA) && QFile::exists(fileB));

        amll.invalidateCache(metaA);
        check("amll: track A cache removed", !QFile::exists(fileA));
        check("amll: unrelated track B cache preserved", QFile::exists(fileB));

        // 5. Obsolete replies not reapplied after cancel
        TrackMetadata metaSlow;
        metaSlow.spotifyTrackId = QStringLiteral("spotify_track_slow");
        server.responses.append({200, kValidTtml, 150}); // 150ms delay

        bool slowLoaded = false;
        auto conn = QObject::connect(&amll, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &, const QString &) {
            slowLoaded = true;
        });

        amll.requestLyrics(metaSlow, 8);
        amll.cancel();
        advanceEvents(200);

        const QString slowFile = AmllProvider::cacheDirectory() + QStringLiteral("/") + metaSlow.spotifyTrackId + QStringLiteral(".ttml");
        check("amll: cancelled slow reply not loaded", !slowLoaded);
        QObject::disconnect(conn);
        server.responses.clear();

        // 6. Search by title & artist when Spotify ID is missing
        TrackMetadata metaSearch;
        metaSearch.title = QStringLiteral("Search Song");
        metaSearch.artist = QStringLiteral("Search Artist");
        metaSearch.durationMs = 120000;

        const QString searchJson = QStringLiteral(
            "{\"status\":200,\"data\":{\"items\":[{\"id\":42,\"filename\":\"test.ttml\"}]}}");

        const QString wordTtml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div>"
            "<p begin=\"00:01.000\" end=\"00:03.000\"><span begin=\"00:01.000\" end=\"00:02.000\">Hello</span> <span begin=\"00:02.000\" end=\"00:03.000\">world</span></p>"
            "</div></body></tt>");

        QJsonObject rootObj;
        rootObj[QStringLiteral("status")] = 200;
        QJsonObject dataObj;
        dataObj[QStringLiteral("lyrics")] = wordTtml;
        rootObj[QStringLiteral("data")] = dataObj;
        const QByteArray getJson = QJsonDocument(rootObj).toJson();

        server.responses.append({200, searchJson.toUtf8()});
        server.responses.append({200, getJson});

        loaded = false;
        failed = false;
        Lyrics searchLyrics;
        auto connSearch = QObject::connect(&amll, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &lyr, const QString &) {
            loaded = true;
            searchLyrics = lyr;
        });

        amll.requestLyrics(metaSearch, 9);
        check("amll: search fallback succeeds for non-spotify track", waitFor([&] { return loaded; }));
        check("amll: search lyrics are word-synced", searchLyrics.type == LyricsType::Syllable);
        const QString searchCachedFile = AmllProvider::cacheDirectory() + QStringLiteral("/") + AmllProvider::makeCacheKey(metaSearch) + QStringLiteral(".ttml");
        check("amll: search track cached by hash key", QFile::exists(searchCachedFile));
        amll.invalidateCache(metaSearch);
        check("amll: search track invalidated", !QFile::exists(searchCachedFile));
        QObject::disconnect(connSearch);
    }

    // =========================================================================
    // Suite 2: lrc.red Provider
    // =========================================================================
    {
        HttpTestServer server;
        const QString base = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
        LrcRedProvider lrcred(nullptr, base);

        TrackMetadata metaIsrc;
        metaIsrc.isrc = QStringLiteral("USUM71703861");
        metaIsrc.title = QStringLiteral("Test Song");
        metaIsrc.durationMs = 200000;

        // 1. Transient 500 error does NOT poison negative cache
        server.responses.append({500, "Internal Server Error", 0, {}}); // ttml
        server.responses.append({500, "Internal Server Error", 0, {}}); // lrc fallback

        bool failed = false;
        MissingReason failReason = MissingReason::NotFound;
        QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason r, const QString &) {
            failed = true;
            failReason = r;
        });

        lrcred.requestLyrics(metaIsrc, 10);
        check("lrcred: transient 500 error fails with Unreachable", waitFor([&] { return failed; }) && failReason == MissingReason::Unreachable);

        // Immediate retry: server returns 200 OK -> must succeed without 24h block
        failed = false;
        bool loaded = false;
        QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &, const QString &) {
            loaded = true;
        });

        server.responses.append({200, kValidTtml, 0, {}});
        lrcred.requestLyrics(metaIsrc, 11);
        check("lrcred: immediate retry after transient error succeeds without 24h block", waitFor([&] { return loaded; }));

        // 2. Positive cache invalidation
        const QString cacheKey = LrcRedProvider::makeCacheKey(metaIsrc.title, metaIsrc.artist, metaIsrc.durationMs, metaIsrc.isrc);
        const QString cachedFile = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + cacheKey + QStringLiteral(".ttml");
        check("lrcred: disk cache file exists after successful load", QFile::exists(cachedFile));

        lrcred.invalidateCache(metaIsrc);
        check("lrcred: invalidateCache removes disk cache file", !QFile::exists(cachedFile));

        // Transient 429 error does NOT poison negative cache
        server.responses.append({429, "Too Many Requests", 0, {}}); // ttml
        server.responses.append({429, "Too Many Requests", 0, {}}); // lrc fallback
        failed = false;
        failReason = MissingReason::NotFound;
        lrcred.requestLyrics(metaIsrc, 100);
        check("lrcred: transient 429 error fails with Unreachable", waitFor([&] { return failed; }) && failReason == MissingReason::Unreachable);

        loaded = false;
        server.responses.append({200, kValidTtml, 0, {}});
        lrcred.requestLyrics(metaIsrc, 101);
        check("lrcred: immediate retry after 429 succeeds without 24h block", waitFor([&] { return loaded; }));
        lrcred.invalidateCache(metaIsrc);

        // Malformed TTML payload does NOT poison negative cache
        server.responses.append({200, "<tt>incomplete truncated xml", 0, {}});
        failed = false;
        failReason = MissingReason::NotFound;
        lrcred.requestLyrics(metaIsrc, 102);
        check("lrcred: malformed TTML fails with Unreachable", waitFor([&] { return failed; }) && failReason == MissingReason::Unreachable);

        loaded = false;
        server.responses.append({200, kValidTtml, 0, {}});
        lrcred.requestLyrics(metaIsrc, 103);
        check("lrcred: immediate retry after malformed TTML succeeds without 24h block", waitFor([&] { return loaded; }));
        lrcred.invalidateCache(metaIsrc);

        // 3. 404 sets negative cache, and invalidateCache clears it
        TrackMetadata meta404;
        meta404.isrc = QStringLiteral("US4040000001");
        meta404.title = QStringLiteral("Missing Song");

        server.responses.append({404, "Not Found"}); // ttml
        server.responses.append({404, "Not Found"}); // lrc fallback
        failed = false;
        failReason = MissingReason::Unreachable;

        lrcred.requestLyrics(meta404, 12);
        check("lrcred: 404 fails with NotFound", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);

        // Immediate retry without invalidate hits negative cache
        failed = false;
        const int reqCountBefore = server.requests.size();
        lrcred.requestLyrics(meta404, 13);
        check("lrcred: negative cache hit fails immediately without network query", waitFor([&] { return failed; }) && server.requests.size() == reqCountBefore);

        // Invalidate negative cache
        lrcred.invalidateCache(meta404);

        loaded = false;
        server.responses.append({200, kValidTtml});
        lrcred.requestLyrics(meta404, 14);
        check("lrcred: request after negative cache invalidation queries network and succeeds", waitFor([&] { return loaded; }));

        // 4. Candidate search validation: reject wrong song with compatible duration
        TrackMetadata searchMeta;
        searchMeta.title = QStringLiteral("Blinding Lights");
        searchMeta.artist = QStringLiteral("The Weeknd");
        searchMeta.durationMs = 200000;

        // Search returns a hit with matching duration (200s), but WRONG TITLE ("Different Track")
        QJsonObject wrongCandidate;
        wrongCandidate[QStringLiteral("title")] = QStringLiteral("Completely Different Song");
        wrongCandidate[QStringLiteral("artist")] = QStringLiteral("Someone Else");
        wrongCandidate[QStringLiteral("duration")] = 200.0;
        QJsonObject wrongFiles;
        wrongFiles[QStringLiteral("ttml")] = QStringLiteral("/s/WRONG.ttml");
        wrongCandidate[QStringLiteral("files")] = wrongFiles;

        QJsonObject searchRootWrong;
        QJsonArray hitsWrong;
        hitsWrong.append(wrongCandidate);
        searchRootWrong[QStringLiteral("hits")] = hitsWrong;

        failed = false;
        failReason = MissingReason::Unreachable;
        server.responses.append({200, QJsonDocument(searchRootWrong).toJson()});

        lrcred.requestLyrics(searchMeta, 15);
        check("lrcred: candidate with matching duration but different title is rejected", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);

        // Clear negative cache from the rejected search
        lrcred.invalidateCache(searchMeta);

        // Now search returns a matching candidate (right title and compatible duration)
        QJsonObject rightCandidate;
        rightCandidate[QStringLiteral("title")] = QStringLiteral("Blinding Lights");
        rightCandidate[QStringLiteral("artist")] = QStringLiteral("The Weeknd");
        rightCandidate[QStringLiteral("duration")] = 201.0;
        QJsonObject rightFiles;
        rightFiles[QStringLiteral("ttml")] = QStringLiteral("/s/RIGHT.ttml");
        rightCandidate[QStringLiteral("files")] = rightFiles;

        QJsonObject searchRootRight;
        QJsonArray hitsRight;
        hitsRight.append(rightCandidate);
        searchRootRight[QStringLiteral("hits")] = hitsRight;

        loaded = false;
        server.responses.append({200, QJsonDocument(searchRootRight).toJson()}); // search reply
        server.responses.append({200, kValidTtml}); // file download reply

        lrcred.requestLyrics(searchMeta, 16);
        check("lrcred: candidate with matching title and compatible duration is accepted", waitFor([&] { return loaded; }));

        // 5. Unknown duration: candidate with different title must be rejected
        TrackMetadata unknownDurMeta;
        unknownDurMeta.title = QStringLiteral("Save Your Tears");
        unknownDurMeta.artist = QStringLiteral("The Weeknd");
        unknownDurMeta.durationMs = 0; // unknown

        QJsonObject unkWrongCand;
        unkWrongCand[QStringLiteral("title")] = QStringLiteral("After Hours");
        unkWrongCand[QStringLiteral("duration")] = 0.0;
        unkWrongCand[QStringLiteral("files")] = wrongFiles;

        QJsonObject searchRootUnk;
        QJsonArray hitsUnk;
        hitsUnk.append(unkWrongCand);
        searchRootUnk[QStringLiteral("hits")] = hitsUnk;

        failed = false;
        failReason = MissingReason::Unreachable;
        server.responses.append({200, QJsonDocument(searchRootUnk).toJson(), 0, {}});

        lrcred.requestLyrics(unknownDurMeta, 17);
        check("lrcred: unknown duration search rejects mismatched title", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);

        // 6. Regression loopback fixtures:
        // (a) different song same artist/duration (reject)
        TrackMetadata metaDiffSong;
        metaDiffSong.title = QStringLiteral("Viva La Vida");
        metaDiffSong.artist = QStringLiteral("Coldplay");
        metaDiffSong.durationMs = 242000;

        QJsonObject diffSongCand;
        diffSongCand[QStringLiteral("title")] = QStringLiteral("Speed of Sound");
        diffSongCand[QStringLiteral("artist")] = QStringLiteral("Coldplay");
        diffSongCand[QStringLiteral("duration")] = 242.0;
        QJsonObject diffSongFiles;
        diffSongFiles[QStringLiteral("ttml")] = QStringLiteral("/s/DIFF_SONG.ttml");
        diffSongCand[QStringLiteral("files")] = diffSongFiles;

        QJsonObject rootDiffSong;
        QJsonArray hitsDiffSong;
        hitsDiffSong.append(diffSongCand);
        rootDiffSong[QStringLiteral("hits")] = hitsDiffSong;

        failed = false;
        failReason = MissingReason::Unreachable;
        server.responses.append({200, QJsonDocument(rootDiffSong).toJson(), 0, {}});

        lrcred.requestLyrics(metaDiffSong, 20);
        check("lrcred: different song same artist and duration is rejected",
              waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);
        lrcred.invalidateCache(metaDiffSong);

        // (b) same title wrong artist (reject)
        TrackMetadata metaWrongArtist;
        metaWrongArtist.title = QStringLiteral("Yellow");
        metaWrongArtist.artist = QStringLiteral("Coldplay");
        metaWrongArtist.durationMs = 269000;

        QJsonObject wrongArtistCand;
        wrongArtistCand[QStringLiteral("title")] = QStringLiteral("Yellow");
        wrongArtistCand[QStringLiteral("artist")] = QStringLiteral("Random Singer");
        wrongArtistCand[QStringLiteral("duration")] = 269.0;
        QJsonObject wrongArtistFiles;
        wrongArtistFiles[QStringLiteral("ttml")] = QStringLiteral("/s/WRONG_ARTIST.ttml");
        wrongArtistCand[QStringLiteral("files")] = wrongArtistFiles;

        QJsonObject rootWrongArtist;
        QJsonArray hitsWrongArtist;
        hitsWrongArtist.append(wrongArtistCand);
        rootWrongArtist[QStringLiteral("hits")] = hitsWrongArtist;

        failed = false;
        failReason = MissingReason::Unreachable;
        server.responses.append({200, QJsonDocument(rootWrongArtist).toJson(), 0, {}});

        lrcred.requestLyrics(metaWrongArtist, 21);
        check("lrcred: same title wrong artist is rejected",
              waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);
        lrcred.invalidateCache(metaWrongArtist);

        // (c) exact proper result (accept) with closest-duration selection
        TrackMetadata metaExact;
        metaExact.title = QStringLiteral("Fix You");
        metaExact.artist = QStringLiteral("Coldplay");
        metaExact.durationMs = 295000;

        QJsonObject exactCand;
        exactCand[QStringLiteral("title")] = QStringLiteral("Fix You");
        exactCand[QStringLiteral("artist")] = QStringLiteral("Coldplay");
        exactCand[QStringLiteral("duration")] = 295.0; // exact duration match
        QJsonObject exactFiles;
        exactFiles[QStringLiteral("ttml")] = QStringLiteral("/s/EXACT.ttml");
        exactCand[QStringLiteral("files")] = exactFiles;

        QJsonObject rootExact;
        QJsonArray hitsExact;
        hitsExact.append(exactCand);
        rootExact[QStringLiteral("hits")] = hitsExact;

        loaded = false;
        server.responses.append({200, QJsonDocument(rootExact).toJson(), 0, {}}); // search
        server.responses.append({200, kValidTtml, 0, {}});                        // file download

        lrcred.requestLyrics(metaExact, 22);
        check("lrcred: exact proper result is accepted", waitFor([&] { return loaded; }));
        lrcred.invalidateCache(metaExact);

        // (d) ISRC fallback: TTML 404 followed by 200 kValidLrc -> loads Line, caches .lrc
        TrackMetadata metaLrcFallback;
        metaLrcFallback.isrc = QStringLiteral("USLRC0000001");
        metaLrcFallback.title = QStringLiteral("LRC Fallback Track");
        metaLrcFallback.durationMs = 180000;

        server.responses.append({404, "Not Found", 0, {}}); // ttml 404
        server.responses.append({200, kValidLrc, 0, {}});    // lrc 200

        loaded = false;
        Lyrics lrcResult;
        auto lrcConn = QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &l, const QString &) {
            loaded = true;
            lrcResult = l;
        });

        lrcred.requestLyrics(metaLrcFallback, 23);
        check("lrcred: TTML 404 fallback to LRC 200 loads Line lyrics",
              waitFor([&] { return loaded; }) && lrcResult.type == LyricsType::Line);
        QObject::disconnect(lrcConn);

        const QString lrcKey = LrcRedProvider::makeCacheKey(metaLrcFallback.title, metaLrcFallback.artist, metaLrcFallback.durationMs, metaLrcFallback.isrc);
        const QString lrcPath = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + lrcKey + QStringLiteral(".lrc");
        check("lrcred: fallback LRC cached on disk as .lrc file", QFile::exists(lrcPath));

        // Next request for same track is a cache hit without querying network
        loaded = false;
        const int reqCountLrc = server.requests.size();
        lrcred.requestLyrics(metaLrcFallback, 24);
        check("lrcred: subsequent request hits .lrc disk cache without network query",
              waitFor([&] { return loaded; }) && server.requests.size() == reqCountLrc);

        // Invalidate removes .lrc
        lrcred.invalidateCache(metaLrcFallback);
        check("lrcred: invalidateCache removes .lrc disk cache file", !QFile::exists(lrcPath));

        // 7. Do not clear unrelated tracks
        TrackMetadata metaRedA;
        metaRedA.isrc = QStringLiteral("USREDA000001");
        TrackMetadata metaRedB;
        metaRedB.isrc = QStringLiteral("USREDB000002");

        server.responses.append({200, kValidTtml, 0, {}});
        loaded = false;
        lrcred.requestLyrics(metaRedA, 18);
        waitFor([&] { return loaded; });

        server.responses.append({200, kValidTtml, 0, {}});
        loaded = false;
        lrcred.requestLyrics(metaRedB, 19);
        waitFor([&] { return loaded; });

        const QString keyA = LrcRedProvider::makeCacheKey(metaRedA.title, metaRedA.artist, metaRedA.durationMs, metaRedA.isrc);
        const QString keyB = LrcRedProvider::makeCacheKey(metaRedB.title, metaRedB.artist, metaRedB.durationMs, metaRedB.isrc);
        const QString fileRedA = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + keyA + QStringLiteral(".ttml");
        const QString fileRedB = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + keyB + QStringLiteral(".ttml");

        check("lrcred: both track A and B cached", QFile::exists(fileRedA) && QFile::exists(fileRedB));
        lrcred.invalidateCache(metaRedA);
        check("lrcred: track A cache removed", !QFile::exists(fileRedA));
        check("lrcred: unrelated track B cache preserved", QFile::exists(fileRedB));
    }

    // =========================================================================
    // Suite 3: LRCLIB Provider & Client Invalidate
    // =========================================================================
    {
        HttpTestServer server;
        LrclibClient client(nullptr, server.apiBase());

        // 1. Positive cache invalidate in LrclibClient
        server.responses.append({200, R"({"trackName":"Test Title","artistName":"Test Artist","duration":200,"syncedLyrics":"[00:01.00] lrclib test"})"});
        QString loadedKey;
        QObject::connect(&client, &LrclibClient::loaded, [&](const QString &key, const Lyrics &) {
            loadedKey = key;
        });

        const QString reqKey = client.request("Test Title", "Test Artist", 200000);
        check("lrclib: request succeeds and caches", waitFor([&] { return !loadedKey.isEmpty(); }));

        const QString diskPath = LrclibClient::cacheDirectory() + QLatin1Char('/') + reqKey + QStringLiteral(".json");
        check("lrclib: disk cache file exists", QFile::exists(diskPath));

        // Invalidate current track
        client.invalidate("Test Title", "Test Artist", 200000);
        check("lrclib: invalidate removes disk cache file", !QFile::exists(diskPath));

        // 2. Negative cache invalidate
        QDir().mkpath(LrclibClient::cacheDirectory());
        QFile negFile(diskPath);
        if (negFile.open(QIODevice::WriteOnly)) {
            negFile.write("{\"missing\":true}");
            negFile.close();
        }
        check("lrclib: negative cache file created", QFile::exists(diskPath));

        client.invalidate("Test Title", "Test Artist", 200000);
        check("lrclib: invalidate removes negative cache file", !QFile::exists(diskPath));

        // 3. Unrelated tracks not cleared
        const QString otherKey = LrclibClient::makeCacheKey("Other Song", "Artist", 150000);
        const QString otherPath = LrclibClient::cacheDirectory() + QLatin1Char('/') + otherKey + QStringLiteral(".json");
        QFile otherFile(otherPath);
        if (otherFile.open(QIODevice::WriteOnly)) {
            otherFile.write("{\"trackName\":\"Other Song\",\"artistName\":\"Artist\",\"duration\":150,\"syncedLyrics\":\"[00:01.00] other\"}");
            otherFile.close();
        }

        client.invalidate("Test Title", "Test Artist", 200000);
        check("lrclib: unrelated track disk cache preserved", QFile::exists(otherPath));

        // 4. LrclibProvider invalidateCache
        LrclibProvider provider;
        TrackMetadata pMeta;
        pMeta.title = QStringLiteral("Provider Track");
        pMeta.artist = QStringLiteral("Provider Artist");
        pMeta.durationMs = 210000;

        const QString pKey = LrclibClient::makeCacheKey(pMeta.title, pMeta.artist, pMeta.durationMs);
        const QString pPath = LrclibClient::cacheDirectory() + QLatin1Char('/') + pKey + QStringLiteral(".json");
        QFile pFile(pPath);
        if (pFile.open(QIODevice::WriteOnly)) {
            pFile.write("{\"trackName\":\"Provider Track\",\"artistName\":\"Provider Artist\",\"duration\":210,\"syncedLyrics\":\"[00:01.00] provider line\"}");
            pFile.close();
        }

        provider.invalidateCache(pMeta);
        check("lrclib provider: invalidateCache removes disk cache file", !QFile::exists(pPath));
    }

    // =========================================================================
    // Suite 4: SpicyLyrics Provider Invalidate
    // =========================================================================
    {
        SpicyLyricsProvider spicyProvider;
        TrackMetadata spicyMeta;
        spicyMeta.spotifyTrackId = QStringLiteral("4cOdK2wGLETKBW3PvgPWqT");

        const QString spicyPath = SpicyLyrics::cacheDirectory() + QLatin1Char('/') + spicyMeta.spotifyTrackId + QStringLiteral(".json");
        QDir().mkpath(SpicyLyrics::cacheDirectory());
        QFile file(spicyPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(R"({"Body":{"Type":"Line","Lines":[{"Time":1000,"Words":"Line"}]}})");
            file.close();
        }
        check("spicy: fixture cache file created", QFile::exists(spicyPath));

        // Unrelated file
        const QString otherId = QStringLiteral("1111111111111111111111");
        const QString otherPath = SpicyLyrics::cacheDirectory() + QLatin1Char('/') + otherId + QStringLiteral(".json");
        QFile otherFile(otherPath);
        if (otherFile.open(QIODevice::WriteOnly)) {
            otherFile.write(R"({"Body":{"Type":"Line","Lines":[{"Time":1000,"Words":"Other"}]}})");
            otherFile.close();
        }

        spicyProvider.invalidateCache(spicyMeta);
        check("spicy provider: invalidateCache removes track cache file", !QFile::exists(spicyPath));
        check("spicy provider: unrelated track cache preserved", QFile::exists(otherPath));
    }

    // =========================================================================
    // Suite 5: Unsynced TTML Rejection and clearAllCaches
    // =========================================================================
    {
        HttpTestServer server;

        // 1. LrcRedProvider rejects unsynced TTML
        LrcRedProvider lrcred(nullptr, server.apiBase().toString());
        const QString unsyncedTtml = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:lrc=\"http://lrc.red/lyric-ttml-internal\" lrc:timing=\"None\">\n"
            "  <body><div><p>Unsynced Line</p></div></body>\n"
            "</tt>");

        server.responses.clear();
        server.responses.append({200, unsyncedTtml.toUtf8(), 0, {}});

        TrackMetadata meta;
        meta.isrc = QStringLiteral("GBTEST123456");
        meta.title = QStringLiteral("Unsynced Test");
        meta.artist = QStringLiteral("Test Artist");

        bool lrcredFailed = false;
        MissingReason failReason = MissingReason::NotFound;
        bool lrcredLoaded = false;

        QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason r, const QString &) {
            lrcredFailed = true;
            failReason = r;
        });
        QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &, const QString &) {
            lrcredLoaded = true;
        });

        lrcred.requestLyrics(meta, 1);
        check("lrcred rejects unsynced: failed emitted", waitFor([&] { return lrcredFailed; }));
        check("lrcred rejects unsynced: not loaded", !lrcredLoaded);
        check("lrcred rejects unsynced: reason is Unsynced", failReason == MissingReason::Unsynced);

        // 2. clearAllCaches sweeps provider directories
        const QString amllDummy = AmllProvider::cacheDirectory() + QStringLiteral("/dummy.ttml");
        const QString lrcredDummy = LrcRedProvider::cacheDirectory() + QStringLiteral("/dummy.ttml");
        const QString lrclibDummy = LrclibClient::cacheDirectory() + QStringLiteral("/dummy.json");
        const QString spicyDummy = SpicyLyrics::cacheDirectory() + QStringLiteral("/dummy.json");

        QDir().mkpath(AmllProvider::cacheDirectory());
        QDir().mkpath(LrcRedProvider::cacheDirectory());
        QDir().mkpath(LrclibClient::cacheDirectory());
        QDir().mkpath(SpicyLyrics::cacheDirectory());

        for (const QString &path : {amllDummy, lrcredDummy, lrclibDummy, spicyDummy}) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("test");
                f.close();
            }
            check("suite 5: created dummy file", QFile::exists(path));
        }

        AmllProvider amll;
        LrcRedProvider lrcredProv;
        LrclibProvider lrclibProv;
        SpicyLyricsProvider spicyProv;

        amll.clearAllCaches();
        lrcredProv.clearAllCaches();
        lrclibProv.clearAllCaches();
        spicyProv.clearAllCaches();

        check("suite 5: clearAllCaches removes amll dummy", !QFile::exists(amllDummy));
        check("suite 5: clearAllCaches removes lrcred dummy", !QFile::exists(lrcredDummy));
        check("suite 5: clearAllCaches removes lrclib dummy", !QFile::exists(lrclibDummy));
        check("suite 5: clearAllCaches removes spicy dummy", !QFile::exists(spicyDummy));
    }

    if (s_failures == 0) {
        std::printf("\nAll provider cache tests passed successfully.\n");
        return 0;
    }
    std::printf("\n%d provider cache test(s) failed.\n", s_failures);
    return 1;
}
