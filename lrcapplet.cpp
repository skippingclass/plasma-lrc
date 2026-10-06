/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcapplet.h"

#include "lrcparser.h"
#include "playermeta.h"
#include "trackname.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <QAction>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <algorithm>

namespace
{
// lrc_tty may hit the network on a track it has never seen before, so give it
// a generous amount of time before deciding that it is stuck.
constexpr int kWatchdogTimeoutMs = 20000;
// Do not spam the shell when the binary is missing: retry now and then.
constexpr int kMissingBinaryRetryMs = 5000;
// How many "no lyrics" answers in a row make us try the next MPRIS player.
constexpr int kNoLyricsBeforeSwitchingPlayer = 3;
// Enumerating bus names is a round trip to the bus, so don't do it on every
// tick: this is also how quickly we notice a player that just started.
constexpr int kPlayerListCacheMs = 5000;
// Probing every player should not block the widget if one of them hangs.
constexpr int kProbeTimeoutMs = 2000;
// How often the bus is asked again who is playing, even when a player is found.
constexpr int kPlayerRerankMs = 30000;
// How much better another player has to look before playback is switched over.
constexpr int kPlayerSwitchMargin = 5;
// Generous upper bound for a local MPRIS property fetch.
constexpr int kDBusCallTimeoutMs = 5000;
// How long the last line stays after the song is over before the panel goes
// empty. A pause between lines can be a second and a half, so this has to be
// clearly longer than that.
constexpr int kTailGraceMs = 2500;

// All entries of contents/config/main.xml live in this group, and that is also
// where KConfigLoader writes them (the applet's own group is only prepended to
// grouped entries), so the settings have to be read back from here.
const QString s_configGroup = QStringLiteral("General");
// Matches the default of pollInterval in contents/config/main.xml.
constexpr int kDefaultPollInterval = 200;
// How often the playback position may be asked for again while it stays unknown.
constexpr qint64 kPositionRetryIntervalMs = 2000;
// How often a known position is checked against the player, which is the only
// way to hear about a seek: MPRIS has no signal for one. Nothing else in the
// widget costs as much as a missed seek looking broken, so this is short: a
// property read on the session bus is a fraction of a millisecond.
constexpr qint64 kPositionCheckMs = 400;
// A difference below this is the clock running slightly off, a bigger one means
// playback was moved.
constexpr qint64 kPositionSnapToleranceMs = 350;
// How far the position may fall from a word and still light it up: on a fast
// line the gaps between words are shorter than a tick.
constexpr qint64 kWordSnapToleranceMs = 150;
// How often the word highlight is recomputed. Fast lines have words well under
// 100ms, so a coarse tick skips them.
constexpr int kWordTickMs = 20;
// While the music is stopped the highlight does not move, but a seek still does,
// so the position has to keep being watched, only less often.
constexpr int kIdleTickMs = 250;

const QString s_mprisPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString s_playerPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString s_playerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString s_propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

// playerctld proxies whatever is currently playing, so it is by far the best
// candidate; the rest is just a rough order of likelihood.
const QStringList s_preferredPlayers = {
    QStringLiteral("playerctld"),
    QStringLiteral("spotify"),
    QStringLiteral("firefox"),
    QStringLiteral("chromium"),
    QStringLiteral("vlc"),
    QStringLiteral("mpv"),
    QStringLiteral("celluloid"),
    QStringLiteral("strawberry"),
    QStringLiteral("audacious"),
    QStringLiteral("elisa"),
    QStringLiteral("quodlibet"),
};

// Everything lrc_tty prints when there is simply nothing to show.
bool isNoLyricsMarker(const QString &line)
{
    const QString normalized = line.simplified().toLower();
    return normalized.isEmpty() || normalized == QLatin1String("(no lyrics)") || normalized == QLatin1String("no lyrics")
        || normalized == QLatin1String("(none)");
}

// Qt hands us nested dictionaries as a QDBusArgument, but be lenient and accept
// an already converted QVariantMap as well.
QVariantMap toVariantMap(const QVariant &variant)
{
    if (variant.metaType().id() == QMetaType::QVariantMap) {
        return variant.toMap();
    }

    QVariantMap map;
    const QDBusArgument argument = variant.value<QDBusArgument>();
    argument >> map;
    return map;
}

QStringList toStringList(const QVariant &variant)
{
    switch (variant.metaType().id()) {
    case QMetaType::QStringList:
        return variant.toStringList();
    case QMetaType::QVariantList: {
        QStringList result;
        const QVariantList list = variant.toList();
        result.reserve(list.size());
        for (const QVariant &item : list) {
            result.append(item.toString());
        }
        return result;
    }
    default:
        break;
    }

    if (!variant.isValid()) {
        return {};
    }
    return {variant.toString()};
}

void sortByPreference(QStringList &players)
{
    std::sort(players.begin(), players.end(), [](const QString &lhs, const QString &rhs) {
        const int lhsRank = s_preferredPlayers.indexOf(lhs);
        const int rhsRank = s_preferredPlayers.indexOf(rhs);
        const int lhsKey = lhsRank == -1 ? s_preferredPlayers.size() : lhsRank;
        const int rhsKey = rhsRank == -1 ? s_preferredPlayers.size() : rhsRank;
        if (lhsKey != rhsKey) {
            return lhsKey < rhsKey;
        }
        return lhs < rhs;
    });
}

QStringList mprisPlayersFromServiceNames(const QStringList &services)
{
    QStringList players;
    for (const QString &service : services) {
        if (!service.startsWith(s_mprisPrefix)) {
            continue;
        }
        const QString player = service.mid(s_mprisPrefix.size());
        // The instance part of chromium.instance19370 style names carries no
        // information for us, but it does have to be passed to lrc_tty as is.
        if (!player.isEmpty() && !players.contains(player)) {
            players.append(player);
        }
    }

    sortByPreference(players);
    return players;
}

}

