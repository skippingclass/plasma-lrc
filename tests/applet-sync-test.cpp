// SPDX-License-Identifier: MIT
#include "lrcapplet.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <KPluginMetaData>
#include <QTemporaryDir>
#include <cstdio>

class LrcAppletSyncTest
{
public:
    static int run()
    {
        int failures = 0;
        auto check = [&](const char *name, bool ok) {
            std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
            failures += !ok;
        };
        LrcApplet widget(nullptr, KPluginMetaData(), {});
        widget.m_watchedService = "org.mpris.MediaPlayer2.audit";
        widget.m_player = "spotify";
        widget.m_playing = true;
        widget.m_playingKnown = true;
        widget.m_trackLengthUs = 200000000;
        auto position = [&](qint64 ms) {
            widget.m_positionValid = true;
            widget.m_positionBaseMs = ms;
            widget.m_positionClock.invalidate();
            widget.m_trackJustChanged = false;
            widget.m_gaplessLeadMs = 0;
            widget.m_recentSeekTimer.invalidate();
        };
        position(198000);
        widget.onPlayerSeeked(500000);
        check("manual seek near end does not add one second", widget.m_gaplessLeadMs == 0);
        check("manual seek is recorded", widget.m_recentSeekTimer.isValid());
        widget.applyPlayerProperties({{"LoopStatus", "Track"}}, {});
        position(198000);
        widget.onPlayerSeeked(500000);
        check("repeat enabled does not turn early manual seek into wrap", widget.m_gaplessLeadMs == 0);
        position(200000);
        widget.onPlayerSeeked(500000);
        check("actual repeat boundary retains compensation", widget.m_gaplessLeadMs == 1000);
        widget.applyPlayerProperties({{"PlaybackStatus", "Paused"}}, {});
        check("pause resets gapless lead", widget.m_gaplessLeadMs == 0);
        widget.applyPlayerProperties({}, {"LoopStatus"});
        check("invalidated loop mode cannot imply repeat", !widget.m_repeatTrack);

        // Expiry & Clipboard tests
        {
            widget.m_watchedService = QStringLiteral("org.mpris.MediaPlayer2.audit");
            widget.m_player = QStringLiteral("spotify");
            widget.m_playing = true;
            widget.m_playingKnown = true;
            widget.m_trackTitle = QStringLiteral("Spicy Track");
            widget.m_trackArtist = QStringLiteral("Spicy Artist");
            widget.m_spotifyTrackId = QStringLiteral("4cOdK2wGLETKBW3PvgPWqT");

            // Seed active Spicy lyrics with future expiry
            Lyrics spicyLyrics;
            spicyLyrics.type = LyricsType::Line;
            LyricLine line1;
            line1.text = QStringLiteral("Spicy Lyric Line 1");
            line1.startMs = 0;
            line1.endMs = 5000;
            spicyLyrics.lines.append(line1);
            spicyLyrics.attribution = QStringLiteral("Spicy Attribution");
            spicyLyrics.attributionData.provider = QStringLiteral("Spicy Lyrics");
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            spicyLyrics.validUntilMs = now + 60000; // 60s in future

            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, spicyLyrics);

            check("spicy lyrics loaded and fromSpicyLyrics is true", widget.fromSpicyLyrics());
            check("lyricsValidUntilMs returns expiry timestamp", widget.lyricsValidUntilMs() == spicyLyrics.validUntilMs);
            check("m_expiryTimer is active for positive expiry", widget.m_expiryTimer.isActive());
            check("fullLyrics returns active lyrics", widget.fullLyrics() == QStringLiteral("Spicy Lyric Line 1"));

            // 1. Spicy copy action doesn't alter clipboard
            QClipboard *clipboard = QGuiApplication::clipboard();
            clipboard->setText(QStringLiteral("sentinel-clipboard-value"));
            widget.m_lineIndex = 0;
            widget.m_text = QStringLiteral("Spicy Lyric Line 1");
            widget.copyCurrentLine();
            check("copyCurrentLine does nothing for spicy lyrics and does not alter clipboard", clipboard->text() == QStringLiteral("sentinel-clipboard-value"));

            // 2. Expired active API clearing / read guard
            // Seed expired validity timestamp while keeping m_lyrics active in memory
            widget.m_lyrics.validUntilMs = now - 5000;
            check("read guard: isSpicyExpired() is true", widget.isSpicyExpired());
            check("read guard: text() returns empty string", widget.text().isEmpty());
            check("read guard: fullLyrics() returns empty string", widget.fullLyrics().isEmpty());
            check("read guard: attribution() returns empty string", widget.attribution().isEmpty());
            check("read guard: provider() returns empty string", widget.provider().isEmpty());
            check("read guard: fromSpicyLyrics() returns false", !widget.fromSpicyLyrics());
            check("read guard: lyricsValidUntilMs() returns 0", widget.lyricsValidUntilMs() == 0);
            check("read guard: isActive() returns false", !widget.isActive());

            // Check read guard for API deadline 0
            widget.m_lyrics.validUntilMs = 0;
            check("read guard: isSpicyExpired() is true for API deadline 0", widget.isSpicyExpired());
            check("read guard: text() returns empty string for API deadline 0", widget.text().isEmpty());
            check("read guard: fromSpicyLyrics() returns false for API deadline 0", !widget.fromSpicyLyrics());
            check("read guard: lyricsValidUntilMs() returns 0 for API deadline 0", widget.lyricsValidUntilMs() == 0);

            // Normal entrypoint / cleanup event discards m_lyrics and resets state (offline isolation)
            widget.m_watchedService.clear();
            widget.onLyricsExpired();
            check("cleanup event discards m_lyrics", widget.m_lyrics.lines.isEmpty());
            check("cleanup event resets m_fromSpicy", !widget.m_fromSpicy);
            check("cleanup event stops m_expiryTimer", !widget.m_expiryTimer.isActive());

            // 3. Pause case: active lyrics discarded on expiry even when playback is paused
            spicyLyrics.validUntilMs = now + 10000;
            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, spicyLyrics);
            check("reloaded spicy lyrics before pause test", widget.fromSpicyLyrics());

