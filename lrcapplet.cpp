/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcapplet.h"

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

// All entries of contents/config/main.xml live in this group, and that is also
// where KConfigLoader writes them (the applet's own group is only prepended to
// grouped entries), so the settings have to be read back from here.
const QString s_configGroup = QStringLiteral("General");
// Matches the default of pollInterval in contents/config/main.xml.
constexpr int kDefaultPollInterval = 200;
// How often the playback position may be asked for again while it stays unknown.
constexpr qint64 kPositionRetryIntervalMs = 2000;
// How often a known position is checked against the player, which is the only
// way to hear about a seek: MPRIS has no signal for one.
constexpr qint64 kPositionCheckMs = 1500;
// A difference below this is the clock running slightly off, a bigger one means
// playback was moved.
constexpr qint64 kPositionSnapToleranceMs = 350;
// How far the position may fall from a word and still light it up: on a fast
// line the gaps between words are shorter than a tick.
constexpr qint64 kWordSnapToleranceMs = 150;
// How often the word highlight is recomputed. Fast lines have words well under
// 100ms, so a coarse tick skips them.
constexpr int kWordTickMs = 20;

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

/**
 * How much this player looks like it is playing music rather than something
 * that merely implements MPRIS.
 *
 * Plenty of applications implement org.mpris.MediaPlayer2.Player for things
 * that are not tracks: Telegram Desktop reports an unviewed voice message in a
 * minimised window as "Playing", and the system then hands it out as the main
 * media player. What tells a real player apart is the metadata a track has and a
 * voice message does not: a length, an album, a track number, cover art.
 */
int musicScore(const QVariantMap &properties)
{
    const QVariantMap metadata = toVariantMap(properties.value(QStringLiteral("Metadata")));

    int score = 0;
    const QString status = properties.value(QStringLiteral("PlaybackStatus")).toString();
    // Playing counts for more than paused: a paused music player still describes
    // the track better than a "playing" voice message does.
    if (status == QLatin1String("Playing")) {
        score += 6;
    } else if (status == QLatin1String("Paused")) {
        score += 2;
    }

    if (metadata.value(QStringLiteral("mpris:length")).toLongLong() > 0) {
        score += 2;
    }
    if (!metadata.value(QStringLiteral("xesam:album")).toString().isEmpty()) {
        score += 2;
    }
    if (!metadata.value(QStringLiteral("mpris:artUrl")).toString().isEmpty()) {
        score += 1;
    }
    if (!metadata.value(QStringLiteral("xesam:trackNumber")).isNull()) {
        score += 1;
    }
    if (!toStringList(metadata.value(QStringLiteral("xesam:artist"))).isEmpty()) {
        score += 1;
    }

    // The convention for "this is not a track" that Telegram Desktop and a few
    // others use. Their titles are not lyrics either, so they go to the back
    // rather than merely losing points.
    if (metadata.value(QStringLiteral("mpris:trackid")).toString().startsWith(QLatin1String("/org/desktop_app/"))) {
        score -= 10;
    }

    return score;
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
    clearLyrics();
    m_spotifyTrackId.clear();
    invalidatePlayerCandidates();
    unwatchPlayer();
    setText(QString());
    setActive(false);
    setTrackInfo(QString());
    m_trackArtist.clear();
    m_trackTitle.clear();
    m_trackLengthUs = 0;
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
    const QString placeholder = setting(QStringLiteral("placeholderText"), QStringLiteral("♪"));
    const int pollInterval = qBound(200, intSetting(QStringLiteral("pollInterval"), kDefaultPollInterval), 10000);
    const int maxCharacters = qBound(0, intSetting(QStringLiteral("maxCharacters"), 40), 500);
    const bool showTimestamp = boolSetting(QStringLiteral("showTimestamp"), false);
    const bool showIcon = boolSetting(QStringLiteral("showIcon"), true);
    const bool showTrackInfo = boolSetting(QStringLiteral("showTrackInfo"), true);
    const bool compactPanel = boolSetting(QStringLiteral("compactPanel"), false);
    const int wordStyle = qBound(0, intSetting(QStringLiteral("wordStyle"), 0), 1);
    const int lyricOffset = qBound(-2000, intSetting(QStringLiteral("lyricOffset"), 0), 2000);
    const bool pauseWhenIdle = boolSetting(QStringLiteral("pauseWhenIdle"), true);
    const bool useSpicy = boolSetting(QStringLiteral("useSpicyLyrics"), true);
    const QString spicyKey = spicyKeySetting();

    const bool changed = binaryPath != m_binaryPath //
        || player != m_configuredPlayer //
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
        requestSpicyLyrics();
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
    return m_fromSpicy ? m_lyrics.attribution : QString();
}