LrcApplet::LrcApplet(QObject *parent, const KPluginMetaData &data, const QVariantList &args)
    : Plasma::Applet(parent, data, args)
    , m_process(new QProcess(this))
    , m_spicy(new SpicyLyrics(this))
    , m_propertyWatcher(nullptr)
    , m_namesWatcher(nullptr)
    , m_positionWatcher(nullptr)
    , m_pollInterval(kDefaultPollInterval)
    , m_maxCharacters(40)
    , m_showTimestamp(false)
    , m_showIcon(true)
    , m_showTrackInfo(true)
    , m_compactPanel(false)
    , m_wordStyle(0)
    , m_lyricOffset(0)
    , m_pauseWhenIdle(true)
    , m_useSpicy(true)
    , m_trackLengthUs(0)
    , m_webPageSession(false)
    , m_useCleanedLookup(true)
    , m_lastLookupWasCleaned(false)
    , m_probed(false)
    , m_awaitingState(false)
    , m_candidateIndex(0)
    , m_lastGoodCandidate(-1)
    , m_noLyricsCount(0)
    , m_active(false)
    , m_available(true)
    , m_playing(false)
    , m_playingKnown(false)
    , m_started(false)
    , m_wordStart(0)
    , m_wordEnd(0)
    , m_wordProgress(0.0)
    , m_wordSynced(false)
    , m_fromSpicy(false)
    , m_fetchingFromSpicy(false)
    , m_fetchingLrc(false)
    , m_missingReason(MissingReason::Unsynced)
    , m_positionMs(0)
    , m_positionBaseMs(0)
    , m_positionValid(false)
    , m_lineIndex(-1)
    , m_wordCursor(0)
{
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LrcApplet::poll);

    m_wordTimer.setInterval(kWordTickMs);
    connect(&m_wordTimer, &QTimer::timeout, this, &LrcApplet::updateWord);

    // Runs whether or not anything is playing: this is how a player that comes
    // back after being closed gets noticed.
    m_busTimer.setInterval(kPlayerRerankMs);
    connect(&m_busTimer, &QTimer::timeout, this, &LrcApplet::considerPlayerSwitch);

    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
        }
    });

    connect(m_process, &QProcess::finished, this, [this] {
        onProcessFinished();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            onProcessFailedToStart();
        }
    });
    connect(m_process, &QProcess::stateChanged, this, [this] {
        if (m_process->state() == QProcess::NotRunning) {
            m_watchdog.stop();
        }
    });

    // Watch the bus itself rather than every player service: this is the only
    // notification we get when a player exits.
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.DBus"),
                                          QStringLiteral("/org/freedesktop/DBus"),
                                          QStringLiteral("org.freedesktop.DBus"),
                                          QStringLiteral("NameOwnerChanged"),
                                          this,
                                          SLOT(onServiceOwnerChanged(QString,QString,QString)));

    connect(m_spicy, &SpicyLyrics::loaded, this, &LrcApplet::onSpicyLoaded);
    connect(m_spicy, &SpicyLyrics::missing, this, &LrcApplet::onSpicyMissing);
}

LrcApplet::~LrcApplet()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void LrcApplet::constraintsEvent(Constraints constraints)
{
    // Applet::init() is not called by the framework, and config() is not
    // available from the constructor, so start once the UI is up and running.
    if (constraints & UiReadyConstraint) {
        startPolling();

        QAction *refreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Update lyrics now"), this);
        connect(refreshAction, &QAction::triggered, this, &LrcApplet::refresh);
        setInternalAction(QStringLiteral("refresh"), refreshAction);
    }
}

void LrcApplet::configChanged()
{
    // startPolling() re-reads the settings and polls right away.
    startPolling();
}

void LrcApplet::startPolling()
{
    readSettings();

    if (!m_started) {
        m_started = true;
        // Ask the bus which players are around before doing anything else.
        refreshPlayerCandidates();
    }

    m_busTimer.start();
    m_timer.start(m_pollInterval);
    poll();
}

void LrcApplet::refresh()
{
    // The API lyrics have to go as well: poll() returns early while they are
    // usable, so the button would do nothing at all on a track that has them.
    clearDisplay();
    invalidatePlayerCandidates();
    unwatchPlayer();
    setPlaying(false, false);

    if (m_started) {
        // Also clears the "binary is missing" backoff, in case lrc_tty has been
        // installed since the last attempt.
        m_timer.start(m_pollInterval);
        poll();
    }
}

QString LrcApplet::setting(const QString &key, const QString &defaultValue) const
{
    return config().group(s_configGroup).readEntry(key, defaultValue);
}

int LrcApplet::intSetting(const QString &key, int defaultValue) const
{
    int value = config().group(s_configGroup).readEntry(key, defaultValue);
    if (value <= 0) {
        value = defaultValue;
    }
    return value;
}

bool LrcApplet::boolSetting(const QString &key, bool defaultValue) const
{
    return config().group(s_configGroup).readEntry(key, defaultValue);
}

QString LrcApplet::spicyKeySetting() const
{
    // The API key may come from the widget config or from the environment; the
    // latter keeps it out of the Plasma config file.
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString fromEnvironment = environment.value(QStringLiteral("SPICY_LYRICS_SECRET_KEY")).trimmed();
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    return setting(QStringLiteral("spicyKey")).trimmed();
}

