/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcapplet.h"

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

#include <algorithm>

namespace
{
// lrc_tty may hit the network on a track it has never seen before, so give it
// a generous amount of time before deciding that it is stuck.
constexpr int kWatchdogTimeoutMs = 20000;
// Do not spam the shell when the binary is missing: retry once a minute.
constexpr int kMissingBinaryRetryMs = 60000;
// How many "no lyrics" answers in a row make us try the next MPRIS player.
constexpr int kNoLyricsBeforeSwitchingPlayer = 3;
// Enumerating bus names is a round trip to the bus, so don't do it on every
// tick: this is also how quickly we notice a player that just started.
constexpr int kPlayerListCacheMs = 5000;
// Generous upper bound for a local MPRIS property fetch.
constexpr int kDBusCallTimeoutMs = 5000;

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
    , m_propertyWatcher(nullptr)
    , m_namesWatcher(nullptr)
    , m_pollInterval(1000)
    , m_maxCharacters(40)
    , m_showTimestamp(false)
    , m_showIcon(true)
    , m_showTrackInfo(true)
    , m_pauseWhenIdle(true)
    , m_candidateIndex(0)
    , m_lastGoodCandidate(-1)
    , m_noLyricsCount(0)
    , m_active(false)
    , m_available(true)
    , m_playing(false)
    , m_playingKnown(false)
    , m_started(false)
{
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LrcApplet::poll);

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

    m_timer.start(m_pollInterval);
    poll();
}

void LrcApplet::refresh()
{
    invalidatePlayerCandidates();
    unwatchPlayer();
    setText(QString());
    setActive(false);
    setTrackInfo(QString());
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
    return config().readEntry(key, defaultValue);
}

int LrcApplet::intSetting(const QString &key, int defaultValue) const
{
    int value = config().readEntry(key, defaultValue);
    if (value <= 0) {
        value = defaultValue;
    }
    return value;
}

bool LrcApplet::boolSetting(const QString &key, bool defaultValue) const
{
    return config().readEntry(key, defaultValue);
}

void LrcApplet::readSettings()
{
    const QString binaryPath = setting(QStringLiteral("binaryPath"), QStringLiteral("lrc_tty"));
    const QString player = setting(QStringLiteral("player")).trimmed();
    const QString placeholder = setting(QStringLiteral("placeholderText"), QStringLiteral("♪"));
    const int pollInterval = qBound(200, intSetting(QStringLiteral("pollInterval"), 1000), 10000);
    const int maxCharacters = qBound(0, intSetting(QStringLiteral("maxCharacters"), 40), 500);
    const bool showTimestamp = boolSetting(QStringLiteral("showTimestamp"), false);
    const bool showIcon = boolSetting(QStringLiteral("showIcon"), true);
    const bool showTrackInfo = boolSetting(QStringLiteral("showTrackInfo"), true);
    const bool pauseWhenIdle = boolSetting(QStringLiteral("pauseWhenIdle"), true);

    const bool changed = binaryPath != m_binaryPath //
        || player != m_configuredPlayer //
        || placeholder != m_placeholderText //
        || pollInterval != m_pollInterval //
        || maxCharacters != m_maxCharacters //
        || showTimestamp != m_showTimestamp //
        || showIcon != m_showIcon //
        || showTrackInfo != m_showTrackInfo //
        || pauseWhenIdle != m_pauseWhenIdle;

    m_binaryPath = binaryPath;
    m_configuredPlayer = player;
    m_placeholderText = placeholder;
    m_pollInterval = pollInterval;
    m_maxCharacters = maxCharacters;
    m_showTimestamp = showTimestamp;
    m_showIcon = showIcon;
    m_showTrackInfo = showTrackInfo;
    m_pauseWhenIdle = pauseWhenIdle;

    if (changed) {
        Q_EMIT settingsChanged();
        invalidatePlayerCandidates();
    }

    if (m_timer.interval() != m_pollInterval && m_timer.isActive()) {
        m_timer.start(m_pollInterval);
    }
}

QString LrcApplet::placeholderText() const
{
    return m_placeholderText;
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

    const QStringList candidates = playerCandidates();
    if (candidates.isEmpty()) {
        // We have not heard from the bus yet (or there is no player at all).
        if (!m_candidatesTimer.isValid() || m_candidatesTimer.elapsed() > kPlayerListCacheMs) {
            refreshPlayerCandidates();
        }
        return;
    }

    const QString player = candidates.at(qBound(0, m_candidateIndex, candidates.size() - 1));
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

    m_watchdog.start(kWatchdogTimeoutMs);
    m_process->start(m_binaryPath, arguments);
}

void LrcApplet::onProcessFinished()
{
    m_watchdog.stop();

    if (!m_available) {
        setAvailable(true);
        if (m_timer.interval() != m_pollInterval) {
            m_timer.start(m_pollInterval);
        }
    }

    const QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    if (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0) {
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
}

void LrcApplet::setTrackInfo(const QString &trackInfo)
{
    if (m_trackInfo == trackInfo) {
        return;
    }
    m_trackInfo = trackInfo;
    Q_EMIT trackInfoChanged();
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

    // Nothing is being watched at the moment, so this may be a good moment to
    // start paying attention to a player.
    if (m_watchedService.isEmpty() && m_player.isEmpty()) {
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
    // The player we were watching is gone: stop trusting its state and start
    // over with the remaining players.
    if (m_watchedService.isEmpty() || name != m_watchedService || newOwner.isEmpty() || oldOwner != name) {
        return;
    }

    setPlaying(false, false);
    setTrackInfo(QString());
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
    }
}

K_PLUGIN_CLASS_WITH_JSON(LrcApplet, "metadata.json")

#include "lrcapplet.moc"
