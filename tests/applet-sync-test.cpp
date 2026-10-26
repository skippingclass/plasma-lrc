// SPDX-License-Identifier: MIT
#include "lrcapplet.h"
#include "lyricspipelinemanager.h"
#include "trackname.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <KPluginMetaData>
#include <QTemporaryDir>
#include <cstdio>

class FakeSettingsProvider : public LyricsProvider
{
public:
    FakeSettingsProvider() = default;
    ~FakeSettingsProvider() override = default;

    QString id() const override { return QStringLiteral("settings-test"); }
    QString displayName() const override { return QStringLiteral("Settings Test Provider"); }
    Capabilities capabilities() const override { return Capability::LineSync; }

    bool isEnabled() const override { return m_enabled; }

    bool canHandle(const TrackMetadata &meta) const override
    {
        Q_UNUSED(meta);
        return true;
    }

    void requestLyrics(const TrackMetadata &meta, quint64 generation) override
    {
        requestCalls++;
        lastGeneration = generation;
        lastMeta = meta;
    }

    void cancel() override
    {
        cancelCalls++;
    }

    int requestCalls = 0;
    int cancelCalls = 0;
    quint64 lastGeneration = 0;
    TrackMetadata lastMeta;
};

class LrcAppletSyncTest
{
public:
    static int run()
    {
        int failures = 0;
        auto check = [&](const char *name, bool ok, const QString &detail = QString()) {
            if (!ok && !detail.isEmpty()) {
                std::printf("%s: FAIL (%s)\n", name, detail.toUtf8().constData());
            } else {
                std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
            }
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

        // 7. Pipeline resolution, provisional -> upgrade progress, stale line resurrection, and allFailed
        {
            widget.m_watchedService = QStringLiteral("org.mpris.MediaPlayer2.audit");
            widget.m_player = QStringLiteral("spotify");
            widget.m_playing = true;
            widget.m_playingKnown = true;
            widget.m_trackTitle = QStringLiteral("Pipeline Test Track");
            widget.m_trackArtist = QStringLiteral("Pipeline Test Artist");
            widget.m_spotifyTrackId = QStringLiteral("spotify-test-id-123");
            widget.m_lookupGeneration = 10;
            widget.m_fetchingLrc = true;
            widget.m_lyricsLookupDone = false;

            const qint64 now = QDateTime::currentMSecsSinceEpoch();

            // A. Provisional Line lyrics from provider "lrclib"
            Lyrics provLyrics;
            provLyrics.type = LyricsType::Line;
            LyricLine pLine;
            pLine.text = QStringLiteral("Provisional Line Lyrics");
            pLine.startMs = 0;
            pLine.endMs = 5000;
            provLyrics.lines.append(pLine);
            provLyrics.attribution = QStringLiteral("LRCLIB attribution");
            provLyrics.attributionData.provider = QStringLiteral("lrclib");
            provLyrics.validUntilMs = 0;

            widget.onPipelineResolved(10, provLyrics, QStringLiteral("lrclib"));

            check("pipeline provisional: text is set", widget.fullLyrics() == QStringLiteral("Provisional Line Lyrics"));
            check("pipeline provisional: provider is lrclib", widget.activeProviderId() == QStringLiteral("lrclib"));
            check("pipeline provisional: fromSpicy is false", !widget.fromSpicyLyrics());
            check("pipeline provisional: isWordSynced is false", !widget.isWordSynced());
            check("pipeline provisional: fetching remains in flight", widget.m_fetchingLrc);
            check("pipeline provisional: lookupDone is false so continuing pipeline not blocked", !widget.m_lyricsLookupDone);
            check("pipeline provisional: expiry timer not active", !widget.m_expiryTimer.isActive());

            // B. Obsolete generation callback during provisional does not overwrite
            Lyrics staleGenLyrics;
            staleGenLyrics.type = LyricsType::Syllable;
            LyricLine sgLine;
            sgLine.text = QStringLiteral("Stale Generation Lyrics");
            staleGenLyrics.lines.append(sgLine);
            widget.onPipelineResolved(9, staleGenLyrics, QStringLiteral("spicy"));
            check("generation guard: stale generation rejected", widget.fullLyrics() == QStringLiteral("Provisional Line Lyrics"));
            check("generation guard: provider remains lrclib", widget.activeProviderId() == QStringLiteral("lrclib"));

            // C. Upgrade to Syllable lyrics from "spicy" with valid future deadline
            Lyrics upgradeLyrics;
            upgradeLyrics.type = LyricsType::Syllable;
            LyricLine uLine;
            uLine.text = QStringLiteral("Upgraded Syllable Lyrics");
            uLine.startMs = 0;
            uLine.endMs = 5000;
            upgradeLyrics.lines.append(uLine);
            upgradeLyrics.attribution = QStringLiteral("Spicy Community");
            upgradeLyrics.attributionData.provider = QStringLiteral("Spicy Lyrics");
            upgradeLyrics.validUntilMs = now + 60000;

            widget.onPipelineResolved(10, upgradeLyrics, QStringLiteral("spicy"));

            check("pipeline upgrade: text updated to syllable", widget.fullLyrics() == QStringLiteral("Upgraded Syllable Lyrics"));
            check("pipeline upgrade: provider upgraded to spicy", widget.activeProviderId() == QStringLiteral("spicy"));
            check("pipeline upgrade: fromSpicy is true", widget.fromSpicyLyrics());
            check("pipeline upgrade: isWordSynced is true", widget.isWordSynced());
            check("pipeline upgrade: lookupDone is true on syllable", widget.m_lyricsLookupDone);
            check("pipeline upgrade: fetching is finished", !widget.m_fetchingLrc);
            check("pipeline upgrade: expiry timer armed", widget.m_expiryTimer.isActive());
            check("pipeline upgrade: expiry timestamp valid", widget.lyricsValidUntilMs() == upgradeLyrics.validUntilMs);
            check("pipeline upgrade: expiry generation matches", widget.m_expiryGeneration == 10);

            // D. Upgrade from Spicy to a non-Spicy provider stops expiry timer and resets fromSpicy
            Lyrics amllUpgradeLyrics;
            amllUpgradeLyrics.type = LyricsType::Syllable;
            LyricLine aLine;
            aLine.text = QStringLiteral("AMLL Upgraded Lyrics");
            aLine.startMs = 0;
            aLine.endMs = 5000;
            amllUpgradeLyrics.lines.append(aLine);
            amllUpgradeLyrics.attribution = QStringLiteral("AMLL Provider");
            amllUpgradeLyrics.attributionData.provider = QStringLiteral("amll");
            amllUpgradeLyrics.validUntilMs = 0;

            widget.onPipelineResolved(10, amllUpgradeLyrics, QStringLiteral("amll"));

            check("spicy upgrade to non-spicy: provider is amll", widget.activeProviderId() == QStringLiteral("amll"));
            check("spicy upgrade to non-spicy: fromSpicy is false", !widget.fromSpicyLyrics());
            check("spicy upgrade to non-spicy: expiry timer stopped", !widget.m_expiryTimer.isActive());
            check("spicy upgrade to non-spicy: lyricsValidUntilMs is 0", widget.lyricsValidUntilMs() == 0);
            check("spicy upgrade to non-spicy: text updated", widget.fullLyrics() == QStringLiteral("AMLL Upgraded Lyrics"));

            // E. Stale line resurrection prevention on onPipelineAllFailed
            widget.m_lookupGeneration = 11;
            widget.m_fetchingLrc = true;
            widget.m_lyricsLookupDone = false;

            // Pipeline fails for all providers on generation 11
            widget.onPipelineAllFailed(11, MissingReason::NotFound);

            check("allFailed: fetching cleared", !widget.m_fetchingLrc);
            check("allFailed: activeProvider cleared", widget.activeProviderId().isEmpty());
            check("allFailed: m_lyrics cleared", widget.m_lyrics.lines.isEmpty());
            check("allFailed: fullLyrics is empty", widget.fullLyrics().isEmpty());
            check("allFailed: text() is empty", widget.text().isEmpty());
            check("allFailed: attribution is empty", widget.attribution().isEmpty());
            check("allFailed: provider is empty", widget.provider().isEmpty());
            check("allFailed: fromSpicyLyrics is false", !widget.fromSpicyLyrics());
            check("allFailed: expiry timer inactive", !widget.m_expiryTimer.isActive());

            // Simulate wordTimer ticking by calling updateWord() - must NOT resurrect old lyrics!
            widget.m_positionValid = true;
            widget.m_positionMs = 1000;
            widget.updateWord();
            check("stale line resurrection prevented: text() remains empty after updateWord", widget.text().isEmpty());
            check("stale line resurrection prevented: word() remains empty after updateWord", widget.word().isEmpty());

            // F. onPipelineLookupFinished completion handling
            widget.m_lookupGeneration = 12;
            widget.m_fetchingLrc = true;
            widget.m_lyricsLookupDone = false;
            widget.onPipelineLookupFinished(12);
            check("lookupFinished: fetching marked finished", !widget.m_fetchingLrc);
            check("lookupFinished: lookupDone marked true", widget.m_lyricsLookupDone);
        }

        // 8. Settings change entrypoint tests: provider settings vs appearance settings
        {
            // Disable all production providers persistently using config General disabledProviders
            QStringList prodDisabled;
            for (LyricsProvider *p : widget.m_pipeline->allProviders()) {
                if (p) {
                    prodDisabled.append(p->id());
                }
            }
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("disabledProviders"), prodDisabled.join(QLatin1Char(',')));

            FakeSettingsProvider &fakeProvider = *new FakeSettingsProvider;
            fakeProvider.setParent(widget.m_pipeline);
            widget.m_pipeline->registerProvider(&fakeProvider);
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("providerOrder"), QStringLiteral("settings-test"));

            widget.m_watchedService.clear();
            widget.readSettings();

            widget.m_watchedService = QStringLiteral("org.mpris.MediaPlayer2.audit");
            widget.m_trackTitle = QStringLiteral("Settings Test Track");
            widget.m_trackArtist = QStringLiteral("Settings Test Artist");
            widget.m_lyricsLookupDone = true;
            widget.m_lookupGeneration = 20;

            // Custom URL change re-requests
            const int callsBeforeCustom = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("customLyricsUrl"), QStringLiteral("https://custom.test/{title}"));
            widget.readSettings();
            check("settings: customLyricsUrl re-requests with invalidated lookupDone", !widget.m_lyricsLookupDone);
            check("settings: customLyricsUrl bumps generation", widget.m_lookupGeneration > 20);
            check("settings: customUrlConfig updated", widget.m_customUrlConfig == QStringLiteral("https://custom.test/{title}"));
            check("settings: customLyricsUrl calls fake provider", fakeProvider.requestCalls == callsBeforeCustom + 1);
            check("settings: customLyricsUrl fake provider generation matches", fakeProvider.lastGeneration == widget.m_lookupGeneration);