void LrcApplet::readSettings()
{
    // An empty setting must never end up as an empty program name, otherwise
    // QProcess fails to start and the widget claims lrc_tty is missing.
    const QString configuredPath = setting(QStringLiteral("binaryPath"), QStringLiteral("lrc_tty")).trimmed();
    // A path that no longer exists (lrc_tty moved, uninstalled and reinstalled
    // elsewhere, ...) is not fatal: fall back to whatever is in $PATH.
    const QString binaryPath = configuredPath.isEmpty() //
        || (configuredPath.contains(QLatin1Char('/')) && !QFileInfo::exists(configuredPath))
        ? QStringLiteral("lrc_tty")
        : configuredPath;
    const QString player = setting(QStringLiteral("player")).trimmed();
    const QString preferredPlayer = setting(QStringLiteral("preferredPlayer")).trimmed();
    const QString placeholder = setting(QStringLiteral("placeholderText"), QStringLiteral("♪"));
    const int pollInterval = qBound(200, intSetting(QStringLiteral("pollInterval"), kDefaultPollInterval), 10000);
    const int maxCharacters = qBound(0, intSetting(QStringLiteral("maxCharacters"), 40), 500);
    const bool showTimestamp = boolSetting(QStringLiteral("showTimestamp"), false);
    const bool showIcon = boolSetting(QStringLiteral("showIcon"), true);
    const bool showTrackInfo = boolSetting(QStringLiteral("showTrackInfo"), true);
    const bool compactPanel = boolSetting(QStringLiteral("compactPanel"), false);
    const int wordStyle = qBound(0, intSetting(QStringLiteral("wordStyle"), 0), 1);
    const int lyricOffset = qBound(-2000, intSetting(QStringLiteral("lyricOffset"), 0), 2000);
    const bool pauseWhenIdle = boolSetting(QStringLiteral("pauseWhenIdle"), false);
    const bool useSpicy = boolSetting(QStringLiteral("useSpicyLyrics"), true);
    const QString spicyKey = spicyKeySetting();

    const bool changed = binaryPath != m_binaryPath //
        || player != m_configuredPlayer //
        || preferredPlayer != m_preferredPlayer //
        || placeholder != m_placeholderText //
        || pollInterval != m_pollInterval //
        || maxCharacters != m_maxCharacters //
        || showTimestamp != m_showTimestamp //
        || showIcon != m_showIcon //
        || showTrackInfo != m_showTrackInfo //
        || compactPanel != m_compactPanel //
        || wordStyle != m_wordStyle //
        || lyricOffset != m_lyricOffset //
        || pauseWhenIdle != m_pauseWhenIdle //
        || useSpicy != m_useSpicy //
        || spicyKey != m_spicyKey;

    m_binaryPath = binaryPath;
    m_configuredPlayer = player;
    m_preferredPlayer = preferredPlayer;
    m_placeholderText = placeholder;
    m_pollInterval = pollInterval;
    m_maxCharacters = maxCharacters;
    m_showTimestamp = showTimestamp;
    m_showIcon = showIcon;
    m_showTrackInfo = showTrackInfo;
    m_compactPanel = compactPanel;
    m_wordStyle = wordStyle;
    m_lyricOffset = lyricOffset;
    m_pauseWhenIdle = pauseWhenIdle;
    m_useSpicy = useSpicy;
    m_spicyKey = spicyKey;

    m_spicy->setKey(m_spicyKey);
    m_spicy->setEnabled(m_useSpicy);

    if (changed) {
        Q_EMIT settingsChanged();
        invalidatePlayerCandidates();
    }

    // A new key is worth another try, even for the track that is playing now.
    if (changed && m_useSpicy && !m_spicyKey.isEmpty() && !m_spotifyTrackId.isEmpty() && !m_fromSpicy) {
        requestLyrics();
    }

    if (m_timer.interval() != m_pollInterval && m_timer.isActive()) {
        m_timer.start(m_pollInterval);
    }
}

QString LrcApplet::placeholderText() const
{
    return m_placeholderText;
}

QString LrcApplet::binaryPath() const
{
    return m_binaryPath;
}

int LrcApplet::maxCharacters() const
{
    return m_maxCharacters;
}

bool LrcApplet::showIcon() const
{
    return m_showIcon;
}

bool LrcApplet::showTrackInfo() const
{
    return m_showTrackInfo;
}

bool LrcApplet::compactPanel() const
{
    return m_compactPanel;
}

int LrcApplet::wordStyle() const
{
    return m_wordStyle;
}

int LrcApplet::lyricOffset() const
{
    return m_lyricOffset;
}

QString LrcApplet::attribution() const
{
    return m_lyrics.attribution;
}

QString LrcApplet::attributionUrl() const
{
    return m_lyrics.attributionUrl;
}

void LrcApplet::poll()
{
    if (!m_started) {
        return;
    }

    // If lyrics are already loaded and usable, the local monotonic timer drives the display
    if (m_lyrics.isUsable()) {
        return;
    }

    considerPlayerSwitch();

    const QStringList candidates = playerCandidates();
    if (candidates.isEmpty() || !m_probed || m_awaitingState) {
        return;
    }

    const QString player = candidates.at(qBound(0, m_candidateIndex, int(candidates.size()) - 1));
    if (!m_configuredPlayer.isEmpty() && !m_busPlayers.contains(player)) {
        return;
    }
    if (player != m_player) {
        setPlayer(player);
        unwatchPlayer();
        watchPlayer();
        return;
    }

    if (m_fetchingFromSpicy || m_fetchingLrc || m_process->state() != QProcess::NotRunning) {
        return;
    }

    if (!m_trackTitle.isEmpty() && !m_lyrics.isUsable()) {
        requestLyrics();
    }
}

/**
 * Keeps an eye on who is on the bus, whether or not anything is playing.
 *
 * This cannot live in poll(): that returns early while nothing plays, and a
 * player that starts afterwards would then never be noticed at all. Coming back
 * from a closed player is exactly that case, which is how the widget could end up
 * blind to a player that had come back.
 */
void LrcApplet::considerPlayerSwitch()
{
    if (!m_started) {
        return;
    }

    if (m_candidates.isEmpty()) {
        // We have not heard from the bus yet (or there is no player at all).
        if (!m_candidatesTimer.isValid() || m_candidatesTimer.elapsed() > kPlayerListCacheMs) {
            refreshPlayerCandidates();
        }
        return;
    }

    if (!m_candidatesTimer.isValid() || m_candidatesTimer.elapsed() > kPlayerRerankMs) {
        refreshPlayerCandidates();
    }
}

void LrcApplet::onProcessFinished()
{
    m_watchdog.stop();
    m_fetchingLrc = false;

    if (!m_available) {
        setAvailable(true);
        setError(QString());
        if (m_timer.interval() != m_pollInterval) {
            m_timer.start(m_pollInterval);
        }
    }

    // A track that the API timed better than lrc_tty does: ignore this answer.
    if (m_fromSpicy && m_lyrics.isUsable()) {
        return;
    }

    const QString output = QString::fromUtf8(m_process->readAllStandardOutput());
    const int exitCode = m_process->exitCode();
    const QProcess::ExitStatus exitStatus = m_process->exitStatus();

    Lyrics parsed;
    const qint64 durationMs = m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0;
    const bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0 && LrcParser::parseLrc(output, &parsed, durationMs));

    if (ok && parsed.isUsable()) {
        m_lyrics = parsed;
        m_fromSpicy = false;
        m_wordSynced = false;
        m_lineIndex = -1;
        m_noLyricsCount = 0;
        m_lastGoodCandidate = m_candidateIndex;
        m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
        updateWord();
        Q_EMIT lyricsChanged();
        return;
    }

    // The cleaned query found nothing. Before giving up on the track, try the
    // title as the player reported it: the cleaning is a guess about a video
    // title, and a song whose own name contains a dash ("Love - Hate") is
    // exactly what it gets wrong.
    if (m_useCleanedLookup && m_lastLookupWasCleaned) {
        m_useCleanedLookup = false;
        fetchLrcLyrics(true);
        return;
    }

    setText(QString());
    setActive(false);
}