QString LrcApplet::attributionUrl() const
{
    return m_fromSpicy ? m_lyrics.attributionUrl : QString();
}

void LrcApplet::poll()
{
    if (!m_started || m_process->state() != QProcess::NotRunning) {
        return;
    }

    // Nothing is playing, so there is nothing to fetch. The MPRIS watcher wakes
    // us up again as soon as playback starts.
    if (m_pauseWhenIdle && m_playingKnown && !m_playing) {
        return;
    }

    // The API gave us word-level timings for this track: we time the line
    // ourselves, so there is nothing left for lrc_tty to do until the track
    // changes or the API turns out to have nothing.
    if (m_fromSpicy && m_lyrics.isUsable()) {
        return;
    }

    // Even with players known, the bus is asked again now and then: a music
    // player can appear long after we settled for something else, and whoever
    // looked like music last time may not be playing any more.
    considerPlayerSwitch();

    const QStringList candidates = playerCandidates();
    if (candidates.isEmpty()) {
        return;
    }

    const QString player = candidates.at(qBound(0, m_candidateIndex, int(candidates.size()) - 1));
    if (player != m_player) {
        setPlayer(player);
        unwatchPlayer();
        watchPlayer();
    }

    QStringList arguments;
    arguments << QStringLiteral("--lines") << QStringLiteral("1") << QStringLiteral("--raw");
    if (m_showTimestamp) {
        arguments << QStringLiteral("--timestamp");
    }
    arguments << QStringLiteral("--player") << player;

    // Browsers, YouTube and yt-dlp hand out titles like "Song (Official Video)
    // [4K]" or "Artist - Topic", and lrclib matches on words. Tell lrc_tty what to
    // look for, but only when cleaning the title actually changed something: a
    // title that needs nothing is better left to the player.
    QString artist = m_trackArtist;
    QString title = m_trackTitle;
    splitTrackArtistAndTitle(&artist, &title);
    const QString cleanedTitle = cleanTrackTitle(title);
    const QString cleanedArtist = cleanTrackArtist(artist);
    if (!cleanedTitle.isEmpty() && (cleanedTitle != m_trackTitle || cleanedArtist != m_trackArtist)) {
        arguments << QStringLiteral("--artist") << cleanedArtist;
        arguments << QStringLiteral("--title") << cleanedTitle;
    }
    if (m_trackLengthUs > 0) {
        // With several versions of a track, the length is what tells them apart.
        // MPRIS reports it in microseconds, lrc_tty wants whole seconds.
        arguments << QStringLiteral("--duration") << QString::number(m_trackLengthUs / 1000000);
    }

    m_watchdog.start(kWatchdogTimeoutMs);
    m_process->start(m_binaryPath, arguments);
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

    if (!m_available) {
        setAvailable(true);
        setError(QString());
        if (m_timer.interval() != m_pollInterval) {
            m_timer.start(m_pollInterval);
        }
    }

    const QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    if (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0) {
        return;
    }

    // A track that the API timed better than lrc_tty does: ignore this answer.
    if (m_fromSpicy && m_lyrics.isUsable()) {
        return;
    }

    // lrc_tty prints a single line, but be forgiving about trailing noise.
    QString line;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString candidate = lines.at(i).trimmed();
        if (!candidate.isEmpty()) {
            line = candidate;
            break;
        }
    }

    if (isNoLyricsMarker(line)) {
        ++m_noLyricsCount;
        // There is a small chance that the currently selected player simply has
        // no lyrics for this track: give the others a try before giving up.
        if (m_noLyricsCount >= kNoLyricsBeforeSwitchingPlayer && playerCandidates().size() > 1) {
            m_noLyricsCount = 0;
            m_candidateIndex = (m_lastGoodCandidate >= 0 && m_lastGoodCandidate != m_candidateIndex) ? m_lastGoodCandidate
                                                                                                  : m_candidateIndex + 1;
            unwatchPlayer();
        }
        setText(QString());
        setActive(false);
        return;
    }

    m_noLyricsCount = 0;
    m_lastGoodCandidate = m_candidateIndex;
    setText(line);
    setActive(true);
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

    // The word timer exists to move the highlight, and nothing moves while the
    // music is stopped: fifty wake-ups a second for a frozen word.
    if (known && !playing) {
        m_wordTimer.stop();
    } else if (playing && m_lyrics.isUsable()) {
        m_wordTimer.start();
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
    m_missingReason = MissingReason::Unsynced;
    m_positionMs = 0;
    m_positionBaseMs = 0;
    m_positionValid = false;
    m_positionClock.invalidate();
    m_lineIndex = -1;
    m_wordCursor = 0;
    m_wordTimer.stop();
    Q_EMIT wordChanged();
    Q_EMIT lyricsChanged();
}