            widget.applyPlayerProperties({{QStringLiteral("PlaybackStatus"), QStringLiteral("Paused")}}, {});
            check("playback is paused", !widget.isPlaying());

            // Expire deadline while paused (offline isolation)
            widget.m_lyrics.validUntilMs = now - 1000;
            widget.m_watchedService.clear();
            widget.onLyricsExpired();
            check("pause case: active m_lyrics discarded on expiry", widget.m_lyrics.lines.isEmpty());
            check("pause case: fullLyrics is empty", widget.fullLyrics().isEmpty());
            check("pause case: fromSpicyLyrics is false", !widget.fromSpicyLyrics());

            // 4. New track timer can't clear new data
            // Load track 1 with expiry timer armed
            widget.applyPlayerProperties({{QStringLiteral("PlaybackStatus"), QStringLiteral("Playing")}}, {});
            spicyLyrics.validUntilMs = now + 10000;
            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, spicyLyrics);
            const quint64 track1ExpiryGeneration = widget.m_expiryGeneration;

            // Transition to new track
            widget.m_trackTitle = QStringLiteral("New Track Title");
            widget.m_spotifyTrackId = QStringLiteral("5cOdK2wGLETKBW3PvgPWqT");
            widget.resetTrackPosition(true); // increments m_lookupGeneration

            Lyrics track2Lyrics;
            track2Lyrics.type = LyricsType::Line;
            LyricLine line2;
            line2.text = QStringLiteral("New Track Lyric Line");
            line2.startMs = 0;
            line2.endMs = 4000;
            track2Lyrics.lines.append(line2);
            track2Lyrics.validUntilMs = now + 60000;

            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, track2Lyrics);
            check("track 2 lyrics loaded", widget.fullLyrics() == QStringLiteral("New Track Lyric Line"));

            // Simulate stale track 1 timer firing with old generation (offline isolation)
            widget.m_expiryGeneration = track1ExpiryGeneration;
            widget.m_watchedService.clear();
            widget.onLyricsExpired();
            check("old timer cannot clear new track lyrics", widget.fullLyrics() == QStringLiteral("New Track Lyric Line"));
            check("track 2 lyrics remain fromSpicy", widget.fromSpicyLyrics());
            // Restore proper generation
            widget.m_expiryGeneration = widget.m_lookupGeneration;

            // 5. Reject API deadline 0 and past deadline in onSpicyLoaded with prior text, no stale visible getters
            widget.m_watchedService.clear(); // Ensure isolated/offline
            widget.m_text = QStringLiteral("Stale API Text 0");
            widget.m_word = QStringLiteral("StaleWord0");
            widget.m_wordStart = 4;
            widget.m_wordEnd = 12;
            widget.m_wordProgress = 0.6;
            widget.m_wordSynced = true;
            widget.m_fromSpicy = true;
            widget.m_expiryRetryCount = 1;

            Lyrics zeroDeadlineLyrics;
            zeroDeadlineLyrics.type = LyricsType::Line;
            LyricLine zLine;
            zLine.text = QStringLiteral("API Deadline Zero Line");
            zeroDeadlineLyrics.lines.append(zLine);
            zeroDeadlineLyrics.attribution = QStringLiteral("Zero Attribution");
            zeroDeadlineLyrics.attributionData.provider = QStringLiteral("Zero Provider");
            zeroDeadlineLyrics.validUntilMs = 0;

            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, zeroDeadlineLyrics);

            check("reject API deadline 0: fromSpicyLyrics is false", !widget.fromSpicyLyrics());
            check("reject API deadline 0: m_fromSpicy is false", !widget.m_fromSpicy);
            check("reject API deadline 0: text() is empty", widget.text().isEmpty());
            check("reject API deadline 0: m_text is cleared", widget.m_text.isEmpty());
            check("reject API deadline 0: word() is empty", widget.word().isEmpty());
            check("reject API deadline 0: m_word is cleared", widget.m_word.isEmpty());
            check("reject API deadline 0: wordStart is 0", widget.wordStart() == 0);
            check("reject API deadline 0: wordEnd is 0", widget.wordEnd() == 0);
            check("reject API deadline 0: wordProgress is 0.0", widget.wordProgress() == 0.0);
            check("reject API deadline 0: isWordSynced is false", !widget.isWordSynced());
            check("reject API deadline 0: fullLyrics() is empty", widget.fullLyrics().isEmpty());
            check("reject API deadline 0: attribution() is empty", widget.attribution().isEmpty());
            check("reject API deadline 0: provider() is empty", widget.provider().isEmpty());
            check("reject API deadline 0: lyricsValidUntilMs is 0", widget.lyricsValidUntilMs() == 0);
            check("reject API deadline 0: m_expiryTimer is inactive", !widget.m_expiryTimer.isActive());
            check("reject API deadline 0: m_expiryRetryCount is not reset", widget.m_expiryRetryCount == 1);

            // Rejection of past deadline with prior API text in m_text
            widget.m_text = QStringLiteral("Stale API Text Past");
            widget.m_word = QStringLiteral("StaleWordPast");
            widget.m_wordStart = 5;
            widget.m_wordEnd = 14;
            widget.m_wordProgress = 0.4;
            widget.m_wordSynced = true;
            widget.m_fromSpicy = true;
            widget.m_expiryRetryCount = 1;

            Lyrics pastDeadlineLyrics;
            pastDeadlineLyrics.type = LyricsType::Line;
            LyricLine pLine;
            pLine.text = QStringLiteral("API Past Deadline Line");
            pastDeadlineLyrics.lines.append(pLine);
            pastDeadlineLyrics.attribution = QStringLiteral("Past Attribution");
            pastDeadlineLyrics.attributionData.provider = QStringLiteral("Past Provider");
            pastDeadlineLyrics.validUntilMs = now - 10000;

            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, pastDeadlineLyrics);

            check("reject past deadline: fromSpicyLyrics is false", !widget.fromSpicyLyrics());
            check("reject past deadline: m_fromSpicy is false", !widget.m_fromSpicy);
            check("reject past deadline: text() is empty", widget.text().isEmpty());
            check("reject past deadline: m_text is cleared", widget.m_text.isEmpty());
            check("reject past deadline: word() is empty", widget.word().isEmpty());
            check("reject past deadline: m_word is cleared", widget.m_word.isEmpty());
            check("reject past deadline: wordStart is 0", widget.wordStart() == 0);
            check("reject past deadline: wordEnd is 0", widget.wordEnd() == 0);
            check("reject past deadline: wordProgress is 0.0", widget.wordProgress() == 0.0);
            check("reject past deadline: isWordSynced is false", !widget.isWordSynced());
            check("reject past deadline: fullLyrics() is empty", widget.fullLyrics().isEmpty());
            check("reject past deadline: attribution() is empty", widget.attribution().isEmpty());
            check("reject past deadline: provider() is empty", widget.provider().isEmpty());
            check("reject past deadline: lyricsValidUntilMs is 0", widget.lyricsValidUntilMs() == 0);
            check("reject past deadline: m_expiryTimer is inactive", !widget.m_expiryTimer.isActive());
            check("reject past deadline: m_expiryRetryCount is not reset", widget.m_expiryRetryCount == 1);

            // Valid payload accepts and resets retry count
            Lyrics validFutureLyrics;
            validFutureLyrics.type = LyricsType::Line;
            LyricLine vfLine;
            vfLine.text = QStringLiteral("Valid Future Line");
            validFutureLyrics.lines.append(vfLine);
            validFutureLyrics.validUntilMs = now + 60000;

            widget.m_fetchingFromSpicy = true;
            widget.onSpicyLoaded(widget.m_spotifyTrackId, validFutureLyrics);
            check("valid payload accepted: fromSpicyLyrics is true", widget.fromSpicyLyrics());
            check("valid payload accepted: m_expiryRetryCount is reset to 0", widget.m_expiryRetryCount == 0);
            check("valid payload accepted: fullLyrics returns text", widget.fullLyrics() == QStringLiteral("Valid Future Line"));
            check("valid payload accepted: m_expiryTimer is active", widget.m_expiryTimer.isActive());

            // 6. Native LRCLIB unaffected and native zero deadline still valid
            widget.clearLyrics();
            Lyrics lrclibLyrics;
            lrclibLyrics.type = LyricsType::Line;
            LyricLine lrcLine;
            lrcLine.text = QStringLiteral("LRCLIB Lyric Line");
            lrcLine.startMs = 0;
            lrcLine.endMs = 3000;
            lrclibLyrics.lines.append(lrcLine);
            lrclibLyrics.attribution = QStringLiteral("LRCLIB Attribution");
            lrclibLyrics.attributionData.provider = QStringLiteral("lrclib");
            lrclibLyrics.validUntilMs = 0; // Native LRCLIB expiry is 0

            widget.m_fetchingLrc = true;
            widget.m_lrclibKey = QStringLiteral("lrclib-test-key");
            widget.onLrclibLoaded(widget.m_lrclibKey, lrclibLyrics);

            check("native LRCLIB loaded successfully", !widget.fromSpicyLyrics());
            check("native LRCLIB lyricsValidUntilMs is 0", widget.lyricsValidUntilMs() == 0);
            check("native LRCLIB m_expiryTimer is inactive", !widget.m_expiryTimer.isActive());
            check("native LRCLIB fullLyrics returns text", widget.fullLyrics() == QStringLiteral("LRCLIB Lyric Line"));
            check("native LRCLIB isSpicyExpired is false", !widget.isSpicyExpired());

            // Native LRCLIB copyCurrentLine works and preserves copying without timestamps
            widget.m_lineIndex = 0;
            widget.m_text = QStringLiteral("[00:01] LRCLIB Lyric Line");
            check("native LRCLIB text() returns text with zero deadline", widget.text() == QStringLiteral("[00:01] LRCLIB Lyric Line"));
            widget.copyCurrentLine();
            check("native LRCLIB copyCurrentLine copies without timestamps", clipboard->text() == QStringLiteral("LRCLIB Lyric Line"));
        }

        widget.m_watchedService.clear();
        return failures ? 1 : 0;
    }
};
int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    qputenv("XDG_CACHE_HOME", runtime.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-plasma-lrc-test-bus");
    QApplication app(argc, argv);
    return LrcAppletSyncTest::run();
}