void LrcApplet::onProcessFailedToStart()
{
    m_watchdog.stop();
    // QProcess knows better than we do what went wrong ("No such file or
    // directory", permission denied, ...), so show that instead of guessing.
    setError(m_process->errorString());
    if (!m_available) {
        return;
    }
    setAvailable(false);
    setText(QString());
    setActive(false);
    // lrc_tty may simply not be installed (yet), so keep an eye on it without
    // spawning a process on every tick.
    m_timer.start(kMissingBinaryRetryMs);
}

void LrcApplet::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    Q_EMIT textChanged();
}

void LrcApplet::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

void LrcApplet::setAvailable(bool available)
{
    if (m_available == available) {
        return;
    }
    m_available = available;
    Q_EMIT availableChanged();
}

void LrcApplet::setError(const QString &error)
{
    if (m_error == error) {
        return;
    }
    m_error = error;
    Q_EMIT availableChanged();
}

void LrcApplet::setPlaying(bool playing, bool known)
{
    if (m_playing == playing && m_playingKnown == known) {
        return;
    }
    m_playing = playing;
    m_playingKnown = known;
    Q_EMIT playingChanged();

    if (playing) {
        // Playback started (or was resumed): go pick up where we left off.
        poll();
    }

    // The word timer exists to move the highlight. While the music is stopped
    // nothing moves, but a seek can still happen and MPRIS says nothing about
    // one, so the timer keeps running at a lazy pace instead of standing still.
    if (known && !playing) {
        m_wordTimer.start(kIdleTickMs);
    } else if (playing && m_lyrics.isUsable()) {
        m_wordTimer.start(kWordTickMs);
    }
}

void LrcApplet::setTrackInfo(const QString &trackInfo)
{
    if (m_trackInfo == trackInfo) {
        return;
    }
    m_trackInfo = trackInfo;
    Q_EMIT trackInfoChanged();
}

void LrcApplet::clearLyrics()
{
    m_lyrics = Lyrics();
    m_word.clear();
    m_wordStart = 0;
    m_wordEnd = 0;
    m_wordProgress = 0.0;
    m_wordSynced = false;
    m_fromSpicy = false;
    m_fetchingFromSpicy = false;
    m_fetchingLrc = false;
    m_missingReason = MissingReason::Unsynced;
    m_positionMs = 0;
    m_positionBaseMs = 0;
    m_positionValid = false;
    m_positionClock.invalidate();
    m_positionRetries.invalidate();
    m_lineIndex = -1;
    m_wordCursor = 0;
    m_wordTimer.stop();
    Q_EMIT wordChanged();
    Q_EMIT lyricsChanged();
}

/**
 * Takes everything off the panel that belonged to what was playing.
 *
 * Both sources go through here, so a new track, a stopped player and the end of a
 * song all end the same way: nothing of the previous one is left hanging on the
 * panel.
 */
void LrcApplet::clearDisplay()
{
    if (m_positionWatcher) {
        delete m_positionWatcher;
        m_positionWatcher = nullptr;
    }

    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(50);
    }
    m_watchdog.stop();

    clearLyrics();
    m_spotifyTrackId.clear();
    m_lineIndex = -1;
    setText(QString());
    setActive(false);
    setTrackInfo(QString());
    m_trackArtist.clear();
    m_trackTitle.clear();
    m_trackLengthUs = 0;
    m_webPageSession = false;
    m_useCleanedLookup = true;
    m_lastLookupWasCleaned = false;
    m_fetchingFromSpicy = false;
    m_fetchingLrc = false;
    m_positionValid = false;
    m_positionBaseMs = 0;
    m_positionClock.invalidate();
    m_positionRetries.invalidate();
    m_positionChecks.invalidate();
}

void LrcApplet::applyMetadata(const QVariantMap &metadata)
{
    const QStringList artists = trackArtists(metadata.value(QStringLiteral("xesam:artist")));
    const QString title = metadata.value(QStringLiteral("xesam:title")).toString();
    const QString artist = artists.join(QStringLiteral(", "));
    const QString trackId = SpicyLyrics::trackIdFromMpris(mprisTrackId(metadata.value(QStringLiteral("mpris:trackid"))));
    const qint64 lengthUs = metadata.value(QStringLiteral("mpris:length")).toLongLong();

    QString trackInfo;
    if (!artists.isEmpty() && !title.isEmpty()) {
        trackInfo = artists.join(QStringLiteral(", ")) + QStringLiteral(" — ") + title;
    } else if (!title.isEmpty()) {
        trackInfo = title;
    } else {
        trackInfo = artists.join(QStringLiteral(", "));
    }

    if (trackInfo.isEmpty()) {
        clearDisplay();
        return;
    }

    const bool trackChanged = (title != m_trackTitle || artist != m_trackArtist || (!trackId.isEmpty() && trackId != m_spotifyTrackId));

    if (trackChanged) {
        clearDisplay();

        setTrackInfo(trackInfo);
        m_webPageSession = looksLikeWebPageSession(mprisTrackId(metadata.value(QStringLiteral("mpris:trackid"))));
        m_trackArtist = artist;
        m_trackTitle = title;
        m_trackLengthUs = lengthUs;
        m_spotifyTrackId = trackId;

        requestPosition();
        requestLyrics();
    } else {
        setTrackInfo(trackInfo);
        m_trackLengthUs = lengthUs;
        if (!m_lyrics.isUsable() && !m_fetchingFromSpicy && !m_fetchingLrc) {
            requestLyrics();
        }
    }
}