void LrcApplet::applyMetadata(const QVariantMap &metadata)
{
    const QString trackId = SpicyLyrics::trackIdFromMpris(metadata.value(QStringLiteral("mpris:trackid")));
    if (trackId == m_spotifyTrackId) {
        return;
    }

    m_spotifyTrackId = trackId;
    clearLyrics();

    // Playback position is only meaningful for the new track, and it has to be
    // asked for right away rather than after the retry delay.
    m_positionValid = false;
    m_positionBaseMs = 0;
    m_positionRetries.invalidate();
    requestPosition();
    requestSpicyLyrics();
}

void LrcApplet::requestSpicyLyrics()
{
    if (!m_useSpicy || m_spotifyTrackId.isEmpty()) {
        return;
    }
    m_spicy->request(m_spotifyTrackId);
}

void LrcApplet::onSpicyLoaded(const QString &trackId, const Lyrics &lyrics)
{
    // A slower request for the previous track may answer after we moved on.
    if (trackId != m_spotifyTrackId) {
        return;
    }

    m_lyrics = lyrics;
    m_fromSpicy = true;
    m_wordSynced = lyrics.type == LyricsType::Syllable;

    // The line now comes from the API, so the process polling would only be a
    // fallback for a track the API does not know.
    requestPosition();
    updateWord();
    m_wordTimer.start();
    Q_EMIT lyricsChanged();
    poll();
}

void LrcApplet::onSpicyMissing(const QString &trackId, MissingReason reason)
{
    if (trackId != m_spotifyTrackId) {
        return;
    }
    m_missingReason = reason;
    // Nothing from the API: lrc_tty keeps doing its job.
    if (m_fromSpicy) {
        clearLyrics();
    }
    poll();
}

