/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "trackmatch.h"
#include "trackname.h"

#include <iostream>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

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
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::cout << "Running provider-match-test...\n";

    // 1. Title Matching Tests
    {
        // Exact and case
        check("title: exact match", TrackMatch::matchTitles(QStringLiteral("Blinding Lights"), QStringLiteral("Blinding Lights")));
        check("title: case insensitive", TrackMatch::matchTitles(QStringLiteral("blinding lights"), QStringLiteral("BLINDING LIGHTS")));
        check("title: whitespace differences", TrackMatch::matchTitles(QStringLiteral("  Blinding   Lights "), QStringLiteral("Blinding Lights")));

        // Punctuation and quotes
        check("title: apostrophe normalization", TrackMatch::matchTitles(QStringLiteral("Don't Stop Believin'"), QStringLiteral("Dont Stop Believin")));
        check("title: unicode curly apostrophe", TrackMatch::matchTitles(QStringLiteral("Don’t Stop"), QStringLiteral("Don't Stop")));
        check("title: quotes removed", TrackMatch::matchTitles(QStringLiteral("\"Bohemian Rhapsody\""), QStringLiteral("Bohemian Rhapsody")));
        check("title: hyphens to space", TrackMatch::matchTitles(QStringLiteral("Can't-Stop-Now"), QStringLiteral("Can't Stop Now")));

        // Audio extensions
        check("title: audio extension .mp3", TrackMatch::matchTitles(QStringLiteral("Song.mp3"), QStringLiteral("Song")));
        check("title: audio extension .flac with track number", TrackMatch::matchTitles(QStringLiteral("01 - Song.flac"), QStringLiteral("Song")));

        // Numeric titles and track numbers: formatting separator + whitespace required
        check("title: numeric title 21 Guns preserved", cleanTrackTitle(QStringLiteral("21 Guns")) == QStringLiteral("21 Guns"));
        check("title: numeric title 99-Problems preserved", cleanTrackTitle(QStringLiteral("99-Problems")) == QStringLiteral("99-Problems"));
        check("title: numeric title 100 Degrees preserved", cleanTrackTitle(QStringLiteral("100 Degrees")) == QStringLiteral("100 Degrees"));
        check("title: track number with dot and space stripped", cleanTrackTitle(QStringLiteral("12. Song")) == QStringLiteral("Song"));
        check("title: unambiguous 01. prefix stripped", cleanTrackTitle(QStringLiteral("01. Song")) == QStringLiteral("Song"));
        check("title: unambiguous 01.- prefix stripped", cleanTrackTitle(QStringLiteral("01.- Song")) == QStringLiteral("Song"));
        check("title: unambiguous 01 - prefix stripped", cleanTrackTitle(QStringLiteral("01 - Song")) == QStringLiteral("Song"));

        // Ordinary official/video noise
        check("title: official video bracket", TrackMatch::matchTitles(QStringLiteral("Song (Official Video)"), QStringLiteral("Song")));
        check("title: official music video [4K]", TrackMatch::matchTitles(QStringLiteral("Song [Official Music Video] [4K]"), QStringLiteral("Song")));
        check("title: lyrics video tail", TrackMatch::matchTitles(QStringLiteral("Song | Lyrics video"), QStringLiteral("Song")));
        check("title: clip officiel", TrackMatch::matchTitles(QStringLiteral("Song (Clip Officiel)"), QStringLiteral("Song")));
        check("title: visualizer", TrackMatch::matchTitles(QStringLiteral("Song (Visualizer)"), QStringLiteral("Song")));
        check("title: Chinese MV bracket", TrackMatch::matchTitles(QStringLiteral("晴天 (官方MV)"), QStringLiteral("晴天")));

        // Remaster / edition noise
        check("title: dash remaster", TrackMatch::matchTitles(QStringLiteral("Bohemian Rhapsody - 2011 Remaster"), QStringLiteral("Bohemian Rhapsody")));
        check("title: deluxe edition bracket", TrackMatch::matchTitles(QStringLiteral("Song (Deluxe Edition)"), QStringLiteral("Song")));
        check("title: original mix", TrackMatch::matchTitles(QStringLiteral("Song (Original Mix)"), QStringLiteral("Song")));

        // Feature decoration
        check("title: candidate has feat bracket", TrackMatch::matchTitles(QStringLiteral("Levitating (feat. DaBaby)"), QStringLiteral("Levitating")));
        check("title: target has feat bracket", TrackMatch::matchTitles(QStringLiteral("Levitating"), QStringLiteral("Levitating (feat. DaBaby)")));
        check("title: feat tail without bracket", TrackMatch::matchTitles(QStringLiteral("Levitating feat. DaBaby"), QStringLiteral("Levitating")));
        check("title: with someone", TrackMatch::matchTitles(QStringLiteral("Song (with Someone)"), QStringLiteral("Song")));

        // Substring rejection (must NOT match substring songs)
        check("title: reject substring Hello World vs Hello", !TrackMatch::matchTitles(QStringLiteral("Hello World"), QStringLiteral("Hello")));
        check("title: reject substring Love Story vs Love", !TrackMatch::matchTitles(QStringLiteral("Love Story"), QStringLiteral("Love")));
        check("title: reject Song 2 vs Song", !TrackMatch::matchTitles(QStringLiteral("Song 2"), QStringLiteral("Song")));

        // Distinguishing versions: Live
        check("version: reject Live candidate for studio target", !TrackMatch::matchTitles(QStringLiteral("Hotel California (Live)"), QStringLiteral("Hotel California")));
        check("version: reject studio candidate for Live target", !TrackMatch::matchTitles(QStringLiteral("Hotel California"), QStringLiteral("Hotel California (Live)")));
        check("version: match Live with Live bracket", TrackMatch::matchTitles(QStringLiteral("Hotel California (Live at Wembley)"), QStringLiteral("Hotel California (Live)")));
        check("version: match Live bracket with Live dash", TrackMatch::matchTitles(QStringLiteral("Hotel California (Live)"), QStringLiteral("Hotel California - Live")));
        check("version: match Chinese Live tag", TrackMatch::matchTitles(QStringLiteral("晴天 (现场版)"), QStringLiteral("晴天 (Live)")));
        check("version: reject Chinese Live for studio target", !TrackMatch::matchTitles(QStringLiteral("晴天 (现场版)"), QStringLiteral("晴天")));

        // Distinguishing versions: Acoustic
        check("version: reject Acoustic candidate for studio target", !TrackMatch::matchTitles(QStringLiteral("Shallow (Acoustic)"), QStringLiteral("Shallow")));
        check("version: reject studio candidate for Acoustic target", !TrackMatch::matchTitles(QStringLiteral("Shallow"), QStringLiteral("Shallow (Acoustic)")));
        check("version: match Acoustic versions", TrackMatch::matchTitles(QStringLiteral("Shallow (Acoustic)"), QStringLiteral("Shallow - Acoustic Version")));

        // Distinguishing versions: Remix
        check("version: reject Remix candidate for studio target", !TrackMatch::matchTitles(QStringLiteral("Radioactive (Remix)"), QStringLiteral("Radioactive")));
        check("version: reject studio candidate for Remix target", !TrackMatch::matchTitles(QStringLiteral("Radioactive"), QStringLiteral("Radioactive (Remix)")));
        check("version: match generic Remixes", TrackMatch::matchTitles(QStringLiteral("Radioactive (Remix)"), QStringLiteral("Radioactive (Remix)")));
        check("version: match same specific Remix", TrackMatch::matchTitles(QStringLiteral("Radioactive (Steve Aoki Remix)"), QStringLiteral("Radioactive (Steve Aoki Remix)")));
        check("version: reject different specific Remixes", !TrackMatch::matchTitles(QStringLiteral("Radioactive (Steve Aoki Remix)"), QStringLiteral("Radioactive (Don Diablo Remix)")));

        // Distinguishing versions: Instrumental
        check("version: reject Instrumental for studio target", !TrackMatch::matchTitles(QStringLiteral("Song (Instrumental)"), QStringLiteral("Song")));
        check("version: reject Chinese karaoke/instrumental for studio target", !TrackMatch::matchTitles(QStringLiteral("晴天 (伴奏)"), QStringLiteral("晴天")));

        // Distinguishing versions: Slowed & Sped Up
        check("version: reject Slowed candidate for studio target", !TrackMatch::matchTitles(QStringLiteral("Song (Slowed + Reverb)"), QStringLiteral("Song")));
        check("version: reject studio candidate for Slowed target", !TrackMatch::matchTitles(QStringLiteral("Song"), QStringLiteral("Song (Slowed)")));
        check("version: match Slowed versions", TrackMatch::matchTitles(QStringLiteral("Song (Slowed + Reverb)"), QStringLiteral("Song - Slowed")));
        check("version: reject Sped Up candidate for Slowed target", !TrackMatch::matchTitles(QStringLiteral("Song (Sped Up)"), QStringLiteral("Song (Slowed)")));
        check("version: reject Sped Up candidate for studio target", !TrackMatch::matchTitles(QStringLiteral("Song (Sped Up)"), QStringLiteral("Song")));
    }

    // 2. Artist Matching Tests (Generic delimiter fix: slash requires whitespace)
    {
        // Exact and case
        check("artist: exact match", TrackMatch::matchArtists(QStringLiteral("Taylor Swift"), QStringLiteral("Taylor Swift")));
        check("artist: case insensitive", TrackMatch::matchArtists(QStringLiteral("taylor swift"), QStringLiteral("TAYLOR SWIFT")));
        check("artist: punctuation AC/DC", TrackMatch::matchArtists(QStringLiteral("AC/DC"), QStringLiteral("AC/DC")));
        check("artist: punctuation Guns N' Roses", TrackMatch::matchArtists(QStringLiteral("Guns N' Roses"), QStringLiteral("Guns N Roses")));
        check("artist: punctuation A$AP Rocky", TrackMatch::matchArtists(QStringLiteral("A$AP Rocky"), QStringLiteral("ASAP Rocky")));

        // Generic embedded-slash preservation: AC/DC and other slash stage names
        check("artist: AC/DC extractPrimaryArtist preserved as one", extractPrimaryArtist(QStringLiteral("AC/DC")) == QStringLiteral("AC/DC"));
        check("artist: AC/DC extractPrimaryArtist with feature", extractPrimaryArtist(QStringLiteral("AC/DC feat. Axl Rose")) == QStringLiteral("AC/DC"));
        check("artist: AC/DC extractArtistList has 1 artist", TrackMatch::extractArtistList(QStringLiteral("AC/DC")).size() == 1);
        check("artist: K/DA embedded slash preserved as one", extractPrimaryArtist(QStringLiteral("K/DA")) == QStringLiteral("K/DA"));
        check("artist: K/DA extractArtistList has 1 artist", TrackMatch::extractArtistList(QStringLiteral("K/DA")).size() == 1);

        // Slash surrounded by whitespace splits artists
        check("artist: slash surrounded by whitespace splits", TrackMatch::extractArtistList(QStringLiteral("Taylor Swift / Bon Iver")).size() == 2);
        check("artist: slash surrounded by whitespace primary extract", extractPrimaryArtist(QStringLiteral("Taylor Swift / Bon Iver")) == QStringLiteral("Taylor Swift"));

        // Primary artist list tolerance
        check("artist: candidate list matches target primary", TrackMatch::matchArtists(QStringLiteral("Taylor Swift, Ed Sheeran"), QStringLiteral("Taylor Swift")));
        check("artist: candidate slash matches target primary", TrackMatch::matchArtists(QStringLiteral("Taylor Swift / Bon Iver"), QStringLiteral("Taylor Swift")));
        check("artist: target list matches candidate primary", TrackMatch::matchArtists(QStringLiteral("Taylor Swift"), QStringLiteral("Taylor Swift & Ed Sheeran")));
        check("artist: target feat matches candidate primary", TrackMatch::matchArtists(QStringLiteral("Taylor Swift"), QStringLiteral("Taylor Swift feat. DaBaby")));
        check("artist: Chinese comma ideographic delimiter", TrackMatch::matchArtists(QStringLiteral("周杰伦、阿信"), QStringLiteral("周杰伦")));
        check("artist: QStringList candidate matches target primary", TrackMatch::matchArtists(QStringList{QStringLiteral("Taylor Swift"), QStringLiteral("Zayn")}, QStringLiteral("Taylor Swift")));

        // Rejections when target artist known
        check("artist: reject empty candidate when target known", !TrackMatch::matchArtists(QStringLiteral(""), QStringLiteral("Adele")));
        check("artist: reject completely different artist", !TrackMatch::matchArtists(QStringLiteral("Lionel Richie"), QStringLiteral("Adele")));
        check("artist: reject wrong band", !TrackMatch::matchArtists(QStringLiteral("Coldplay"), QStringLiteral("Radiohead")));

        // Unknown target artist passes
        check("artist: empty target artist passes", TrackMatch::matchArtists(QStringLiteral("Adele"), QStringLiteral("")));
    }

    // 3. Shared Metadata Normalization Tests (Idempotency and Edge Cases)
    {
        // (a) YouTube raw title + uploader channel
        TrackMetadata ytMeta;
        ytMeta.title = QStringLiteral("Coldplay - Yellow (Official Music Video) [4K]");
        ytMeta.artist = QStringLiteral("ColdplayVEVO");
        ytMeta.isWebSession = true;
        normalizeTrackMetadata(&ytMeta);
        check("normalize: YouTube raw title split and cleaned", ytMeta.title == QStringLiteral("Yellow") && ytMeta.artist == QStringLiteral("Coldplay"));

        // Idempotency: second call does NOT re-split or alter normalized fields
        normalizeTrackMetadata(&ytMeta);
        check("normalize: idempotent on YouTube metadata after 2 calls", ytMeta.title == QStringLiteral("Yellow") && ytMeta.artist == QStringLiteral("Coldplay"));

        // (b) Native playback: Drake Love - Hate is NOT split
        TrackMetadata nativeMeta;
        nativeMeta.title = QStringLiteral("Love - Hate");
        nativeMeta.artist = QStringLiteral("Drake");
        nativeMeta.isWebSession = false;
        normalizeTrackMetadata(&nativeMeta);
        check("normalize: native Love - Hate not split", nativeMeta.title == QStringLiteral("Love - Hate") && nativeMeta.artist == QStringLiteral("Drake"));

        normalizeTrackMetadata(&nativeMeta);
        check("normalize: native Love - Hate idempotent after 2 calls", nativeMeta.title == QStringLiteral("Love - Hate") && nativeMeta.artist == QStringLiteral("Drake"));

        // (d) Artist - Song - Live with uploader channel: idempotency after 2 calls
        TrackMetadata liveMeta;
        liveMeta.title = QStringLiteral("Ken Carson - deaf note - Live [Official Video]");
        liveMeta.artist = QStringLiteral("TopDawgRecords"); // uploader channel
        liveMeta.isWebSession = true;
        normalizeTrackMetadata(&liveMeta);
        check("normalize: Artist - Song - Live parsed correctly",
              liveMeta.artist == QStringLiteral("Ken Carson") && liveMeta.title == QStringLiteral("deaf note - Live") && liveMeta.searchNormalized);

        // Crucial test: second call must NOT parse "deaf note" into artist!
        normalizeTrackMetadata(&liveMeta);
        check("normalize: Artist - Song - Live idempotent after 2 calls (not split into artistSong)",
              liveMeta.artist == QStringLiteral("Ken Carson") && liveMeta.title == QStringLiteral("deaf note - Live"));

        // (e) Numeric titles preserved in metadata
        TrackMetadata numMeta1;
        numMeta1.title = QStringLiteral("21 Guns");
        numMeta1.artist = QStringLiteral("Green Day");
        normalizeTrackMetadata(&numMeta1);
        check("normalize: 21 Guns preserved", numMeta1.title == QStringLiteral("21 Guns") && numMeta1.artist == QStringLiteral("Green Day"));

        TrackMetadata numMeta2;
        numMeta2.title = QStringLiteral("99-Problems");
        numMeta2.artist = QStringLiteral("Jay-Z");
        normalizeTrackMetadata(&numMeta2);
        check("normalize: 99-Problems preserved", numMeta2.title == QStringLiteral("99-Problems") && numMeta2.artist == QStringLiteral("Jay-Z"));

        TrackMetadata numMeta3;
        numMeta3.title = QStringLiteral("100 Degrees");
        numMeta3.artist = QStringLiteral("Kylie Minogue");
        normalizeTrackMetadata(&numMeta3);
        check("normalize: 100 Degrees preserved", numMeta3.title == QStringLiteral("100 Degrees") && numMeta3.artist == QStringLiteral("Kylie Minogue"));

        // (f) Embedded slash in artist preserved
        TrackMetadata acdcMeta;
        acdcMeta.title = QStringLiteral("AC/DC - Highway to Hell (Official Video)");
        acdcMeta.artist = QStringLiteral("AC/DC - Topic");
        acdcMeta.isWebSession = true;
        normalizeTrackMetadata(&acdcMeta);
        check("normalize: AC/DC preserved as one artist", acdcMeta.title == QStringLiteral("Highway to Hell") && acdcMeta.artist == QStringLiteral("AC/DC"));
    }

    // 4. Duration Matching Tests
    {
        check("duration: exact match", TrackMatch::matchDurations(210000, 210000, 4000));
        check("duration: within 4s tolerance (diff 2s)", TrackMatch::matchDurations(210000, 212000, 4000));
        check("duration: exactly 4s tolerance", TrackMatch::matchDurations(210000, 214000, 4000));
        check("duration: exceeds 4s tolerance (diff 5s)", !TrackMatch::matchDurations(210000, 215000, 4000));
        check("duration: target duration unknown passes", TrackMatch::matchDurations(0, 210000, 4000));
        check("duration: candidate duration unknown passes", TrackMatch::matchDurations(210000, 0, 4000));
    }

    // 5. Combined Candidate Matching (Never select solely by duration)
    {
        TrackMetadata target;
        target.title = QStringLiteral("Stay");
        target.artist = QStringLiteral("The Kid LAROI, Justin Bieber");
        target.durationMs = 141000;

        // Perfect match
        check("matchCandidate: all fields match", TrackMatch::matchCandidate(target, QStringLiteral("Stay"), QStringLiteral("The Kid LAROI"), 141000));

        // Duration matches but wrong artist -> MUST REJECT!
        check("matchCandidate: duration matches but wrong artist rejects",
              !TrackMatch::matchCandidate(target, QStringLiteral("Stay"), QStringLiteral("Rihanna"), 141000));

        // Duration matches but wrong title -> MUST REJECT!
        check("matchCandidate: duration matches but wrong title rejects",
              !TrackMatch::matchCandidate(target, QStringLiteral("Different Song"), QStringLiteral("The Kid LAROI"), 141000));

        // Duration matches but both wrong -> MUST REJECT!
        check("matchCandidate: duration-only fallback rejected",
              !TrackMatch::matchCandidate(target, QStringLiteral("Completely Wrong"), QStringLiteral("Random Artist"), 141000));

        // Title and artist match, but duration diff > 4s -> MUST REJECT!
        check("matchCandidate: duration exceeding tolerance rejects",
              !TrackMatch::matchCandidate(target, QStringLiteral("Stay"), QStringLiteral("The Kid LAROI"), 180000));
    }

    // 6. Provider Search & Exact Lookup Response Simulation
    {
        TrackMetadata target;
        target.title = QStringLiteral("Yellow");
        target.artist = QStringLiteral("Coldplay");
        target.durationMs = 200000;

        QJsonArray neteaseSongs;
        {
            QJsonObject s1;
            s1[QStringLiteral("id")] = 101;
            s1[QStringLiteral("name")] = QStringLiteral("Wrong Track");
            s1[QStringLiteral("dt")] = 200000;
            neteaseSongs.append(s1);

            QJsonObject s2;
            s2[QStringLiteral("id")] = 102;
            s2[QStringLiteral("name")] = QStringLiteral("Yellow");
            s2[QStringLiteral("dt")] = 200000;
            QJsonArray ar2;
            QJsonObject a2; a2[QStringLiteral("name")] = QStringLiteral("Someone Else");
            ar2.append(a2);
            s2[QStringLiteral("ar")] = ar2;
            neteaseSongs.append(s2);

            QJsonObject s3;
            s3[QStringLiteral("id")] = 103;
            s3[QStringLiteral("name")] = QStringLiteral("Yellow (Official Video)");
            s3[QStringLiteral("dt")] = 202000;
            QJsonArray ar3;
            QJsonObject a3; a3[QStringLiteral("name")] = QStringLiteral("Coldplay");
            ar3.append(a3);
            s3[QStringLiteral("ar")] = ar3;
            neteaseSongs.append(s3);
        }

        QString neteaseMatchedId;
        for (const QJsonValue &v : neteaseSongs) {
            const QJsonObject obj = v.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const qint64 dt = obj.value(QStringLiteral("dt")).toVariant().toLongLong();
            const QString songId = QString::number(obj.value(QStringLiteral("id")).toVariant().toLongLong());

            QStringList artistList;
            for (const QJsonValue &a : obj.value(QStringLiteral("ar")).toArray()) {
                artistList.append(a.toObject().value(QStringLiteral("name")).toString());
            }

            if (TrackMatch::matchCandidate(target, name, artistList, dt, 4000)) {
                neteaseMatchedId = songId;
                break;
            }
        }
        check("netease simulation: selected song 103, skipped 101 and 102", neteaseMatchedId == QStringLiteral("103"), neteaseMatchedId);

        // KuGou Simulation
        QJsonArray kugouSongs;
        {
            QJsonObject k1;
            k1[QStringLiteral("hash")] = QStringLiteral("HASH1");
            k1[QStringLiteral("songname")] = QStringLiteral("Random Song");
            k1[QStringLiteral("singername")] = QStringLiteral("Random Singer");
            k1[QStringLiteral("duration")] = 200;
            kugouSongs.append(k1);

            QJsonObject k2;
            k2[QStringLiteral("hash")] = QStringLiteral("HASH2");
            k2[QStringLiteral("songname")] = QStringLiteral("Yellow");
            k2[QStringLiteral("singername")] = QStringLiteral("Coldplay");
            k2[QStringLiteral("duration")] = 201;
            kugouSongs.append(k2);
        }

        QString kugouMatchedHash;
        for (const QJsonValue &v : kugouSongs) {
            const QJsonObject obj = v.toObject();
            const QString songname = obj.value(QStringLiteral("songname")).toString();
            const QString singername = obj.value(QStringLiteral("singername")).toString();
            const qint64 dur = obj.value(QStringLiteral("duration")).toInt();
            const QString hash = obj.value(QStringLiteral("hash")).toString();

            if (TrackMatch::matchCandidate(target, songname, singername, dur * 1000, 4000)) {
                kugouMatchedHash = hash;
                break;
            }
        }
        check("kugou simulation: selected HASH2, skipped HASH1", kugouMatchedHash == QStringLiteral("HASH2"), kugouMatchedHash);

        // QQ Music Simulation
        QJsonArray qqSongs;
        {
            QJsonObject q1;
            q1[QStringLiteral("mid")] = QStringLiteral("MID1");
            q1[QStringLiteral("name")] = QStringLiteral("Other Song");
            q1[QStringLiteral("interval")] = 200;
            qqSongs.append(q1);

            QJsonObject q2;
            q2[QStringLiteral("mid")] = QStringLiteral("MID2");
            q2[QStringLiteral("name")] = QStringLiteral("Yellow");
            q2[QStringLiteral("interval")] = 202;
            QJsonArray singers;
            QJsonObject s; s[QStringLiteral("name")] = QStringLiteral("Coldplay");
            singers.append(s);
            q2[QStringLiteral("singer")] = singers;
            qqSongs.append(q2);
        }

        QString qqMatchedMid;
        for (const QJsonValue &v : qqSongs) {
            const QJsonObject obj = v.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const qint64 interval = obj.value(QStringLiteral("interval")).toInt();
            const QString mid = obj.value(QStringLiteral("mid")).toString();

            QStringList singerList;
            for (const QJsonValue &s : obj.value(QStringLiteral("singer")).toArray()) {
                singerList.append(s.toObject().value(QStringLiteral("name")).toString());
            }

            if (TrackMatch::matchCandidate(target, name, singerList, interval * 1000, 4000)) {
                qqMatchedMid = mid;
                break;
            }
        }
        check("qqmusic simulation: selected MID2, skipped MID1", qqMatchedMid == QStringLiteral("MID2"), qqMatchedMid);

        // Unison Simulation
        QJsonArray unisonResults;
        {
            QJsonObject u1;
            u1[QStringLiteral("id")] = QStringLiteral("U1");
            u1[QStringLiteral("song")] = QStringLiteral("Unrelated");
            u1[QStringLiteral("artist")] = QStringLiteral("Coldplay");
            unisonResults.append(u1);

            QJsonObject u2;
            u2[QStringLiteral("id")] = QStringLiteral("U2");
            u2[QStringLiteral("song")] = QStringLiteral("Yellow");
            u2[QStringLiteral("artist")] = QStringLiteral("Coldplay");
            unisonResults.append(u2);
        }

        QString unisonMatchedId;
        for (const QJsonValue &v : unisonResults) {
            const QJsonObject obj = v.toObject();
            const QString song = obj.value(QStringLiteral("song")).toString();
            const QString artist = obj.value(QStringLiteral("artist")).toString();
            const QString idStr = obj.value(QStringLiteral("id")).toString();

            if (TrackMatch::matchCandidate(target, song, artist, 0)) {
                unisonMatchedId = idStr;
                break;
            }
        }
        check("unison simulation: selected U2, skipped U1 (no first hit bug)", unisonMatchedId == QStringLiteral("U2"), unisonMatchedId);

        // Unison Exact-ID Direct Lookup validation
        check("unison exact-ID: no metadata fields in response accepted",
              TrackMatch::validateExactLookupMetadata(target, QString(), QString(), 0));
        check("unison exact-ID: matching metadata in response accepted",
              TrackMatch::validateExactLookupMetadata(target, QStringLiteral("Yellow"), QStringLiteral("Coldplay"), 200000));
        check("unison exact-ID: mismatched title in response rejected",
              !TrackMatch::validateExactLookupMetadata(target, QStringLiteral("Wrong Title"), QStringLiteral("Coldplay"), 200000));
        check("unison exact-ID: mismatched artist in response rejected",
              !TrackMatch::validateExactLookupMetadata(target, QStringLiteral("Yellow"), QStringLiteral("Wrong Artist"), 200000));
        check("unison exact-ID: mismatched duration in response rejected",
              !TrackMatch::validateExactLookupMetadata(target, QStringLiteral("Yellow"), QStringLiteral("Coldplay"), 300000));
    }

    if (s_failed == 0) {
        std::cout << "\nAll provider matching tests PASSED successfully!\n";
        return 0;
    }

    std::cout << "\n" << s_failed << " test(s) FAILED!\n";
    return 1;
}