void LrcApplet::requestLyrics()
{
    if (m_watchedService.isEmpty() || m_trackTitle.isEmpty()) {
        return;
    }

    // 1. If Spotify and Spicy is enabled with a key, try Spicy Lyrics first
    if (m_useSpicy && !m_spotifyTrackId.isEmpty() && !m_spicyKey.isEmpty()) {
        m_fetchingFromSpicy = true;
        m_fetchingLrc = false;
        m_spicy->request(m_spotifyTrackId);
        return;
    }

    // 2. Otherwise, fetch LRC via lrc_tty --dump
    m_fetchingFromSpicy = false;
    fetchLrcLyrics();
}

void LrcApplet::requestSpicyLyrics()
{
    requestLyrics();
}

void LrcApplet::fetchLrcLyrics(bool rawFallback)
{
    if (m_watchedService.isEmpty() || m_player.isEmpty() || m_trackTitle.isEmpty()) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(50);
    }

    QStringList arguments;
    arguments << QStringLiteral("--dump");
    arguments << QStringLiteral("--player") << m_player;

    const bool trackBelongsToPlayer = (m_watchedService == s_mprisPrefix + m_player);

    if (!rawFallback && m_useCleanedLookup && trackBelongsToPlayer) {
        QString artist = m_trackArtist;
        QString title = m_trackTitle;
        splitTrackArtistAndTitle(&artist, &title, m_webPageSession);
        const QString cleanedTitle = cleanTrackTitle(title);
        const QString cleanedArtist = cleanTrackArtist(artist);
        if (!cleanedTitle.isEmpty() && (cleanedTitle != m_trackTitle || cleanedArtist != m_trackArtist)) {
            arguments << QStringLiteral("--artist") << cleanedArtist;
            arguments << QStringLiteral("--title") << cleanedTitle;
            m_lastLookupWasCleaned = true;
        } else {
            m_lastLookupWasCleaned = false;
        }
    } else {
        m_lastLookupWasCleaned = false;
    }

    if (trackBelongsToPlayer && m_trackLengthUs > 0) {
        arguments << QStringLiteral("--duration") << QString::number(m_trackLengthUs / 1000000);
    }

    m_fetchingLrc = true;
    m_watchdog.start(kWatchdogTimeoutMs);
    m_process->start(m_binaryPath, arguments);
}

void LrcApplet::onSpicyLoaded(const QString &trackId, const Lyrics &lyrics)
{
    if (trackId != m_spotifyTrackId) {
        return;
    }

    m_fetchingFromSpicy = false;
    m_lyrics = lyrics;
    m_fromSpicy = true;
    m_wordSynced = lyrics.type == LyricsType::Syllable;

    requestPosition();
    m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
    updateWord();
    Q_EMIT lyricsChanged();
}

void LrcApplet::onSpicyMissing(const QString &trackId, MissingReason reason)
{
    if (trackId != m_spotifyTrackId) {
        return;
    }
    m_fetchingFromSpicy = false;
    m_missingReason = reason;

    // Fall back to full LRC via lrc_tty --dump
    fetchLrcLyrics();
}

void LrcApplet::requestPosition()
{
    if (m_watchedService.isEmpty()) {
        return;
    }

    // One at a time: updateWord() runs 25 times a second and asks while it has
    // no position, and a player that never answers must not turn that into a
    // D-Bus flood.
    if (m_positionWatcher) {
        return;
    }

    // The retry delay is for a position that is not known yet. Once it is known,
    // how often the player is asked is the caller's decision: the same throttle
    // used to hold the periodic check back to two seconds, and a seek went
    // unnoticed for all of it.
    if (!m_positionValid && m_positionRetries.isValid() && m_positionRetries.elapsed() < kPositionRetryIntervalMs) {
        return;
    }
    m_positionRetries.start();

    QDBusMessage message = QDBusMessage::createMethodCall(m_watchedService, s_playerPath, s_propertiesInterface, QStringLiteral("Get"));
    message << s_playerInterface << QStringLiteral("Position");

    m_positionWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, kDBusCallTimeoutMs), this);
    connect(m_positionWatcher, &QDBusPendingCallWatcher::finished, this, &LrcApplet::onPositionFetched);
}

void LrcApplet::onPositionFetched()
{
    QDBusPendingCallWatcher *watcher = m_positionWatcher;
    if (!watcher) {
        return;
    }

    // Properties.Get answers with a variant; QDBusPendingReply<qint64> refuses it
    // ("got v, expected x"), so the variant is unwrapped by hand.
    const QDBusPendingReply<QVariant> reply = *watcher;
    m_positionWatcher = nullptr;
    watcher->deleteLater();

    if (reply.isError() || !reply.value().isValid()) {
        return;
    }

    const qint64 positionUs = reply.value().toLongLong();
    if (positionUs < 0) {
        return;
    }

    const qint64 reportedMs = positionUs / 1000;
    const bool jumped = positionJumped(reportedMs);

    m_positionBaseMs = reportedMs;
    m_positionValid = true;
    m_positionClock.start();
    m_positionRetries.start();

    if (jumped && m_lyrics.isUsable() && m_lyrics.lineAt(reportedMs) < 0) {
        // Playback was moved into a gap between lines. The line on screen belongs
        // to a different part of the song now, so it goes away instead of
        // pretending somebody is still singing it.
        m_lineIndex = -1;
        setText(QString());
        setActive(false);
    } else if (m_lyrics.isUsable()) {
        updateWord();
    }
}

/// True when the answer differs from where we thought playback was by more than a
/// seek is expected to move it.
bool LrcApplet::positionJumped(qint64 reportedMs) const
{
    // Nothing to compare against yet: the first reading of a track is not a jump.
    if (!m_positionValid || !m_positionClock.isValid()) {
        return false;
    }

    const qint64 expected = m_positionBaseMs + (m_playing ? m_positionClock.elapsed() : 0);
    return qAbs(reportedMs - expected) > kPositionSnapToleranceMs;
}

void LrcApplet::clearWordHighlight()
{
    if (m_word.isEmpty() && m_wordProgress == 0.0) {
        return;
    }
    m_word.clear();
    m_wordStart = 0;
    m_wordEnd = 0;
    m_wordProgress = 0.0;
    Q_EMIT wordChanged();
}