            // Local folder change re-requests
            widget.m_lyricsLookupDone = true;
            const quint64 genAfterCustom = widget.m_lookupGeneration;
            const int callsBeforeLocal = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("localLyricsFolder"), QStringLiteral("/tmp/local_lyrics_test"));
            widget.readSettings();
            check("settings: localLyricsFolder re-requests with invalidated lookupDone", !widget.m_lyricsLookupDone);
            check("settings: localLyricsFolder bumps generation", widget.m_lookupGeneration > genAfterCustom);
            check("settings: localFolderConfig updated", widget.m_localFolderConfig == QStringLiteral("/tmp/local_lyrics_test"));
            check("settings: localLyricsFolder calls fake provider", fakeProvider.requestCalls == callsBeforeLocal + 1);
            check("settings: localLyricsFolder fake provider generation matches", fakeProvider.lastGeneration == widget.m_lookupGeneration);

            // Spicy key change re-requests even when currently active
            widget.m_lyricsLookupDone = true;
            const quint64 genBeforeKey = widget.m_lookupGeneration;
            const int callsBeforeKey = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("spicyKey"), QStringLiteral("test-spicy-key-456"));
            widget.readSettings();
            check("settings: spicy key change re-requests with invalidated lookupDone", !widget.m_lyricsLookupDone);
            check("settings: spicy key change bumps generation", widget.m_lookupGeneration > genBeforeKey);
            check("settings: spicyKey updated", widget.m_spicyKey == QStringLiteral("test-spicy-key-456"));
            check("settings: actual key applied to m_spicy", widget.m_spicy->key() == QStringLiteral("test-spicy-key-456"));
            check("settings: spicy key change calls fake provider", fakeProvider.requestCalls == callsBeforeKey + 1);
            check("settings: spicy key change fake provider generation matches", fakeProvider.lastGeneration == widget.m_lookupGeneration);

            // Provider enable change (useSpicy false) re-requests
            widget.m_lyricsLookupDone = true;
            const quint64 genBeforeEnable = widget.m_lookupGeneration;
            const int callsBeforeEnable = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("useSpicyLyrics"), false);
            widget.readSettings();
            check("settings: useSpicyLyrics change re-requests with invalidated lookupDone", !widget.m_lyricsLookupDone);
            check("settings: useSpicyLyrics change bumps generation", widget.m_lookupGeneration > genBeforeEnable);
            check("settings: useSpicy is false", !widget.m_useSpicy);
            check("settings: actual enable applied to m_spicy (disabled)", !widget.m_spicy->isEnabled());
            check("settings: production spicy stays disabled via disabledProviders", !widget.m_pipeline->provider(QStringLiteral("spicy"))->isEnabled());
            check("settings: useSpicyLyrics change calls fake provider", fakeProvider.requestCalls == callsBeforeEnable + 1);
            check("settings: useSpicyLyrics change fake provider generation matches", fakeProvider.lastGeneration == widget.m_lookupGeneration);

            // Provider enable change (useSpicy true) re-requests
            widget.m_lyricsLookupDone = true;
            const quint64 genBeforeEnableTrue = widget.m_lookupGeneration;
            const int callsBeforeEnableTrue = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("useSpicyLyrics"), true);
            widget.readSettings();
            check("settings: useSpicyLyrics true re-requests with invalidated lookupDone", !widget.m_lyricsLookupDone);
            check("settings: useSpicyLyrics true bumps generation", widget.m_lookupGeneration > genBeforeEnableTrue);
            check("settings: useSpicy is true", widget.m_useSpicy);
            check("settings: actual enable applied to m_spicy (enabled)", widget.m_spicy->isEnabled());
            check("settings: production spicy stays disabled when useSpicy true", !widget.m_pipeline->provider(QStringLiteral("spicy"))->isEnabled());
            check("settings: useSpicyLyrics true calls fake provider", fakeProvider.requestCalls == callsBeforeEnableTrue + 1);
            check("settings: useSpicyLyrics true fake provider generation matches", fakeProvider.lastGeneration == widget.m_lookupGeneration);

            // Appearance settings changes (wordStyle, maxCharacters) must NOT re-request
            widget.m_lyricsLookupDone = true;
            const quint64 genBeforeAppearance = widget.m_lookupGeneration;
            const int callsBeforeAppearance = fakeProvider.requestCalls;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("wordStyle"), 2);
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("maxCharacters"), 120);
            widget.readSettings();
            check("settings: appearance change does not re-request (lookupDone remains true)", widget.m_lyricsLookupDone);
            check("settings: appearance change does not bump generation", widget.m_lookupGeneration == genBeforeAppearance);
            check("settings: appearance change does not call fake provider", fakeProvider.requestCalls == callsBeforeAppearance);
        }

        // -------------------------------------------------------------
        // Backing vocals, Simultaneous highlights, Primary priority, Reset & Search metadata tests
        // -------------------------------------------------------------
        {
            // 1. Entrypoints & Default settings
            check("backing: showBackingVocals defaults to false", !widget.showBackingVocals());
            check("backing: backingText defaults to empty", widget.backingText().isEmpty());
            check("backing: backingWordStart defaults to 0", widget.backingWordStart() == 0);
            check("backing: backingWordEnd defaults to 0", widget.backingWordEnd() == 0);

            // Toggle showBackingVocals via settings without refetching providers
            widget.m_lyricsLookupDone = true;
            const quint64 genBeforeToggle = widget.m_lookupGeneration;
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
            widget.readSettings();
            check("backing: showBackingVocals updated to true", widget.showBackingVocals());
            check("backing: toggle does not invalidate lookupDone", widget.m_lyricsLookupDone);
            check("backing: toggle does not bump generation", widget.m_lookupGeneration == genBeforeToggle);

            // 2. Prepare syllable test lyrics with overlapping main and backing words
            // Line: "bands (Blegh, blegh)"
            // Word 0 (main): "bands" [1000..2000ms, groupStart=0, groupEnd=5, isBackground=false]
            // Word 1 (back): "(Blegh," [1000..1500ms, groupStart=6, groupEnd=13, isBackground=true, backgroundGroup=0]
            // Word 2 (back): "blegh)" [1500..2000ms, groupStart=14, groupEnd=20, isBackground=true, backgroundGroup=0]
            LyricLine testLine;
            testLine.text = QStringLiteral("bands (Blegh, blegh)");
            testLine.startMs = 1000;
            testLine.endMs = 2500;
            testLine.isBackground = false;

            LyricWord w0;
            w0.text = QStringLiteral("bands");
            w0.startMs = 1000;
            w0.endMs = 2000;
            w0.groupStart = 0;
            w0.groupEnd = 5;
            w0.isBackground = false;
            w0.backgroundGroup = -1;

            LyricWord w1;
            w1.text = QStringLiteral("(Blegh,");
            w1.startMs = 1000;
            w1.endMs = 1500;
            w1.groupStart = 6;
            w1.groupEnd = 13;
            w1.isBackground = true;
            w1.backgroundGroup = 0;

            LyricWord w2;
            w2.text = QStringLiteral("blegh)");
            w2.startMs = 1500;
            w2.endMs = 2000;
            w2.groupStart = 14;
            w2.groupEnd = 20;
            w2.isBackground = true;
            w2.backgroundGroup = 0;

            testLine.words = {w0, w1, w2};

            Lyrics testLyrics;
            testLyrics.type = LyricsType::Syllable;
            testLyrics.lines = {testLine};

            widget.m_lyrics = testLyrics;
            widget.m_wordSynced = true;
            widget.m_positionValid = true;
            widget.m_playing = true;
            widget.m_playingKnown = true;
            widget.m_pauseHidden = false;
            widget.m_positionClock.invalidate();
            widget.m_gaplessLeadMs = 0;
            widget.m_lyricOffset = 0;

            // 3. Test primary priority in legacy mode (showBackingVocals = false)
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), false);
            widget.readSettings();
            widget.m_positionBaseMs = 1200; // at 1200ms, both w0 (main) and w1 (back) are active
            widget.updateWord();

            check("legacy: full line text displayed", widget.text() == QStringLiteral("bands (Blegh, blegh)"));
            check("legacy: primary priority selects main word", widget.word() == QStringLiteral("bands"));
            check("legacy: main word start is 0", widget.wordStart() == 0);
            check("legacy: main word end is 5", widget.wordEnd() == 5);
            check("legacy: backingText is empty", widget.backingText().isEmpty());
            check("legacy: backingWordStart is 0", widget.backingWordStart() == 0);
            check("legacy: backingWordEnd is 0", widget.backingWordEnd() == 0);

            // 4. Test SIMULTANEOUS & independent highlights (showBackingVocals = true)
            widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
            widget.readSettings();

            // At 1200ms: Word 0 ("bands") and Word 1 ("(Blegh,") are both singing
            widget.m_positionBaseMs = 1200;
            widget.updateWord();

            check("separate: main text excludes bg range", widget.text() == QStringLiteral("bands"));
            check("separate: main word is bands", widget.word() == QStringLiteral("bands"));
            check("separate: main wordStart is 0", widget.wordStart() == 0);
            check("separate: main wordEnd is 5", widget.wordEnd() == 5);
            check("separate: backingText is (Blegh, blegh)", widget.backingText() == QStringLiteral("(Blegh, blegh)"));
            check("separate: backingWordStart is 0", widget.backingWordStart() == 0);
            check("separate: backingWordEnd is 7 for (Blegh,", widget.backingWordEnd() == 7);

            // At 1700ms: Word 0 ("bands") is still singing, and Word 2 ("blegh)") is singing
            widget.m_positionBaseMs = 1700;
            widget.updateWord();

            check("simultaneous: main word remains bands", widget.word() == QStringLiteral("bands"));
            check("simultaneous: backingWordStart advances to 8", widget.backingWordStart() == 8);
            check("simultaneous: backingWordEnd advances to 14 for blegh)", widget.backingWordEnd() == 14);

            // 5. Test Bg-only line sets active true
            LyricLine bgOnlyLine;
            bgOnlyLine.text = QStringLiteral("(Backing Only)");
            bgOnlyLine.startMs = 3000;
            bgOnlyLine.endMs = 4000;
            bgOnlyLine.isBackground = true;
            LyricWord bgW;
            bgW.text = QStringLiteral("(Backing Only)");
            bgW.startMs = 3000;
            bgW.endMs = 4000;
            bgW.groupStart = 0;
            bgW.groupEnd = 14;
            bgW.isBackground = true;
            bgOnlyLine.words = {bgW};

            testLyrics.lines.append(bgOnlyLine);
            widget.m_lyrics = testLyrics;
            widget.m_positionBaseMs = 3500;
            widget.updateWord();

            check("bg-only: main text is empty", widget.text().isEmpty());
            check("bg-only: backingText has content", widget.backingText() == QStringLiteral("(Backing Only)"));
            check("bg-only: active is true when bg-only and experimental nonempty", widget.isActive());

            // 6. Test pause reset, track change, outro, expiry clears backing
            widget.m_playing = false;
            widget.m_playingKnown = true;
            widget.onPauseTimeout();
            check("pause reset: backingText cleared", widget.backingText().isEmpty());
            check("pause reset: backingWordStart is 0", widget.backingWordStart() == 0);
            check("pause reset: backingWordEnd is 0", widget.backingWordEnd() == 0);

            // Restore position and playing state explicitly before later cases
            widget.m_playing = true;
            widget.m_playingKnown = true;
            widget.m_pauseHidden = false;
            widget.m_positionClock.invalidate();
            widget.m_gaplessLeadMs = 0;
            widget.m_lyricOffset = 0;
            widget.m_positionBaseMs = 1200;
            widget.updateWord();
            check("restore: backingText restored", !widget.backingText().isEmpty());

            widget.clearDisplay();
            check("clearDisplay: backingText cleared", widget.backingText().isEmpty());

            // 7. Test search metadata normalization in requestLyrics and refresh
            {
                TrackMetadata rawMeta;
                rawMeta.title = QStringLiteral("Song Title (Official Video) [4K]");
                rawMeta.artist = QStringLiteral("Artist Name - Topic");
                rawMeta.url = QStringLiteral("file:///home/user/Music/song.mp3");

                TrackMetadata normMeta = rawMeta;
                normalizeTrackMetadata(&normMeta);

                check("normalization: title cleaned", normMeta.title == QStringLiteral("Song Title"), normMeta.title);
                check("normalization: artist cleaned", normMeta.artist == QStringLiteral("Artist Name"), normMeta.artist);
                check("normalization: url preserved for local file match", normMeta.url == rawMeta.url);

                widget.m_trackTitle = rawMeta.title;
                widget.m_trackArtist = rawMeta.artist;
                widget.m_trackUrl = rawMeta.url;

                // Test that requestLyrics does not mutate displayed raw title/artist
                widget.m_watchedService = QStringLiteral("org.mpris.MediaPlayer2.mock");
                widget.m_lyricsLookupDone = false;
                widget.requestLyrics();

                check("requestLyrics: displayed title unmutated", widget.m_trackTitle == rawMeta.title);
                check("requestLyrics: displayed artist unmutated", widget.m_trackArtist == rawMeta.artist);
            }

            // 8. Regression (1): BG paragraph starts later than primary main line (main 0..5000, bg 1000..3000 at 1500 => BOTH active)
            {
                LyricLine mainLine0;
                mainLine0.text = QStringLiteral("Primary Main Line");
                mainLine0.startMs = 0;
                mainLine0.endMs = 5000;
                mainLine0.isBackground = false;

                LyricWord mw0;
                mw0.text = QStringLiteral("Primary");
                mw0.startMs = 0;
                mw0.endMs = 2500;
                mw0.groupStart = 0;
                mw0.groupEnd = 7;
                mw0.isBackground = false;
                mainLine0.words = {mw0};

                LyricLine bgLine1;
                bgLine1.text = QStringLiteral("(Backing Later Paragraph)");
                bgLine1.startMs = 1000;
                bgLine1.endMs = 3000;
                bgLine1.isBackground = true;

                LyricWord bw1;
                bw1.text = QStringLiteral("(Backing Later Paragraph)");
                bw1.startMs = 1000;
                bw1.endMs = 3000;
                bw1.groupStart = 0;
                bw1.groupEnd = 25;
                bw1.isBackground = true;
                bgLine1.words = {bw1};

                Lyrics overlapLyrics;
                overlapLyrics.type = LyricsType::Syllable;
                overlapLyrics.lines = {mainLine0, bgLine1};

                widget.m_lyrics = overlapLyrics;
                widget.m_wordSynced = true;
                widget.m_positionValid = true;
                widget.m_playing = true;
                widget.m_playingKnown = true;
                widget.m_pauseHidden = false;
                widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
                widget.readSettings();

                widget.m_positionBaseMs = 1500;
                widget.updateWord();

                check("regression 1: main line active at 1500ms", widget.text() == QStringLiteral("Primary Main Line"));
                check("regression 1: bg line active at 1500ms", widget.backingText() == QStringLiteral("(Backing Later Paragraph)"));
                check("regression 1: both active", widget.isActive());
            }

            // 9. Regression (2): Legacy mode single wordAt (main ends 1000, bg 900..1500, at 1050 picks bg)
            {
                LyricLine legLine;
                legLine.text = QStringLiteral("main (back)");
                legLine.startMs = 800;
                legLine.endMs = 2000;
                legLine.isBackground = false;

                LyricWord mw;
                mw.text = QStringLiteral("main");
                mw.startMs = 800;
                mw.endMs = 1000;
                mw.groupStart = 0;
                mw.groupEnd = 4;
                mw.isBackground = false;

                LyricWord bw;
                bw.text = QStringLiteral("(back)");
                bw.startMs = 900;
                bw.endMs = 1500;
                bw.groupStart = 5;
                bw.groupEnd = 11;
                bw.isBackground = true;

                legLine.words = {mw, bw};

                Lyrics legLyrics;
                legLyrics.type = LyricsType::Syllable;
                legLyrics.lines = {legLine};

                widget.m_lyrics = legLyrics;
                widget.m_wordSynced = true;
                widget.m_playing = true;
                widget.m_playingKnown = true;
                widget.m_pauseHidden = false;
                widget.m_gaplessLeadMs = 0;
                widget.m_lyricOffset = 0;
                widget.m_positionClock.invalidate();
                widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), false);
                widget.readSettings();

                // updateWord subtracts 90ms audio lead compensation, so setting base 1140 yields effective 1050ms
                widget.m_positionBaseMs = 1140;
                widget.updateWord();

                check("regression 2: effective position is 1050ms", widget.m_positionMs == 1050, QString::number(widget.m_positionMs));
                check("regression 2: legacy picks active bg at 1050ms", widget.word() == QStringLiteral("(back)"));
                check("regression 2: legacy wordStart is 5", widget.wordStart() == 5);
                check("regression 2: legacy wordEnd is 11", widget.wordEnd() == 11);
            }

            // 10. Regression (3): Multiple simultaneous nested BG groups in same line
            {
                LyricLine multiLine;
                multiLine.text = QStringLiteral("main (first) (second)");
                multiLine.startMs = 1000;
                multiLine.endMs = 3000;
                multiLine.isBackground = false;

                LyricWord mw;
                mw.text = QStringLiteral("main");
                mw.startMs = 1000;
                mw.endMs = 3000;
                mw.groupStart = 0;
                mw.groupEnd = 4;
                mw.isBackground = false;

                LyricWord bg1;
                bg1.text = QStringLiteral("(first)");
                bg1.startMs = 1000;
                bg1.endMs = 1500;
                bg1.groupStart = 5;
                bg1.groupEnd = 12;
                bg1.isBackground = true;
                bg1.backgroundGroup = 0;

                LyricWord bg2;
                bg2.text = QStringLiteral("(second)");
                bg2.startMs = 1500;
                bg2.endMs = 2000;
                bg2.groupStart = 13;
                bg2.groupEnd = 21;
                bg2.isBackground = true;
                bg2.backgroundGroup = 1;

                multiLine.words = {mw, bg1, bg2};

                Lyrics multiLyrics;
                multiLyrics.type = LyricsType::Syllable;
                multiLyrics.lines = {multiLine};

                widget.m_lyrics = multiLyrics;
                widget.m_wordSynced = true;
                widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
                widget.readSettings();

                // At 1200ms: phrase 0 (first) is active
                widget.m_positionBaseMs = 1200;
                widget.updateWord();
                check("regression 3: phrase 0 text is (first)", widget.backingText() == QStringLiteral("(first)"));
                check("regression 3: phrase 0 wordStart is 0", widget.backingWordStart() == 0);
                check("regression 3: phrase 0 wordEnd is 7", widget.backingWordEnd() == 7);

                // At 1700ms: phrase 1 (second) is active
                widget.m_positionBaseMs = 1700;
                widget.updateWord();
                check("regression 3: phrase 1 text is (second)", widget.backingText() == QStringLiteral("(second)"));
                check("regression 3: phrase 1 wordStart is 0", widget.backingWordStart() == 0);
                check("regression 3: phrase 1 wordEnd is 8", widget.backingWordEnd() == 8);
            }

            // 11. Regression (4): Bg-only line end clears active
            {
                LyricLine bgOnly;
                bgOnly.text = QStringLiteral("(Outro Backing)");
                bgOnly.startMs = 1000;
                bgOnly.endMs = 2000;
                bgOnly.isBackground = true;

                LyricWord bw;
                bw.text = QStringLiteral("(Outro Backing)");
                bw.startMs = 1000;
                bw.endMs = 2000;
                bw.groupStart = 0;
                bw.groupEnd = 15;
                bw.isBackground = true;
                bgOnly.words = {bw};

                Lyrics bgOnlyLyrics;
                bgOnlyLyrics.type = LyricsType::Syllable;
                bgOnlyLyrics.lines = {bgOnly};

                widget.m_lyrics = bgOnlyLyrics;
                widget.m_wordSynced = true;
                widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
                widget.readSettings();

                widget.m_positionBaseMs = 1500;
                widget.updateWord();
                check("regression 4: bg active at 1500", widget.isActive());
                check("regression 4: backingText at 1500", widget.backingText() == QStringLiteral("(Outro Backing)"));

                // Seek/advance past bg line end
                widget.m_positionBaseMs = 2500;
                widget.updateWord();
                check("regression 4: backingText cleared after bg only line ends", widget.backingText().isEmpty());
                check("regression 4: active false after bg only line ends", !widget.isActive());
            }

            {
                // Regression 5: Repeated lines with identical main and backing lyrics (Playboi Carti - Stop Breathing)
                LyricWord w1_1;
                w1_1.text = QStringLiteral("I take");
                w1_1.startMs = 1000;
                w1_1.endMs = 2000;
                w1_1.groupStart = 0;
                w1_1.groupEnd = 6;
                w1_1.isBackground = false;

                LyricWord w1_bg;
                w1_bg.text = QStringLiteral("(Yeah)");
                w1_bg.startMs = 2200;
                w1_bg.endMs = 2800;
                w1_bg.groupStart = 7;
                w1_bg.groupEnd = 13;
                w1_bg.isBackground = true;
                w1_bg.backgroundGroup = 0;

                LyricLine line1;
                line1.text = QStringLiteral("I take (Yeah)");
                line1.startMs = 1000;
                line1.endMs = 3000;
                line1.isBackground = false;
                line1.words = {w1_1, w1_bg};

                LyricWord w2_1;
                w2_1.text = QStringLiteral("I take");
                w2_1.startMs = 3000;
                w2_1.endMs = 4000;
                w2_1.groupStart = 0;
                w2_1.groupEnd = 6;
                w2_1.isBackground = false;

                LyricWord w2_bg;
                w2_bg.text = QStringLiteral("(Yeah)");
                w2_bg.startMs = 4200;
                w2_bg.endMs = 4800;
                w2_bg.groupStart = 7;
                w2_bg.groupEnd = 13;
                w2_bg.isBackground = true;
                w2_bg.backgroundGroup = 0;

                LyricLine line2;
                line2.text = QStringLiteral("I take (Yeah)");
                line2.startMs = 3000;
                line2.endMs = 5000;
                line2.isBackground = false;
                line2.words = {w2_1, w2_bg};

                Lyrics cartiLyrics;
                cartiLyrics.type = LyricsType::Syllable;
                cartiLyrics.lines = {line1, line2};

                widget.m_lyrics = cartiLyrics;
                widget.m_wordSynced = true;
                widget.config().group(QStringLiteral("General")).writeEntry(QStringLiteral("showBackingVocals"), true);
                widget.readSettings();

                // Position in line 1 during backing vocal (Yeah)
                widget.m_positionBaseMs = 2500;
                widget.updateWord();
                check("regression 5: line 1 index is 0", widget.lineIndex() == 0);
                check("regression 5: line 1 text matches", widget.text() == QStringLiteral("I take"));
                check("regression 5: line 1 backingText matches", widget.backingText() == QStringLiteral("(Yeah)"));
                check("regression 5: line 1 backingWordEnd is positive (sung)", widget.backingWordEnd() > 0);

                // Position advances to line 2 during first main word "I", before backing vocal
                widget.m_positionBaseMs = 3200;
                widget.updateWord();
                check("regression 5: line 2 index is 1", widget.lineIndex() == 1);
                check("regression 5: line 2 text matches", widget.text() == QStringLiteral("I take"));
                check("regression 5: line 2 backingText matches", widget.backingText() == QStringLiteral("(Yeah)"));
                check("regression 5: line 2 backingWordEnd reset to 0 before backing sung", widget.backingWordEnd() == 0);
            }

            // Regression 6: hasLyrics, isIntro, isBreak logic
            {
                LyricLine lineA;
                lineA.text = QStringLiteral("Intro track line 1");
                lineA.startMs = 15000;
                lineA.endMs = 20000;

                LyricLine lineB;
                lineB.text = QStringLiteral("Track line 2 after break");
                lineB.startMs = 30000;
                lineB.endMs = 35000;

                Lyrics trackLyrics;
                trackLyrics.type = LyricsType::Line;
                trackLyrics.lines = {lineA, lineB};

                widget.m_lyrics = trackLyrics;

                // Before line 1 (intro at 5000ms)
                widget.m_positionBaseMs = 5000;
                widget.updateWord();
                check("regression 6: intro hasLyrics is true", widget.hasLyrics());
                check("regression 6: isIntro is true at 5s", widget.isIntro());
                check("regression 6: isBreak is false at 5s", !widget.isBreak());
                check("regression 6: text is empty during intro", widget.text().isEmpty());

                // Line 1 actively playing at 16000ms
                widget.m_positionBaseMs = 16000;
                widget.updateWord();
                check("regression 6: isIntro is false during line 1", !widget.isIntro());
                check("regression 6: isBreak is false during line 1", !widget.isBreak());
                check("regression 6: lineIndex is 0", widget.lineIndex() == 0);

                // Instrumental break at 25000ms (between line 1 and line 2, gap is 10s >= 2.5s, > 1.2s grace)
                widget.m_positionBaseMs = 25000;
                widget.updateWord();
                check("regression 6: isIntro is false during break", !widget.isIntro());
                check("regression 6: isBreak is true during break", widget.isBreak());
                check("regression 6: lineIndex is -1 during break", widget.lineIndex() == -1);

                // Line 2 actively playing at 31000ms
                widget.m_positionBaseMs = 31000;
                widget.updateWord();
                check("regression 6: isIntro is false during line 2", !widget.isIntro());
                check("regression 6: isBreak is false during line 2", !widget.isBreak());
                check("regression 6: lineIndex is 1", widget.lineIndex() == 1);
            }

            // Regression 7: discard lyrics this time, restore lyrics, and reset on track change
            {
                LyricLine lineA;
                lineA.text = QStringLiteral("Test line for discard");
                lineA.startMs = 5000;
                lineA.endMs = 15000;

                Lyrics trackLyrics;
                trackLyrics.type = LyricsType::Line;
                trackLyrics.lines = {lineA};

                widget.m_trackTitle = QStringLiteral("Discard Song");
                widget.m_lyrics = trackLyrics;
                widget.m_positionBaseMs = 6000;
                widget.updateWord();

                check("regression 7: initial hasLyrics is true", widget.hasLyrics());
                check("regression 7: initial isLyricsDiscarded is false", !widget.isLyricsDiscarded());
                check("regression 7: text is active", !widget.text().isEmpty());

                // Discard lyrics for current track
                widget.discardCurrentLyrics();
                check("regression 7: isLyricsDiscarded is true", widget.isLyricsDiscarded());
                check("regression 7: hasLyrics is false after discard", !widget.hasLyrics());
                check("regression 7: text is empty after discard", widget.text().isEmpty());

                // Restore lyrics
                widget.restoreCurrentLyrics();
                check("regression 7: isLyricsDiscarded is false after restore", !widget.isLyricsDiscarded());
                check("regression 7: hasLyrics is true after restore", widget.hasLyrics());
                check("regression 7: text is restored", !widget.text().isEmpty());

                // Toggle discard
                widget.toggleDiscardCurrentLyrics();
                check("regression 7: toggleDiscardCurrentLyrics sets discarded", widget.isLyricsDiscarded());
                check("regression 7: hasLyrics is false after toggle", !widget.hasLyrics());

                // Track change resets discarded state
                QVariantMap nextMeta;
                nextMeta.insert(QStringLiteral("xesam:title"), QStringLiteral("Next Song"));
                nextMeta.insert(QStringLiteral("xesam:artist"), QStringList{QStringLiteral("Artist")});
                widget.applyMetadata(nextMeta);
                check("regression 7: track change resets isLyricsDiscarded", !widget.isLyricsDiscarded());

                // showNoteDuringPauses check
                check("regression 7: showNoteDuringPauses default is false", !widget.showNoteDuringPauses());
            }
        }

        widget.m_watchedService.clear();
        return failures ? 1 : 0;
    }
};
int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("PATH", runtime.path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", runtime.path().toUtf8());
    qputenv("XDG_CACHE_HOME", runtime.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-plasma-lrc-test-bus");
    qunsetenv("SPICY_LYRICS_SECRET_KEY");
    QApplication app(argc, argv);
    return LrcAppletSyncTest::run();
}
