// SPDX-License-Identifier: MIT
#include "amllprovider.h"
#include "customprovider.h"
#include "http-test-server.h"
#include "kugouprovider.h"
#include "localprovider.h"
#include "lrclib.h"
#include "lrclibprovider.h"
#include "lrcredprovider.h"
#include "lyriclimits.h"
#include "spicylyricsprovider.h"
#include "ttmlparser.h"
#include "trackname.h"
#include "unisonprovider.h"

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

struct ProviderResult {
    QList<quint64> generations;
    QString providerId;
    Lyrics lyrics;
    MissingReason reason = MissingReason::NotFound;

    void observe(LyricsProvider &provider)
    {
        QObject::connect(&provider, &LyricsProvider::lyricsLoaded, [&](quint64 generation, const Lyrics &value, const QString &id) {
            generations.append(generation);
            providerId = id;
            lyrics = value;
        });
        QObject::connect(&provider, &LyricsProvider::lyricsFailed, [&](quint64 generation, MissingReason value, const QString &id) {
            generations.append(generation);
            providerId = id;
            lyrics = {};
            reason = value;
        });
    }
};
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

        // 5. Corrupted / Malformed JSON in search does NOT poison negative cache
        TrackMetadata metaMalformed;
        metaMalformed.title = QStringLiteral("Malformed Test");
        metaMalformed.artist = QStringLiteral("Corrupt Band");
        metaMalformed.durationMs = 180000;

        server.responses.append({200, "{\"code\":200, \"data\": INVALID_JSON", 0, {}});
        failed = false;
        failReason = MissingReason::NotFound;
        auto connFail = QObject::connect(&amll, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason r, const QString &) {
            failed = true;
            failReason = r;
        });

        amll.requestLyrics(metaMalformed, 12);
        check("amll: corrupted JSON in search fails with Unreachable", waitFor([&] { return failed; }) && failReason == MissingReason::Unreachable);

        // Immediate retry: valid search with 0 results -> fails with NotFound AND sets negative cache
        server.responses.append({200, "{\"code\":200, \"data\": {\"items\": []}}", 0, {}});
        failed = false;
        failReason = MissingReason::Unreachable;
        amll.requestLyrics(metaMalformed, 13);
        check("amll: valid empty search results fail with NotFound", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);

        // Now subsequent request hits negative cache immediately without touching network
        failed = false;
        failReason = MissingReason::Unreachable;
        amll.requestLyrics(metaMalformed, 14);
        check("amll: negative cache hit after valid empty search", waitFor([&] { return failed; }) && failReason == MissingReason::NotFound);
        QObject::disconnect(connFail);
        amll.invalidateCache(metaMalformed);
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

        // 8. Multi-candidate queue: Candidate #1 static TTML, Candidate #2 synced TTML
        TrackMetadata metaMulti;
        metaMulti.title = QStringLiteral("Multi Candidate Track");
        metaMulti.artist = QStringLiteral("Queue Artist");
        metaMulti.durationMs = 210000;

        const QString searchJsonMulti = QStringLiteral(
            "{\"hits\": ["
            "  {\"title\":\"Multi Candidate Track\",\"artist\":\"Queue Artist\",\"duration\":210,"
            "   \"files\":{\"ttml\":\"/cand1.ttml\"}},"
            "  {\"title\":\"Multi Candidate Track\",\"artist\":\"Queue Artist\",\"duration\":210,"
            "   \"files\":{\"ttml\":\"/cand2.ttml\"}}"
            "]}");

        const QString unsyncedCand1 = QStringLiteral(
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:lrc=\"http://lrc.red/lyric-ttml-internal\" lrc:timing=\"None\">"
            "<body><div><p>Static lyric</p></div></body></tt>");

        server.responses.append({200, searchJsonMulti.toUtf8(), 0, {}}); // search response
        server.responses.append({200, unsyncedCand1.toUtf8(), 0, {}});   // cand 1 ttml -> static!
        server.responses.append({200, kValidTtml, 0, {}});               // cand 2 ttml -> synced!

        loaded = false;
        Lyrics multiLyrics;
        auto connMulti = QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &l, const QString &) {
            loaded = true;
            multiLyrics = l;
        });

        lrcred.requestLyrics(metaMulti, 31);
        check("lrcred: static cand1 falls back to synced cand2", waitFor([&] { return loaded; }));
        check("lrcred: cand2 loaded synced lyrics", multiLyrics.type == LyricsType::Line || multiLyrics.type == LyricsType::Syllable);
        QObject::disconnect(connMulti);
        lrcred.invalidateCache(metaMulti);

        // 9. Candidate with static TTML but working synchronized LRC
        TrackMetadata metaCandLrc;
        metaCandLrc.title = QStringLiteral("Static TTML Synced LRC Track");
        metaCandLrc.artist = QStringLiteral("Format Artist");
        metaCandLrc.durationMs = 200000;

        const QString searchJsonLrc = QStringLiteral(
            "{\"hits\": ["
            "  {\"title\":\"Static TTML Synced LRC Track\",\"artist\":\"Format Artist\",\"duration\":200,"
            "   \"files\":{\"ttml\":\"/candStatic.ttml\",\"lrc\":\"/candSynced.lrc\"}}"
            "]}");

        server.responses.append({200, searchJsonLrc.toUtf8(), 0, {}}); // search
        server.responses.append({200, unsyncedCand1.toUtf8(), 0, {}});   // ttml -> static!
        server.responses.append({200, kValidLrc, 0, {}});                // lrc -> synced!

        loaded = false;
        Lyrics formatLyrics;
        auto connFormat = QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &l, const QString &) {
            loaded = true;
            formatLyrics = l;
        });

        lrcred.requestLyrics(metaCandLrc, 32);
        check("lrcred: static TTML falls back to synchronized LRC of same candidate", waitFor([&] { return loaded; }));
        check("lrcred: loaded line-synced lyrics from LRC", formatLyrics.type == LyricsType::Line);
        QObject::disconnect(connFormat);
        lrcred.invalidateCache(metaCandLrc);

        // 4 candidates: queue truncated to 3, all 3 checked candidates fail, 4th left uninspected
        QJsonArray fourCandidates;
        for (int i = 1; i <= 4; ++i) {
            QJsonObject cand;
            cand[QStringLiteral("title")] = QStringLiteral("Truncate Song");
            cand[QStringLiteral("artist")] = QStringLiteral("Truncate Artist");
            cand[QStringLiteral("duration")] = 200;
            cand[QStringLiteral("isrc")] = QStringLiteral("TRUNC000000%1").arg(i);
            QJsonObject files;
            files[QStringLiteral("ttml")] = QStringLiteral("http://127.0.0.1:%1/cand%2.ttml").arg(server.serverPort()).arg(i);
            files[QStringLiteral("lrc")] = QStringLiteral("http://127.0.0.1:%1/cand%2.lrc").arg(server.serverPort()).arg(i);
            cand[QStringLiteral("files")] = files;
            fourCandidates.append(cand);
        }
        QJsonObject fourRoot;
        fourRoot[QStringLiteral("hits")] = fourCandidates;
        const QString fourSearchJson = QString::fromUtf8(QJsonDocument(fourRoot).toJson());

        server.responses.clear();
        server.responses.append({200, fourSearchJson.toUtf8(), 0, {}}); // search
        // Candidate 1: TTML 404, LRC 404
        server.responses.append({404, "Not Found", 0, {}});
        server.responses.append({404, "Not Found", 0, {}});
        // Candidate 2: TTML 404, LRC 404
        server.responses.append({404, "Not Found", 0, {}});
        server.responses.append({404, "Not Found", 0, {}});
        // Candidate 3: TTML 404, LRC 404
        server.responses.append({404, "Not Found", 0, {}});
        server.responses.append({404, "Not Found", 0, {}});

        TrackMetadata metaTrunc;
        metaTrunc.title = QStringLiteral("Truncate Song");
        metaTrunc.artist = QStringLiteral("Truncate Artist");
        metaTrunc.durationMs = 200000;

        bool truncFailed = false;
        auto connTrunc = QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason, const QString &) {
            truncFailed = true;
        });

        lrcred.requestLyrics(metaTrunc, 50);
        check("lrcred candidate truncation: fails after 3 candidates", waitFor([&] { return truncFailed; }));
        QObject::disconnect(connTrunc);

        // A second request for the same track must NOT hit negative cache!
        server.responses.append({200, fourSearchJson.toUtf8(), 0, {}});
        truncFailed = false;
        connTrunc = QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason, const QString &) {
            truncFailed = true;
        });

        lrcred.requestLyrics(metaTrunc, 51);
        check("lrcred candidate truncation: negative cache was NOT set (Miss on retry)", lrcred.cacheUsage(51) != QStringLiteral("Hit (negative cache)"));
        QObject::disconnect(connTrunc);
        lrcred.cancel();
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
        server.responses.append({404, "Not Found", 0, {}});

        TrackMetadata meta;
        meta.isrc = QStringLiteral("GBTEST123456");
        meta.title = QStringLiteral("Unsynced Test");
        meta.artist = QStringLiteral("Test Artist");

        bool lrcredFailed = false;
        MissingReason failReason = MissingReason::NotFound;
        bool lrcredLoaded = false;
        quint64 failedGeneration = 0;

        QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64 generation, MissingReason r, const QString &) {
            lrcredFailed = true;
            failReason = r;
            failedGeneration = generation;
        });
        QObject::connect(&lrcred, &LyricsProvider::lyricsLoaded, [&](quint64, const Lyrics &, const QString &) {
            lrcredLoaded = true;
        });

        lrcred.requestLyrics(meta, 1);
        check("lrcred rejects unsynced: failed emitted", waitFor([&] { return lrcredFailed; }));
        check("lrcred rejects unsynced: not loaded", !lrcredLoaded);
        check("lrcred rejects unsynced: reason is Unsynced", failReason == MissingReason::Unsynced);

        const qsizetype beforeCachedLookup = server.requests.size();
        lrcredFailed = false;
        lrcred.requestLyrics(meta, 2);
        check("lrcred: cached untimed lyrics keep Unsynced at the new generation", waitFor([&] { return lrcredFailed; })
              && failedGeneration == 2 && failReason == MissingReason::Unsynced && server.requests.size() == beforeCachedLookup);

        AmllProvider amllUnsynced(nullptr, server.apiBase().toString());
        ProviderResult amllResult;
        amllResult.observe(amllUnsynced);
        TrackMetadata amllMeta;
        amllMeta.spotifyTrackId = QStringLiteral("0123456789abcdefghijkl");
        server.responses.append({200, unsyncedTtml.toUtf8(), 0, {}});
        amllUnsynced.requestLyrics(amllMeta, 3);
        check("amll: untimed reply reports Unsynced", waitFor([&] { return amllResult.generations.size() == 1; })
              && amllResult.generations.back() == 3 && amllResult.reason == MissingReason::Unsynced);
        const qsizetype beforeAmllCache = server.requests.size();
        amllUnsynced.requestLyrics(amllMeta, 4);
        check("amll: cached untimed lyrics keep Unsynced at the new generation", waitFor([&] { return amllResult.generations.size() == 2; })
              && amllResult.generations.back() == 4 && amllResult.reason == MissingReason::Unsynced
              && server.requests.size() == beforeAmllCache);

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

    // =========================================================================
    // Suite 6: Identity Validation and Search Variant Invalidation
    // =========================================================================
    {
        HttpTestServer server;
        const QString amllBase = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        AmllProvider amll(nullptr, amllBase);
        LrcRedProvider lrcred(nullptr, amllBase);
        LrclibProvider lrclib(nullptr, server.apiBase());

        // 1. AMLL disk cache identity validation: reject foreign candidate
        {
            TrackMetadata meta;
            meta.title = QStringLiteral("Identity Song");
            meta.artist = QStringLiteral("Target Artist");
            meta.durationMs = 200000;

            const QString key = AmllProvider::makeCacheKey(meta);
            const QString ttmlPath = AmllProvider::cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".ttml");
            const QString metaPath = AmllProvider::cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".meta");

            QDir().mkpath(AmllProvider::cacheDirectory());
            QFile tf(ttmlPath);
            tf.open(QIODevice::WriteOnly);
            tf.write(kValidTtml);
            tf.close();

            QFile mf(metaPath);
            mf.open(QIODevice::WriteOnly);
            QJsonObject metaObj;
            metaObj[QStringLiteral("title")] = QStringLiteral("Identity Song");
            metaObj[QStringLiteral("artists")] = QJsonArray{QStringLiteral("Wrong Artist")};
            metaObj[QStringLiteral("durationMs")] = 200000;
            mf.write(QJsonDocument(metaObj).toJson());
            mf.close();

            check("suite 6: amll foreign cache files created", QFile::exists(ttmlPath) && QFile::exists(metaPath));

            server.responses.append({404, "Not Found", 0, {}});
            bool failed = false;
            QObject::connect(&amll, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason, const QString &) {
                failed = true;
            });
            amll.requestLyrics(meta, 1);
            waitFor([&] { return failed; });

            check("suite 6: amll rejected foreign disk cache and removed ttml", !QFile::exists(ttmlPath));
            check("suite 6: amll rejected foreign disk cache and removed meta", !QFile::exists(metaPath));
        }

        // 2. LrcRed disk cache identity validation: reject foreign candidate
        {
            TrackMetadata meta;
            meta.title = QStringLiteral("Identity Song 2");
            meta.artist = QStringLiteral("Target Artist 2");
            meta.durationMs = 200000;

            const QString key = LrcRedProvider::makeCacheKey(meta);
            const QString ttmlPath = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".ttml");
            const QString metaPath = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".meta");

            QDir().mkpath(LrcRedProvider::cacheDirectory());
            QFile tf(ttmlPath);
            tf.open(QIODevice::WriteOnly);
            tf.write(kValidTtml);
            tf.close();

            QFile mf(metaPath);
            mf.open(QIODevice::WriteOnly);
            QJsonObject metaObj;
            metaObj[QStringLiteral("title")] = QStringLiteral("Identity Song 2");
            metaObj[QStringLiteral("artist")] = QStringLiteral("Wrong Artist 2");
            metaObj[QStringLiteral("durationMs")] = 200000;
            mf.write(QJsonDocument(metaObj).toJson());
            mf.close();

            check("suite 6: lrcred foreign cache files created", QFile::exists(ttmlPath) && QFile::exists(metaPath));

            server.responses.append({404, "Not Found", 0, {}});
            bool failed = false;
            QObject::connect(&lrcred, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason, const QString &) {
                failed = true;
            });
            lrcred.requestLyrics(meta, 2);
            waitFor([&] { return failed; });

            check("suite 6: lrcred rejected foreign disk cache and removed ttml", !QFile::exists(ttmlPath));
            check("suite 6: lrcred rejected foreign disk cache and removed meta", !QFile::exists(metaPath));
        }

        // 3. LRCLIB disk cache identity validation: reject foreign candidate
        {
            TrackMetadata meta;
            meta.title = QStringLiteral("Identity Song 3");
            meta.artist = QStringLiteral("Target Artist 3");
            meta.durationMs = 200000;

            const QString key = LrclibClient::makeCacheKey(meta);
            const QString jsonPath = LrclibClient::cacheDirectory() + QStringLiteral("/") + key + QStringLiteral(".json");

            QDir().mkpath(LrclibClient::cacheDirectory());
            QFile jf(jsonPath);
            jf.open(QIODevice::WriteOnly);
            QJsonObject obj;
            obj[QStringLiteral("trackName")] = QStringLiteral("Identity Song 3");
            obj[QStringLiteral("artistName")] = QStringLiteral("Wrong Artist 3");
            obj[QStringLiteral("duration")] = 200.0;
            obj[QStringLiteral("plainLyrics")] = QStringLiteral("plain");
            obj[QStringLiteral("syncedLyrics")] = QStringLiteral("[00:01.00]Line");
            jf.write(QJsonDocument(obj).toJson());
            jf.close();

            check("suite 6: lrclib foreign cache file created", QFile::exists(jsonPath));

            server.responses.append({404, "Not Found", 0, {}});
            bool failed = false;
            QObject::connect(&lrclib, &LyricsProvider::lyricsFailed, [&](quint64, MissingReason, const QString &) {
                failed = true;
            });
            lrclib.requestLyrics(meta, 3);
            check("suite 6: lrclib synchronously rejected foreign disk cache and removed json", !QFile::exists(jsonPath));
            waitFor([&] { return failed; });

            if (QFile::exists(jsonPath)) {
                QFile rf(jsonPath);
                if (rf.open(QIODevice::ReadOnly)) {
                    const QJsonObject resObj = QJsonDocument::fromJson(rf.readAll()).object();
                    check("suite 6: recreated cache is negative cache, not foreign lyrics", resObj.value(QStringLiteral("missing")).toBool());
                }
            }
        }

        // 4. Invalidation clears all search variants
        {
            TrackMetadata meta;
            meta.isWebSession = true;
            meta.title = QStringLiteral("WebArtist - WebTitle (Official Music Video)");
            meta.artist = QStringLiteral("WebArtist");
            meta.durationMs = 180000;

            const QVector<TrackMetadata> variants = generateSearchVariants(meta);
            check("suite 6: search variants generated", variants.size() >= 2);

            QList<QString> amllFiles;
            QList<QString> lrcredFiles;
            QList<QString> lrclibFiles;

            for (const auto &v : variants) {
                const QString amllKey = AmllProvider::makeCacheKey(v);
                const QString amllFile = AmllProvider::cacheDirectory() + QStringLiteral("/") + amllKey + QStringLiteral(".ttml");
                QFile af(amllFile);
                if (af.open(QIODevice::WriteOnly)) { af.write("dummy"); af.close(); amllFiles.append(amllFile); }

                const QString lrcredKey = LrcRedProvider::makeCacheKey(v);
                const QString lrcredFile = LrcRedProvider::cacheDirectory() + QStringLiteral("/") + lrcredKey + QStringLiteral(".ttml");
                QFile rf(lrcredFile);
                if (rf.open(QIODevice::WriteOnly)) { rf.write("dummy"); rf.close(); lrcredFiles.append(lrcredFile); }

                const QString lrclibKey = LrclibClient::makeCacheKey(v);
                const QString lrclibFile = LrclibClient::cacheDirectory() + QStringLiteral("/") + lrclibKey + QStringLiteral(".json");
                QFile lf(lrclibFile);
                if (lf.open(QIODevice::WriteOnly)) { lf.write("dummy"); lf.close(); lrclibFiles.append(lrclibFile); }
            }

            check("suite 6: all variant files written", !amllFiles.isEmpty() && !lrcredFiles.isEmpty() && !lrclibFiles.isEmpty());

            amll.invalidateCache(meta);
            lrcred.invalidateCache(meta);
            lrclib.invalidateCache(meta);

            bool allAmllPurged = true;
            for (const QString &f : amllFiles) {
                if (QFile::exists(f)) allAmllPurged = false;
            }
            bool allLrcredPurged = true;
            for (const QString &f : lrcredFiles) {
                if (QFile::exists(f)) allLrcredPurged = false;
            }
            bool allLrclibPurged = true;
            for (const QString &f : lrclibFiles) {
                if (QFile::exists(f)) allLrclibPurged = false;
            }

            check("suite 6: amll variant files purged", allAmllPurged);
            check("suite 6: lrcred variant files purged", allLrcredPurged);
            check("suite 6: lrclib variant files purged", allLrclibPurged);
        }
    }

    {
        HttpTestServer server;
        CustomProvider provider;
        ProviderResult result;
        result.observe(provider);
        provider.setUrlTemplate(server.apiBase().toString() + QStringLiteral("lyrics?title={title}&duration={durationMs}"));
        TrackMetadata meta;
        meta.title = QStringLiteral("One & two");
        meta.durationMs = 200000;
        server.responses.append({200, "[00:02.00]old line", 120, {}});
        server.responses.append({200, R"({"lyrics":"Untimed text","syncedLyrics":"[00:01.00]Current timed line"})", 0, {}});
        provider.requestLyrics(meta, 70);
        check("custom: the superseded request reached the endpoint", waitFor([&] { return server.requests.size() == 1; }));
        provider.requestLyrics(meta, 71);
        check("custom: JSON timed lyrics can follow an untimed field", waitFor([&] { return result.generations.size() == 1; })
              && result.lyrics.lines.size() == 1 && result.lyrics.lines.front().text == QStringLiteral("Current timed line"));
        advanceEvents(140);
        check("custom: replacement is the only terminal result", result.generations == QList<quint64>{71}
              && result.providerId == QStringLiteral("custom"));
        check("custom: URL substitutions encode title and duration", server.requests.size() == 2
              && server.requests.back() == QStringLiteral("/api/lyrics?title=One%20%26%20two&duration=200000"));

        server.responses.append({200, kValidLrc, 0, {}});
        provider.requestLyrics(meta, 72);
        check("custom: a repeat reaches the endpoint and raw LRC loads", waitFor([&] { return result.generations.size() == 2; })
              && result.generations.back() == 72 && server.requests.size() == 3 && result.lyrics.lines.size() == 2
              && result.lyrics.attributionData.provider == QStringLiteral("Custom URL") && result.lyrics.attributionUrl.isEmpty());

        server.responses.append({200, R"({"message":"Sample timestamp [00:01.00] in API documentation"})", 0, {}});
        provider.requestLyrics(meta, 85);
        check("custom: JSON metadata is not interpreted as lyrics", waitFor([&] { return result.generations.size() == 3; })
              && result.generations.back() == 85 && result.reason == MissingReason::NotFound
              && result.lyrics.lines.isEmpty());
    }

    {
        HttpTestServer origin;
        HttpTestServer redirected;
        CustomProvider provider;
        ProviderResult result;
        result.observe(provider);
        provider.setUrlTemplate(origin.apiBase().toString() + QStringLiteral("redirect"));
        TrackMetadata meta;
        meta.title = QStringLiteral("Redirect fixture");

        provider.setHeaders(QStringLiteral("X-Fixture: enabled"));
        origin.responses.append({302, {}, 0, "Location: /same-origin\r\n"});
        origin.responses.append({200, kValidLrc, 0, {}});
        provider.requestLyrics(meta, 87);
        check("custom: redirects on the configured origin still load lyrics", waitFor([&] { return result.generations.size() == 1; })
              && result.generations.back() == 87 && result.lyrics.lines.size() == 2
              && origin.requests.size() == 2 && origin.requests.back() == QStringLiteral("/same-origin"));

        const QByteArray differentOrigin = "Location: " + redirected.apiBase().toEncoded() + "lyrics\r\n";
        origin.responses.append({302, {}, 0, differentOrigin});
        provider.requestLyrics(meta, 88);
        check("custom: configured headers keep redirects on the original origin", waitFor([&] { return result.generations.size() == 2; })
              && result.generations.back() == 88 && result.reason == MissingReason::Unreachable && redirected.requests.isEmpty());

        provider.setHeaders({});
        origin.responses.append({302, {}, 0, differentOrigin});
        redirected.responses.append({200, kValidLrc, 0, {}});
        provider.requestLyrics(meta, 89);
        check("custom: header-free redirect can load from another origin", waitFor([&] { return result.generations.size() == 3; })
              && result.generations.back() == 89 && result.lyrics.lines.size() == 2 && redirected.requests.size() == 1);
    }

    {
        QTemporaryDir directory;
        LocalProvider provider;
        ProviderResult result;
        result.observe(provider);
        provider.setCustomDirectory(directory.path());
        TrackMetadata meta;
        meta.title = QStringLiteral("Local integration fixture");
        meta.url = QUrl::fromLocalFile(directory.filePath(QStringLiteral("audio.flac"))).toString();
        QFile sidecar(directory.filePath(QStringLiteral("audio.ttml")));
        check("local: oversized sidecar fixture created", sidecar.open(QIODevice::WriteOnly)
              && sidecar.resize(LyricsLimits::kMaxInputSizeBytes + 1));
        sidecar.close();
        provider.requestLyrics(meta, 73);
        check("local: oversized sidecar remains the terminal reason", result.generations == QList<quint64>{73}
              && result.reason == MissingReason::SizeLimitExceeded && result.lyrics.lines.isEmpty());

        QFile customFile(directory.filePath(meta.title + QStringLiteral(".lrc")));
        check("local: writable LRC fixture created", customFile.open(QIODevice::WriteOnly)
              && customFile.write(kValidLrc) == qint64(qstrlen(kValidLrc)));
        customFile.close();
        provider.requestLyrics(meta, 74);
        check("local: custom directory supplies lyrics after invalid sidecar", result.generations == QList<quint64>{73, 74}
              && result.providerId == QStringLiteral("local") && result.lyrics.lines.size() == 2
              && result.lyrics.lines.front().text == QStringLiteral("Hello world"));
        check("local: provider does not modify either source file", sidecar.size() == LyricsLimits::kMaxInputSizeBytes + 1
              && customFile.open(QIODevice::ReadOnly) && customFile.readAll() == kValidLrc);
    }

    {
        HttpTestServer server;
        LrcRedProvider provider(nullptr, server.apiBase().toString());
        ProviderResult result;
        result.observe(provider);
        TrackMetadata meta;
        meta.title = QStringLiteral("Bad search shape fixture");
        const QString cachePath = LrcRedProvider::cacheDirectory() + QLatin1Char('/') + LrcRedProvider::makeCacheKey(meta);
        server.responses.append({200, R"({"hits":null})", 0, {}});
        provider.requestLyrics(meta, 75);
        check("lrcred: malformed search shape is unreachable", waitFor([&] { return result.generations.size() == 1; })
              && result.generations.back() == 75 && result.reason == MissingReason::Unreachable);

        server.responses.append({200, R"({"hits":[{"title":"Bad search shape fixture","files":{"lrc":"good.lrc"}}]})", 0, {}});
        server.responses.append({200, kValidLrc, 0, {}});
        provider.requestLyrics(meta, 76);
        check("lrcred: a malformed search never blocks immediate recovery", waitFor([&] { return result.generations.size() == 2; })
              && result.generations.back() == 76 && result.lyrics.lines.size() == 2 && server.requests.size() == 3
              && QFile::exists(cachePath + QStringLiteral(".lrc")));
    }

    {
        HttpTestServer server;
        LrcRedProvider provider(nullptr, server.apiBase().toString());
        ProviderResult result;
        result.observe(provider);
        TrackMetadata meta;
        meta.title = QStringLiteral("Long line fixture");
        meta.isrc = QStringLiteral("USTEST123456");
        const QString cachePath = LrcRedProvider::cacheDirectory() + QLatin1Char('/') + LrcRedProvider::makeCacheKey(meta);
        server.responses.append({404, {}, 0, {}});
        server.responses.append({200, "[00:01.00]" + QByteArray(LyricsLimits::kMaxLineLengthChars, 'x'), 0, {}});
        provider.requestLyrics(meta, 77);
        check("lrcred: parser limit is reported without a positive cache entry", waitFor([&] { return result.generations.size() == 1; })
              && result.generations.back() == 77 && result.reason == MissingReason::SizeLimitExceeded
              && !QFile::exists(cachePath + QStringLiteral(".lrc")));

        server.responses.append({200, kValidTtml, 0, {}});
        provider.requestLyrics(meta, 78);
        check("lrcred: parser limit does not create a negative cache entry", waitFor([&] { return result.generations.size() == 2; })
              && result.generations.back() == 78 && result.lyrics.lines.size() == 2 && server.requests.size() == 3
              && QFile::exists(cachePath + QStringLiteral(".ttml")));
    }

    {
        HttpTestServer server;
        LrcRedProvider lrcred(nullptr, server.apiBase().toString());
        AmllProvider amll(nullptr, server.apiBase().toString());
        TrackMetadata meta;
        meta.title = QStringLiteral("Candidate fixture");
        const QByteArray lrcRedResults = R"({"results":[{"title":"Candidate fixture","files":{"lrc":"track.lrc"}}]})";
        const QByteArray amllResults = R"({"data":{"items":[{"title":"Candidate fixture","id":12}]}})";
        const std::pair<LyricsProvider *, QByteArray> cases[]{{&lrcred, lrcRedResults}, {&amll, amllResults}};
        for (const auto &[provider, results] : cases) {
            QObject observations;
            QList<quint64> completed;
            int candidatesFound = 0;
            QList<LyricCandidate> candidates;
            MissingReason reason = MissingReason::NotFound;
            QObject::connect(provider, &LyricsProvider::candidateSearchFailed, &observations,
                             [&](quint64 searchId, MissingReason value, const QString &, int) {
                                 completed.append(searchId);
                                 reason = value;
                             });
            QObject::connect(provider, &LyricsProvider::candidatesFound, &observations,
                             [&](quint64 searchId, const QList<LyricCandidate> &value, const QString &) {
                                 completed.append(searchId);
                                 candidates = value;
                                 ++candidatesFound;
                             });

            server.responses.append({200, R"({"message":"Temporarily unavailable","data":null})", 0, {}});
            provider->searchCandidates(meta, 92);
            check(qPrintable(provider->id() + QStringLiteral(": malformed manual search stays transient")),
                  waitFor([&] { return completed.size() == 1; }) && completed.back() == 92
                      && reason == MissingReason::Unreachable && candidatesFound == 0);

            server.responses.append({200, results, 0, {}});
            provider->searchCandidates(meta, 93);
            check(qPrintable(provider->id() + QStringLiteral(": manual retry loads an existing response shape")),
                  waitFor([&] { return completed.size() == 2; }) && completed.back() == 93
                      && candidates.size() == 1 && candidates.front().title == meta.title);

            const int countBeforeEmpty = candidatesFound;
            server.responses.append({200, "[]", 0, {}});
            provider->searchCandidates(meta, 94);
            check(qPrintable(provider->id() + QStringLiteral(": valid empty manual search remains successful")),
                  waitFor([&] { return completed.size() == 3; }) && completed.back() == 94
                      && candidatesFound == countBeforeEmpty + 1 && candidates.isEmpty());
        }
    }

    {
        HttpTestServer server;
        KugouProvider provider(nullptr, server.apiBase().toString());
        QList<quint64> failed;
        QList<quint64> loaded;
        MissingReason reason = MissingReason::NotFound;
        Lyrics lyrics;
        QObject::connect(&provider, &LyricsProvider::candidateLyricsLoaded, [&](quint64 fetchId, const Lyrics &value, const QString &) {
            loaded.append(fetchId);
            lyrics = value;
        });
        QObject::connect(&provider, &LyricsProvider::candidateLyricsFailed, [&](quint64 fetchId, MissingReason value, const QString &) {
            failed.append(fetchId);
            reason = value;
        });

        server.responses.append({200, QByteArray(LyricsLimits::kMaxInputSizeBytes + 1, ' '), 0, {}});
        provider.fetchCandidateLyrics(QStringLiteral("fixtureHash:15"), 90);
        check("kugou: first candidate fetch step reports the transport size limit", waitFor([&] { return failed.size() == 1; })
              && failed.back() == 90 && reason == MissingReason::SizeLimitExceeded && loaded.isEmpty() && server.requests.size() == 1);

        server.responses.append({200, R"({"candidates":[{"id":"1","accesskey":"fixture"}]})", 0, {}});
        const QJsonObject payload{{"content", QString::fromLatin1(QByteArray(kValidLrc).toBase64())}};
        server.responses.append({200, QJsonDocument(payload).toJson(), 0, {}});
        provider.fetchCandidateLyrics(QStringLiteral("fixtureHash:15"), 91);
        check("kugou: both candidate fetch steps run and the bounded LRC fallback loads", waitFor([&] { return loaded.size() == 1; })
              && loaded.back() == 91 && failed.size() == 1 && lyrics.lines.size() == 2 && server.requests.size() == 3);
    }

    {
        HttpTestServer server;
        UnisonProvider provider(nullptr, server.apiBase().toString());
        ProviderResult result;
        result.observe(provider);
        TrackMetadata meta;
        meta.title = QStringLiteral("Automatic track");
        meta.durationMs = 2500;
        server.responses.append({200, R"({"data":{"lyrics":"[00:01.00]Manual track","format":"lrc"}})", 60, {}});
        server.responses.append({200, R"({"data":{"song":"Automatic track","lyrics":"[00:01.00]Automatic track","format":"lrc"}})", 0, {}});
        QList<quint64> manualFetches;
        Lyrics manualLyrics;
        QObject::connect(&provider, &LyricsProvider::candidateLyricsLoaded, [&](quint64 fetchId, const Lyrics &value, const QString &) {
            manualFetches.append(fetchId);
            manualLyrics = value;
        });
        provider.fetchCandidateLyrics(QStringLiteral("manual"), 79);
        check("unison: manual lookup reaches the endpoint", waitFor([&] { return server.requests.size() == 1; }));
        provider.requestLyrics(meta, 80);
        check("unison: direct LRC uses automatic track duration", waitFor([&] { return result.generations.size() == 1; })
              && result.generations.back() == 80 && result.lyrics.lines.size() == 1 && result.lyrics.lines.front().endMs == 2500);
        check("unison: manual LRC timing is independent of simultaneous automatic search", waitFor([&] { return manualFetches.size() == 1; })
              && manualFetches.back() == 79 && manualLyrics.lines.size() == 1 && manualLyrics.lines.front().endMs == 7000);

        QJsonObject payload{{"song", "Automatic track"}, {"format", "lrc"}, {"lyrics", QStringLiteral("[00:01.00]")
            + QString(LyricsLimits::kMaxLineLengthChars, QLatin1Char('x'))}};
        server.responses.append({200, QJsonDocument(QJsonObject{{"data", payload}}).toJson(), 0, {}});
        provider.requestLyrics(meta, 86);
        check("unison: direct parser limit stays a size failure", waitFor([&] { return result.generations.size() == 2; })
              && result.generations.back() == 86 && result.reason == MissingReason::SizeLimitExceeded && server.requests.size() == 3);
    }

    if (s_failures == 0) {
        std::printf("\nAll provider cache tests passed successfully.\n");
        return 0;
    }
    std::printf("\n%d provider cache test(s) failed.\n", s_failures);
    return 1;
}