void LrcApplet::updateWord()
{
    if (!m_lyrics.isUsable()) {
        return;
    }

    if (!m_positionValid) {
        requestPosition();
        return;
    }

    // MPRIS has no signal for either position or seeking, so the position is
    // advanced with the monotonic clock and checked against the player now and
    // then. Without the check a seek is not noticed until the track changes,
    // which is what used to happen.
    if (!m_positionChecks.isValid() || m_positionChecks.elapsed() > kPositionCheckMs) {
        m_positionChecks.start();
        requestPosition();
    }

    // Some syncs simply run early or late, and no amount of clever reading will
    // fix that; a setting does.
    m_positionMs = m_positionBaseMs + (m_playing ? m_positionClock.elapsed() : 0) + m_lyricOffset;

    const int lineIndex = m_lyrics.lineAt(m_positionMs);
    if (lineIndex < 0) {
        // Between lines nobody is singing, and a pause there is normal, so what
        // is shown stays. Past the end of the last line there is nothing to keep:
        // what stays would be the outro of a song that is over, and it looks like
        // the panel has lost track of what is playing.
        if (!m_lyrics.lines.isEmpty() && m_positionMs > m_lyrics.lines.last().endMs + kTailGraceMs) {
            setText(QString());
            setActive(false);
            clearWordHighlight();
        }
        return;
    }

    const LyricLine &line = m_lyrics.lines.at(lineIndex);
    if (line.text != m_text) {
        setText(line.text);
        setActive(true);
    }

    // A new line starts without its first word, so a word from the line before
    // must not stay highlighted for a tick.
    if (lineIndex != m_lineIndex) {
        m_lineIndex = lineIndex;
        m_wordCursor = 0;
        clearWordHighlight();
    }

    if (!m_wordSynced || line.words.isEmpty()) {
        clearWordHighlight();
        return;
    }

    // The timings are per piece, but the highlight covers the whole word: a word
    // that was sung in two pieces would otherwise blink between its halves.
    QString currentWord;
    int currentStart = 0;
    int currentEnd = 0;
    double progress = 0.0;

    const LyricWord *active = nullptr;
    const int wordCount = static_cast<int>(line.words.size());
    // Playback moves forward, so the walk continues from last time; a seek that
    // goes back simply starts over.
    const int from = m_positionMs >= line.words.value(qBound(0, m_wordCursor, wordCount - 1)).startMs ? qBound(0, m_wordCursor, wordCount - 1) : 0;

    for (int i = from; i < wordCount; ++i) {
        const LyricWord &word = line.words.at(i);
        if (m_positionMs >= word.startMs && (m_positionMs < word.endMs || word.endMs <= word.startMs)) {
            active = &word;
            m_wordCursor = i;
            break;
        }
    }

    if (!active) {
        // The position fell between two words. On a fast line those gaps are
        // shorter than the tick and the word in between would never light up, so
        // the closest one within reach is used instead of dropping the highlight.
        qint64 closest = kWordSnapToleranceMs + 1;
        for (int i = from; i < wordCount; ++i) {
            const LyricWord &word = line.words.at(i);
            const qint64 distance = m_positionMs < word.startMs ? word.startMs - m_positionMs
                                                               : (m_positionMs > word.endMs ? m_positionMs - word.endMs : 0);
            if (distance < closest) {
                closest = distance;
                active = &word;
            }
        }
        if (closest > kWordSnapToleranceMs) {
            active = nullptr;
        } else {
            m_wordCursor = static_cast<int>(active - line.words.constData());
        }
    }

    if (active) {
        currentWord = line.text.mid(active->groupStart, active->groupEnd - active->groupStart);
        currentStart = active->groupStart;
        currentEnd = active->groupEnd;
        const qint64 span = active->endMs - active->startMs;
        progress = span > 0 ? static_cast<double>(m_positionMs - active->startMs) / static_cast<double>(span) : 0.0;
        progress = qBound(0.0, progress, 1.0);
    }

    // Only a change of the word itself is worth a signal. Progress moves on every
    // tick, and emitting that had the panel rebuilding its rich text fifty times a
    // second for something nobody draws.
    if (currentWord != m_word || currentStart != m_wordStart || currentEnd != m_wordEnd) {
        m_word = currentWord;
        m_wordStart = currentStart;
        m_wordEnd = currentEnd;
        m_wordProgress = progress;
        Q_EMIT wordChanged();
    } else {
        m_wordProgress = progress;
    }
}

void LrcApplet::setPlayer(const QString &player)
{
    if (m_player == player) {
        return;
    }
    m_player = player;
    Q_EMIT playerChanged();
}

void LrcApplet::invalidatePlayerCandidates()
{
    m_candidates.clear();
    m_busPlayers.clear();
    m_probed = false;
    m_candidatesTimer.invalidate();
    m_candidateIndex = 0;
    m_lastGoodCandidate = -1;
    m_noLyricsCount = 0;

    delete m_namesWatcher;
    m_namesWatcher = nullptr;
}

QStringList LrcApplet::ignoredPlayers() const
{
    QStringList ignored;
    const QString raw = setting(QStringLiteral("ignoredPlayers"));
    for (const QString &name : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty()) {
            ignored.append(trimmed);
        }
    }
    return ignored;
}

QStringList LrcApplet::playerCandidates()
{
    return m_candidates;
}

/**
 * The players a name written in the settings stands for.
 *
 * With nothing configured that is everybody on the bus. With a lock it is whoever
 * matches, and the name as written when nobody matches yet — so a locked widget
 * waits for its player instead of quietly following another one.
 */
QStringList LrcApplet::matchingPlayers(const QStringList &players) const
{
    if (m_configuredPlayer.isEmpty()) {
        return players;
    }

    QStringList matched;
    for (const QString &player : players) {
        if (playerNameMatches(m_configuredPlayer, player)) {
            matched.append(player);
        }
    }
    return matched.isEmpty() ? QStringList{m_configuredPlayer} : matched;
}