void LrcApplet::requestPosition()
{
    if (m_watchedService.isEmpty()) {
        return;
    }

    // updateWord() runs 25 times a second and asks for the position while it has
    // none; a player that never answers must not turn that into a D-Bus flood.
    if (m_positionWatcher) {
        return;
    }
    if (m_positionRetries.isValid() && m_positionRetries.elapsed() < kPositionRetryIntervalMs) {
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

    const int lineIndex = m_lyrics.lineAt(m_positionMs, m_lineIndex);
    if (lineIndex < 0) {
        // Between lines nobody is singing; keep whatever was shown.
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
        if (!m_word.isEmpty()) {
            m_word.clear();
            m_wordStart = 0;
            m_wordEnd = 0;
            m_wordProgress = 0.0;
            Q_EMIT wordChanged();
        }
    }

    if (!m_wordSynced || line.words.isEmpty()) {
        if (!m_word.isEmpty() || m_wordProgress != 0.0) {
            m_word.clear();
            m_wordStart = 0;
            m_wordEnd = 0;
            m_wordProgress = 0.0;
            Q_EMIT wordChanged();
        }
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
    if (!m_configuredPlayer.isEmpty()) {
        return {m_configuredPlayer};
    }

    return m_candidates;
}

void LrcApplet::refreshPlayerCandidates()
{
    if (!m_configuredPlayer.isEmpty()) {
        m_candidates = {m_configuredPlayer};
        m_candidatesTimer.start();
        return;
    }

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

    m_candidates = mprisPlayersFromServiceNames(reply.value());
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
            } else {
                m_playerScores.insert(player, musicScore(reply.value()));
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
    // A stable sort keeps the name-based preference as the tie breaker.
    std::stable_sort(m_candidates.begin(), m_candidates.end(), [this](const QString &lhs, const QString &rhs) {
        return m_playerScores.value(lhs, 0) > m_playerScores.value(rhs, 0);
    });

    // Where in the list to continue. A player that is giving us lyrics is left
    // alone unless something else is much more likely to be the music; a player
    // that gives nothing has nothing to lose, so the best one is taken right
    // away.
    const int currentIndex = m_candidates.indexOf(m_player);
    bool takeBest = currentIndex < 0;
    if (currentIndex >= 0) {
        takeBest = m_text.isEmpty() || m_playerScores.value(m_candidates.first(), 0) > m_playerScores.value(m_player, 0) + kPlayerSwitchMargin;
        m_candidateIndex = takeBest ? 0 : currentIndex;
    } else {
        m_candidateIndex = 0;
    }

    m_lastGoodCandidate = -1;
    m_noLyricsCount = 0;

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

    if (m_watchedService.isEmpty()) {
        return;
    }

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
    clearLyrics();
    m_spotifyTrackId.clear();
    setText(QString());
    setActive(false);
    setPlaying(false, false);
    setTrackInfo(QString());
    m_trackArtist.clear();
    m_trackTitle.clear();
    m_trackLengthUs = 0;
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

    if (reply.isError()) {
        return;
    }

    applyPlayerProperties(reply.value(), {});
}

void LrcApplet::applyPlayerProperties(const QVariantMap &properties, const QStringList &invalidated)
{
    if (invalidated.contains(QStringLiteral("PlaybackStatus"))) {
        // The player no longer knows its own status: poll blindly.
        setPlaying(false, false);
        return;
    }

    const auto statusIt = properties.constFind(QStringLiteral("PlaybackStatus"));
    if (statusIt != properties.constEnd()) {
        setPlaying(statusIt.value().toString().compare(QLatin1String("Playing"), Qt::CaseInsensitive) == 0, true);
        if (m_playingKnown) {
            // Playback state changed, so the interpolated position is stale and
            // worth asking for without waiting out the retry delay.
            m_positionValid = false;
            m_positionRetries.invalidate();
            requestPosition();
        }
    }

    const auto metadataIt = properties.constFind(QStringLiteral("Metadata"));
    if (metadataIt != properties.constEnd()) {
        const QVariantMap metadata = toVariantMap(metadataIt.value());

        QStringList artists = toStringList(metadata.value(QStringLiteral("xesam:artist")));
        artists.removeAll(QString());
        const QString title = metadata.value(QStringLiteral("xesam:title")).toString();

        QString trackInfo;
        if (!artists.isEmpty() && !title.isEmpty()) {
            trackInfo = artists.join(QStringLiteral(", ")) + QStringLiteral(" — ") + title;
        } else if (!title.isEmpty()) {
            trackInfo = title;
        } else {
            trackInfo = artists.join(QStringLiteral(", "));
        }

        setTrackInfo(trackInfo);

        m_trackArtist = toStringList(metadata.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", "));
        m_trackTitle = metadata.value(QStringLiteral("xesam:title")).toString();
        // MPRIS reports mpris:length in microseconds.
        m_trackLengthUs = metadata.value(QStringLiteral("mpris:length")).toLongLong();

        applyMetadata(metadata);
    }
}

K_PLUGIN_CLASS_WITH_JSON(LrcApplet, "metadata.json")

#include "lrcapplet.moc"
