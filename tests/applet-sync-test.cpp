// SPDX-License-Identifier: MIT
#include "lrcapplet.h"
#include <QApplication>
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