void LrcApplet::refreshPlayerCandidates()
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("ListNames"));

    delete m_namesWatcher;
    m_namesWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, kDBusCallTimeoutMs), this);
    connect(m_namesWatcher, &QDBusPendingCallWatcher::finished, this, &LrcApplet::onPlayerListFetched);
}

void LrcApplet::onPlayerListFetched()
{
    QDBusPendingCallWatcher *watcher = m_namesWatcher;
    if (!watcher) {
        return;
    }

    const QDBusPendingReply<QStringList> reply = *watcher;
    m_namesWatcher = nullptr;
    watcher->deleteLater();

    if (reply.isError()) {
        // The bus is having a bad day; try again on the next tick.
        m_candidatesTimer.invalidate();
        return;
    }

    m_busPlayers = mprisPlayersFromServiceNames(reply.value());
    m_candidates = matchingPlayers(m_busPlayers);
    m_candidatesTimer.start();

    // Ask everyone what they are playing before picking one: the name alone says
    // nothing about whether this is a music player.
    probeCandidates();

    // Nothing is being watched at the moment, so this may be a good moment to
    // start paying attention to a player.
    if (m_watchedService.isEmpty() && m_player.isEmpty()) {
        poll();
    }
}

void LrcApplet::probeCandidates()
{
    qDeleteAll(m_probeWatchers);
    m_probeWatchers.clear();

    // Whatever the user named is gone before anybody is asked, so that the
    // rotation cannot land on it either.
    // A locked widget whose player is not on the bus right now: the name stays in
    // the list so that it is used the moment it appears, but there is nobody to
    // ask about it.
    if (!m_candidates.isEmpty() && !m_busPlayers.contains(m_candidates.first()) && m_candidates.size() == 1
        && !m_configuredPlayer.isEmpty()) {
        return;
    }

    const QStringList ignored = ignoredPlayers();
    if (!ignored.isEmpty()) {
        m_candidates.removeIf([&ignored](const QString &player) {
            return ignored.contains(player, Qt::CaseInsensitive);
        });
    }
    if (m_candidates.isEmpty()) {
        return;
    }

    // Players that went away keep their score otherwise, and a name that comes
    // back later would be ranked by a reading from a session that is long gone.
    QStringList live = m_candidates;
    const QStringList gone = m_playerScores.keys();
    for (const QString &name : gone) {
        if (!live.removeOne(name)) {
            m_playerScores.remove(name);
        }
    }

    for (const QString &player : std::as_const(m_candidates)) {
        QDBusMessage message =
            QDBusMessage::createMethodCall(s_mprisPrefix + player, s_playerPath, s_propertiesInterface, QStringLiteral("GetAll"));
        message << s_playerInterface;

        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, kProbeTimeoutMs), this);
        m_probeWatchers.append(watcher);

        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, player] {
            const QDBusPendingReply<QVariantMap> reply = *watcher;
            m_probeWatchers.removeAll(watcher);
            watcher->deleteLater();

            if (reply.isError()) {
                // A player that does not answer is no help, and an old score for
                // it would be worse than none.
                m_playerScores.remove(player);
                m_playerPlaying.remove(player);
            } else {
                m_playerScores.insert(player, musicScore(reply.value()));
                // Whether it is playing right now decides the favourite player and
                // whether the one we watch may be left, so it is kept apart from
                // the score: a score of a paused player is still high.
                m_playerPlaying.insert(player,
                                       reply.value().value(QStringLiteral("PlaybackStatus")).toString()
                                           == QLatin1String("Playing"));
            }

            if (!m_probeWatchers.isEmpty()) {
                return;
            }
            sortCandidatesByScore();
        });
    }
}

void LrcApplet::sortCandidatesByScore()
{
    // Playing first, score second, and a stable sort so that the name-based
    // preference settles what is left. A paused player describes a track better
    // than a web page does and still loses: what is on the panel should be what is
    // playing, and a Spotify paused in the middle of a song is not it.
    const auto playing = [this](const QString &name) {
        // The watched player is asked directly and knows right now; the others know
        // what the last sweep read.
        return name == m_player ? (m_playingKnown && m_playing) : m_playerPlaying.value(name, false);
    };
    std::stable_sort(m_candidates.begin(), m_candidates.end(), [this, &playing](const QString &lhs, const QString &rhs) {
        if (playing(lhs) != playing(rhs)) {
            return playing(lhs);
        }
        return m_playerScores.value(lhs, 0) > m_playerScores.value(rhs, 0);
    });

    // The favourite player goes to the front, but only while it is actually
    // playing: an idle favourite must not leave the panel empty waiting for it.
    // Compared this way it also covers a name written down loosely, like
    // "chromium" for "chromium.instance18422".
    if (!m_preferredPlayer.isEmpty()) {
        for (int i = 1; i < m_candidates.size(); ++i) {
            if (m_playerPlaying.value(m_candidates.at(i), false) && playerNameMatches(m_preferredPlayer, m_candidates.at(i))) {
                m_candidates.move(i, 0);
                break;
            }
        }
    }

    // Where in the list to continue, and whether to jump to the top of it.
    //
    // A player that is playing is left alone unless the newcomer looks a lot more
    // like music, because switching mid-song cuts it off, which is worse than one
    // switch too many. A player that is not playing has nothing to lose, so the
    // best candidate takes over at once — and it has to, or a line left over from
    // a player that stopped holds the panel for good.
    //
    // That last case is why this used to be broken at login: Spotify scoring 12
    // could not pass a paused player scoring 8 by the five points the rule asked
    // for, so the widget kept whatever it had found first, and only a restart of
    // plasmashell cleared it.
    const int currentIndex = m_candidates.indexOf(m_player);
    const int currentScore = m_playerScores.value(m_player, 0);
    const int bestScore = m_playerScores.value(m_candidates.first(), 0);
    const bool currentIsPlaying = m_playingKnown && m_playing;

    bool takeBest = currentIndex < 0;
    if (currentIndex >= 0) {
        takeBest = currentIsPlaying ? bestScore > currentScore + kPlayerSwitchMargin : bestScore > 0 && m_candidates.first() != m_player;
        m_candidateIndex = takeBest ? 0 : currentIndex;
    } else {
        m_candidateIndex = 0;
    }

    m_lastGoodCandidate = -1;
    m_noLyricsCount = 0;
    m_probed = true;

    // Switching has to happen here rather than in poll(): that returns early
    // while nothing plays, which is exactly the state a player that is coming
    // back is found in.
    const QString wanted = m_candidates.value(m_candidateIndex);
    if (takeBest && !wanted.isEmpty() && wanted != m_player) {
        setPlayer(wanted);
        unwatchPlayer();
        watchPlayer();
        poll();
    }
}

void LrcApplet::unwatchPlayer()
{
    delete m_propertyWatcher;
    m_propertyWatcher = nullptr;

    delete m_positionWatcher;
    m_positionWatcher = nullptr;

    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(50);
    }
    m_watchdog.stop();

    if (m_watchedService.isEmpty()) {
        return;
    }

    m_awaitingState = false;

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.disconnect(m_watchedService,
                   s_playerPath,
                   s_propertiesInterface,
                   QStringLiteral("PropertiesChanged"),
                   this,
                   SLOT(onPlayerPropertiesChanged(QString,QVariantMap,QStringList)));
    m_watchedService.clear();

    // Nobody is around to tell us about playback any more: fall back to plain
    // polling so that another player can be picked up.
    if (m_playingKnown) {
        setPlaying(false, false);
    }
}

void LrcApplet::watchPlayer()
{
    unwatchPlayer();

    if (m_player.isEmpty()) {
        return;
    }

    m_watchedService = s_mprisPrefix + m_player;

    // Whatever the previous player told us is not this one's: not its track, not
    // its length, and not the line on the panel. Leaving that last one is what put
    // the words of one player under the name of another in the tooltip — and since
    // a player nobody is listening to is not polled, that line could stay there for
    // good. Until the new player's properties arrive there is nothing to show, and
    // an empty panel says so honestly.
    clearDisplay();

    m_awaitingState = true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(m_watchedService,
                s_playerPath,
                s_propertiesInterface,
                QStringLiteral("PropertiesChanged"),
                this,
                SLOT(onPlayerPropertiesChanged(QString,QVariantMap,QStringList)));

    requestPlayerProperties();
}

void LrcApplet::onPlayerPropertiesChanged(const QString &interfaceName, const QVariantMap &changed, const QStringList &invalidated)
{
    if (m_watchedService.isEmpty() || interfaceName != s_playerInterface) {
        return;
    }

    applyPlayerProperties(changed, invalidated);
}

void LrcApplet::onServiceOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    Q_UNUSED(oldOwner);

    // A player appeared. Who is playing may have changed without anything else
    // happening, so the list is asked again right away instead of on the next
    // scheduled sweep — otherwise a player that has just come back is not noticed
    // for up to half a minute.
    if (!newOwner.isEmpty() && name.startsWith(s_mprisPrefix)) {
        m_candidatesTimer.invalidate();
        considerPlayerSwitch();
        return;
    }

    // The player we were watching is gone: stop trusting its state and start
    // over with the remaining players. NameOwnerChanged reports the well-known
    // name in `name` and the unique one (":1.42") in `oldOwner`, so only the
    // first and the empty newOwner can be compared here.
    if (m_watchedService.isEmpty() || name != m_watchedService || !newOwner.isEmpty()) {
        return;
    }

    // Everything we know belonged to that process. The lyrics have to go too:
    // poll() returns early while the API lyrics are usable, so keeping them
    // would freeze the last line on screen and stop the rotation to whatever
    // player is left.
    clearDisplay();
    setPlaying(false, false);
    invalidatePlayerCandidates();
    unwatchPlayer();
    poll();
}

void LrcApplet::requestPlayerProperties()
{
    if (m_watchedService.isEmpty()) {
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(m_watchedService, s_playerPath, s_propertiesInterface, QStringLiteral("GetAll"));
    message << s_playerInterface;

    delete m_propertyWatcher;
    m_propertyWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, kDBusCallTimeoutMs), this);
    connect(m_propertyWatcher, &QDBusPendingCallWatcher::finished, this, &LrcApplet::onPlayerPropertiesFetched);
}

void LrcApplet::onPlayerPropertiesFetched()
{
    QDBusPendingCallWatcher *watcher = m_propertyWatcher;
    if (!watcher) {
        return;
    }

    const QDBusPendingReply<QVariantMap> reply = *watcher;
    m_propertyWatcher = nullptr;
    watcher->deleteLater();

    m_awaitingState = false;
    if (reply.isError()) {
        // Nobody answered, so nothing is known about this player beyond its name.
        // Letting the polling go on is better than staying silent forever.
        return;
    }

    applyPlayerProperties(reply.value(), {});
}

void LrcApplet::applyPlayerProperties(const QVariantMap &properties, const QStringList &invalidated)
{
    m_awaitingState = false;

    if (invalidated.contains(QStringLiteral("PlaybackStatus"))) {
        // The player no longer knows its own status: poll blindly.
        setPlaying(false, false);
        return;
    }

    const auto statusIt = properties.constFind(QStringLiteral("PlaybackStatus"));
    if (statusIt != properties.constEnd()) {
        const QString status = statusIt.value().toString();
        setPlaying(status.compare(QLatin1String("Playing"), Qt::CaseInsensitive) == 0, true);
        if (m_playingKnown) {
            // Playback state changed, so the interpolated position is stale and
            // worth asking for without waiting out the retry delay.
            m_positionValid = false;
            m_positionRetries.invalidate();
            requestPosition();
        }

        // Stopped is not paused. A paused track keeps its line, because it is
        // still the track being listened to; a stopped player has nothing left to
        // say, and polling has already stopped by then, so nothing else would ever
        // take the line down. That is how a closed YouTube tab left its text on
        // the panel for the rest of the session.
        if (status.compare(QLatin1String("Stopped"), Qt::CaseInsensitive) == 0) {
            clearDisplay();
        }
    }

    const auto metadataIt = properties.constFind(QStringLiteral("Metadata"));
    if (metadataIt != properties.constEnd()) {
        const QVariantMap metadata = toVariantMap(metadataIt.value());
        applyMetadata(metadata);
    }
}

K_PLUGIN_CLASS_WITH_JSON(LrcApplet, "metadata.json")

#include "lrcapplet.moc"
