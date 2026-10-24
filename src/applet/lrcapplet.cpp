/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "lrcapplet.h"

#include "lrcparser.h"
#include "lrclib.h"
#include "playermeta.h"
#include "trackname.h"
#include "lyricselection.h"

#include <KLocalizedString>
#include <KPluginFactory>
#include <kconfigpropertymap.h>

#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>

namespace
{
// lrc_tty may hit the network on a track it has never seen before, so give it
// a generous amount of time before deciding that it is stuck.
constexpr int kWatchdogTimeoutMs = 20000;
// Do not spam the shell when the binary is missing: retry now and then.
constexpr int kMissingBinaryRetryMs = 5000;
// Enumerating bus names is a round trip to the bus, so don't do it on every
// tick: this is also how quickly we notice a player that just started.
constexpr int kPlayerListCacheMs = 5000;
// Probing every player should not block the widget if one of them hangs.
constexpr int kProbeTimeoutMs = 2000;
// How often the bus is asked again who is playing, even when a player is found.
constexpr int kPlayerRerankMs = 5000;
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
constexpr int kDefaultPollInterval = 2000;
// How often the playback position may be asked for again while it stays unknown.
constexpr qint64 kPositionRetryIntervalMs = 150;
// How often a known position is checked against the player, which is the only
// way to hear about a seek: MPRIS has no signal for one. Nothing else in the
// widget costs as much as a missed seek looking broken, so this is short: a
// property read on the session bus is a fraction of a millisecond.
constexpr qint64 kPositionCheckMs = 400;
// A difference below this is the clock running slightly off; a bigger one means
// playback was moved (seeked) or the player flushed its internal pre-buffer.
constexpr qint64 kPositionSnapToleranceMs = 500;
// How long a word highlight may linger across a tiny gap between syllables (<= 80ms)
// to prevent flicker, without ever snapping forward to future words.
constexpr qint64 kWordGapHoldMs = 80;
// Hardware audio buffer latency compensation for PipeWire / PulseAudio / ALSA.
// MPRIS Position reports decoder stream time, which leads acoustic output by buffer depth (~90ms).
constexpr qint64 kAudioLeadCompensationMs = 90;
// Spotify on Linux pre-buffers audio for gapless playback into a ~1000ms sink buffer.
// During continuous playback transitions, Spotify updates Metadata and reports Position
// from its decoder write-head, leading acoustic speaker output by the buffer depth (~1000ms).
// This lead is flushed and reset to 0 upon pause, seek, or manual track selection.
constexpr qint64 kSpotifyGaplessLeadMs = 1000;
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

QString normalizeBlacklistEntry(QString text)
{
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QChar(0x2212), QLatin1Char('-'));
    return text.simplified().toLower();
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
    , m_process(nullptr)
    , m_pipeline(new LyricsPipelineManager(this))
    , m_localProvider(new LocalProvider(this))
    , m_unisonProvider(new UnisonProvider(this))
    , m_lrcredProvider(new LrcRedProvider(this))
    , m_neteaseProvider(new NeteaseProvider(this))
    , m_kugouProvider(new KugouProvider(this))
    , m_qqmusicProvider(new QqMusicProvider(this))
    , m_amllProvider(new AmllProvider(this))
    , m_spicyProvider(new SpicyLyricsProvider(this))
    , m_lrclibProvider(new LrclibProvider(this))
    , m_customProvider(new CustomProvider(this))
    , m_spicy(m_spicyProvider->client())
    , m_lrclib(m_lrclibProvider->client())
    , m_trackLengthUs(0)
    , m_webPageSession(false)
    , m_useCleanedLookup(true)
    , m_lastLookupWasCleaned(false)
    , m_propertyWatcher(nullptr)
    , m_namesWatcher(nullptr)
    , m_positionWatcher(nullptr)
    , m_pollInterval(kDefaultPollInterval)
    , m_maxCharacters(100)
    , m_showTimestamp(false)
    , m_showIcon(false)
    , m_showTrackInfo(true)
    , m_attributionLayout(QStringLiteral("Right"))
    , m_wordStyle(1)
    , m_lyricOffset(0)
    , m_pauseWhenIdle(true)
    , m_pauseHideDelay(3)
    , m_fadeTransition(true)
    , m_clickAction(0)
    , m_middleClickAction(1)
    , m_textAlignment(0)
    , m_useSpicy(true)
    , m_probed(false)
    , m_awaitingState(false)
    , m_candidateIndex(0)
    , m_active(false)
    , m_available(true)
    , m_playing(false)
    , m_playingKnown(false)
    , m_started(false)
    , m_lyricsLookupDone(false)
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
    m_pipeline->registerProvider(m_localProvider);
    m_pipeline->registerProvider(m_unisonProvider);
    m_pipeline->registerProvider(m_lrcredProvider);
    m_pipeline->registerProvider(m_neteaseProvider);
    m_pipeline->registerProvider(m_kugouProvider);
    m_pipeline->registerProvider(m_qqmusicProvider);
    m_pipeline->registerProvider(m_amllProvider);
    m_pipeline->registerProvider(m_spicyProvider);
    m_pipeline->registerProvider(m_lrclibProvider);
    m_pipeline->registerProvider(m_customProvider);

    connect(m_pipeline, &LyricsPipelineManager::lyricsResolved, this, &LrcApplet::onPipelineResolved);
    connect(m_pipeline, &LyricsPipelineManager::allFailed, this, &LrcApplet::onPipelineAllFailed);
    connect(m_pipeline, &LyricsPipelineManager::lookupFinished, this, &LrcApplet::onPipelineLookupFinished);

    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LrcApplet::poll);

    m_wordTimer.setTimerType(Qt::PreciseTimer);
    m_wordTimer.setInterval(kWordTickMs);
    connect(&m_wordTimer, &QTimer::timeout, this, &LrcApplet::updateWord);

    // Runs whether or not anything is playing: this is how a player that comes
    // back after being closed gets noticed.
    m_busTimer.setInterval(kPlayerRerankMs);
    connect(&m_busTimer, &QTimer::timeout, this, &LrcApplet::considerPlayerSwitch);

    m_pauseTimer.setSingleShot(true);
    connect(&m_pauseTimer, &QTimer::timeout, this, &LrcApplet::onPauseTimeout);

    m_expiryTimer.setSingleShot(true);
    connect(&m_expiryTimer, &QTimer::timeout, this, &LrcApplet::onLyricsExpired);

    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_process && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
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

    // Listen to PropertiesChanged across all MPRIS players so that when another
    // player starts playback, we hear about it immediately rather than waiting for sweeps.
    QDBusConnection::sessionBus().connect(QString(),
                                          s_playerPath,
                                          s_propertiesInterface,
                                          QStringLiteral("PropertiesChanged"),
                                          this,
                                          SLOT(onAnyPlayerPropertiesChanged(QString,QVariantMap,QStringList)));

    connect(m_spicy, &SpicyLyrics::loaded, this, &LrcApplet::onSpicyLoaded);
    connect(m_spicy, &SpicyLyrics::missing, this, &LrcApplet::onSpicyMissing);
    connect(m_lrclib, &LrclibClient::loaded, this, &LrcApplet::onLrclibLoaded);
    connect(m_lrclib, &LrclibClient::missing, this, &LrcApplet::onLrclibMissing);
}

LrcApplet::~LrcApplet()
{
    cancelProcess();
    m_pipeline->cancel();
}

void LrcApplet::constraintsEvent(Constraints constraints)
{
    // Applet::init() is not called by the framework, and config() is not
    // available from the constructor, so start once the UI is up and running.
    if (constraints & UiReadyConstraint) {
        startPolling();

        if (!internalAction(QStringLiteral("refresh"))) {
            QAction *refreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Update lyrics now"), this);
            connect(refreshAction, &QAction::triggered, this, &LrcApplet::refresh);
            setInternalAction(QStringLiteral("refresh"), refreshAction);
        }
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
    TrackMetadata meta;
    meta.title = m_trackTitle;
    meta.artist = m_trackArtist;
    meta.album = m_trackAlbum;
    meta.durationMs = m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0;
    meta.spotifyTrackId = m_spotifyTrackId;
    meta.mprisTrackId = m_mprisTrackId;
    meta.url = m_trackUrl;
    meta.isWebSession = m_webPageSession;

    TrackMetadata searchMeta = meta;
    normalizeTrackMetadata(&searchMeta);

    if (m_pipeline) {
        m_pipeline->invalidateCaches(searchMeta);
    }
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
    return config().group(s_configGroup).readEntry(key, defaultValue);
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
    const QString player = setting(QStringLiteral("player")).trimmed();
    const QString preferredPlayer = setting(QStringLiteral("preferredPlayer")).trimmed();
    const QString placeholder = setting(QStringLiteral("placeholderText"), QStringLiteral("♪"));
    const int pollInterval = qBound(500, intSetting(QStringLiteral("pollInterval"), kDefaultPollInterval), 10000);
    const int maxCharacters = qBound(0, intSetting(QStringLiteral("maxCharacters"), 100), 500);
    const bool showTimestamp = boolSetting(QStringLiteral("showTimestamp"), false);
    const bool showIcon = boolSetting(QStringLiteral("showIcon"), false);
    const bool showTrackInfo = boolSetting(QStringLiteral("showTrackInfo"), true);
    const QString attributionLayoutRaw = setting(QStringLiteral("attributionLayout")).trimmed();
    const QString attributionLayout = (attributionLayoutRaw.compare(QLatin1String("Below"), Qt::CaseInsensitive) == 0 || attributionLayoutRaw == QLatin1String("1"))
        ? QStringLiteral("Below")
        : QStringLiteral("Right");
    const int wordStyle = qBound(0, intSetting(QStringLiteral("wordStyle"), 1), 4);
    const int lyricOffset = qBound(-2000, intSetting(QStringLiteral("lyricOffset"), 0), 2000);
    const bool pauseWhenIdle = boolSetting(QStringLiteral("pauseWhenIdle"), false);
    const int pauseHideDelay = qBound(0, intSetting(QStringLiteral("pauseHideDelay"), 3), 60);
    const bool fadeTransition = boolSetting(QStringLiteral("fadeTransition"), true);
    const int clickAction = qBound(0, intSetting(QStringLiteral("clickAction"), 0), 1);
    const int middleClickAction = qBound(0, intSetting(QStringLiteral("middleClickAction"), 1), 2);
    const int textAlignment = qBound(0, intSetting(QStringLiteral("textAlignment"), 0), 1);
    const QString customHighlightColor = setting(QStringLiteral("customHighlightColor")).trimmed();
    const QString blacklistedTracks = setting(QStringLiteral("blacklistedTracks")).trimmed();
    const bool useSpicy = boolSetting(QStringLiteral("useSpicyLyrics"), true);
    const QString spicyKey = spicyKeySetting();
    const bool showBackingVocals = boolSetting(QStringLiteral("showBackingVocals"), false);

    const bool changed = player != m_configuredPlayer //
        || preferredPlayer != m_preferredPlayer //
        || placeholder != m_placeholderText //
        || pollInterval != m_pollInterval //
        || maxCharacters != m_maxCharacters //
        || showTimestamp != m_showTimestamp //
        || showIcon != m_showIcon //
        || showTrackInfo != m_showTrackInfo //
        || attributionLayout != m_attributionLayout //
        || wordStyle != m_wordStyle //
        || lyricOffset != m_lyricOffset //
        || pauseWhenIdle != m_pauseWhenIdle //
        || pauseHideDelay != m_pauseHideDelay //
        || fadeTransition != m_fadeTransition //
        || clickAction != m_clickAction //
        || middleClickAction != m_middleClickAction //
        || textAlignment != m_textAlignment //
        || customHighlightColor != m_customHighlightColor //
        || blacklistedTracks != m_blacklistedTracks //
        || useSpicy != m_useSpicy //
        || spicyKey != m_spicyKey //
        || showBackingVocals != m_showBackingVocals;

    const bool blacklistChanged = (blacklistedTracks != m_blacklistedTracks);

    m_configuredPlayer = player;
    m_preferredPlayer = preferredPlayer;
    m_placeholderText = placeholder;
    m_pollInterval = pollInterval;
    m_maxCharacters = maxCharacters;
    m_showTimestamp = showTimestamp;
    m_showIcon = showIcon;
    m_showTrackInfo = showTrackInfo;
    m_attributionLayout = attributionLayout;
    m_wordStyle = wordStyle;
    m_lyricOffset = lyricOffset;
    m_pauseWhenIdle = pauseWhenIdle;
    m_pauseHideDelay = pauseHideDelay;
    m_fadeTransition = fadeTransition;
    m_clickAction = clickAction;
    m_middleClickAction = middleClickAction;
    m_textAlignment = textAlignment;
    m_customHighlightColor = customHighlightColor;
    m_blacklistedTracks = blacklistedTracks;

    const QString defaultOrder = QStringLiteral("local,unison,lrcred,netease,kugou,qqmusic,amll,spicy,lrclib,custom");
    const QString providerOrderRaw = setting(QStringLiteral("providerOrder"), defaultOrder).trimmed();
    const QString disabledProvidersRaw = setting(QStringLiteral("disabledProviders")).trimmed();
    const QString localFolder = setting(QStringLiteral("localLyricsFolder")).trimmed();
    const QString customUrl = setting(QStringLiteral("customLyricsUrl")).trimmed();
    const QString customHeaders = setting(QStringLiteral("customLyricsHeaders")).trimmed();

    const bool localFolderChanged = (localFolder != m_localFolderConfig);
    const bool customUrlChanged = (customUrl != m_customUrlConfig);
    const bool customHeadersChanged = (customHeaders != m_customHeadersConfig);
    const bool orderChanged = (providerOrderRaw != m_providerOrderConfig);
    const bool disabledProvidersChanged = (disabledProvidersRaw != m_disabledProvidersConfig);
    const bool useSpicyChanged = (useSpicy != m_useSpicy);
    const bool spicyKeyChanged = (spicyKey != m_spicyKey);

    const bool providerSettingsChanged = localFolderChanged
        || customUrlChanged
        || customHeadersChanged
        || orderChanged
        || disabledProvidersChanged
        || useSpicyChanged
        || spicyKeyChanged;

    m_localFolderConfig = localFolder;
    m_customUrlConfig = customUrl;
    m_customHeadersConfig = customHeaders;
    m_providerOrderConfig = providerOrderRaw;
    m_disabledProvidersConfig = disabledProvidersRaw;
    m_useSpicy = useSpicy;
    m_spicyKey = spicyKey;

    const bool backingToggleChanged = (showBackingVocals != m_showBackingVocals);
    m_showBackingVocals = showBackingVocals;
    if (backingToggleChanged) {
        if (!m_showBackingVocals) {
            clearBacking();
        }
        updateWord();
    }

    if (m_localProvider) {
        m_localProvider->setCustomDirectory(localFolder);
    }

    if (m_customProvider) {
        m_customProvider->setUrlTemplate(customUrl);
        m_customProvider->setHeaders(customHeaders);
    }

    m_spicy->setKey(m_spicyKey);
    m_spicy->setEnabled(m_useSpicy);

    if (m_pipeline) {
        if (!providerOrderRaw.isEmpty()) {
            m_pipeline->setProviderOrder(providerOrderRaw.split(QLatin1Char(','), Qt::SkipEmptyParts));
        }
        const QStringList disabledList = disabledProvidersRaw.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (LyricsProvider *p : m_pipeline->allProviders()) {
            const bool shouldEnable = !disabledList.contains(p->id());
            if (p->id() == QLatin1String("spicy")) {
                p->setEnabled(shouldEnable && m_useSpicy);
            } else {
                p->setEnabled(shouldEnable);
            }
        }
    }

    if (orderChanged || disabledProvidersChanged) {
        Q_EMIT providerOrderChanged();
    }

    if (blacklistChanged) {
        Q_EMIT trackBlacklistChanged();
    }

    if (m_playingKnown && !m_playing) {
        if (m_pauseHideDelay > 0 && !m_pauseHidden) {
            m_pauseTimer.start(m_pauseHideDelay * 1000);
        } else if (m_pauseHideDelay == 0 && m_pauseHidden) {
            m_pauseTimer.stop();
            m_pauseHidden = false;
            Q_EMIT pauseHiddenChanged();
            if (m_lyrics.isUsable()) {
                m_wordTimer.start(kIdleTickMs);
                updateWord();
            }
        }
    }

    if (changed) {
        Q_EMIT settingsChanged();
        invalidatePlayerCandidates();
    }

    if (providerSettingsChanged && !m_watchedService.isEmpty() && !m_trackTitle.isEmpty()) {
        m_lyricsLookupDone = false;
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

QString LrcApplet::attributionLayout() const
{
    return m_attributionLayout;
}

int LrcApplet::wordStyle() const
{
    return m_wordStyle;
}

int LrcApplet::lyricOffset() const
{
    return m_lyricOffset;
}

QString LrcApplet::fullLyrics() const
{
    if (isSpicyExpired() || !m_lyrics.isUsable()) {
        return QString();
    }
    QStringList lines;
    lines.reserve(m_lyrics.lines.size());
    for (const auto &line : m_lyrics.lines) {
        lines.append(line.text);
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

void LrcApplet::copyCurrentLine()
{
    if (m_fromSpicy || isSpicyExpired()) {
        return;
    }
    // Read the source line: m_text may include the optional timestamp prefix.
    if ((m_text.isEmpty() && (!m_showBackingVocals || m_backingText.isEmpty())) || m_lineIndex < 0 || m_lineIndex >= m_lyrics.lines.size()) {
        return;
    }
    const QString line = m_lyrics.lines.at(m_lineIndex).text.trimmed();
    if (!line.isEmpty()) {
        QGuiApplication::clipboard()->setText(line);
    }
}

int LrcApplet::currentLyricOffset() const
{
    if (isSpicyExpired() || m_text.isEmpty() || m_lineIndex < 0 || m_lineIndex >= m_lyrics.lines.size()) {
        return -1;
    }
    QStringList lines;
    int offset = 0;
    for (int i = 0; i < m_lyrics.lines.size(); ++i) {
        const QString &text = m_lyrics.lines.at(i).text;
        lines.append(text);
        if (i < m_lineIndex) {
            offset += text.size() + 1;
        }
    }
    const QString raw = lines.join(QLatin1Char('\n'));
    int leadingSpace = 0;
    while (leadingSpace < raw.size() && raw.at(leadingSpace).isSpace()) {
        ++leadingSpace;
    }
    return qMax(0, offset - leadingSpace);
}

QString LrcApplet::attribution() const
{
    if (isSpicyExpired()) {
        return QString();
    }
    return m_lyrics.attribution;
}

QString LrcApplet::attributionUrl() const
{
    if (isSpicyExpired()) {
        return QString();
    }
    return m_lyrics.attributionUrl;
}

QString LrcApplet::provider() const
{
    if (isSpicyExpired()) {
        return QString();
    }
    if (!m_lyrics.attributionData.provider.isEmpty()) {
        return m_lyrics.attributionData.provider;
    }
    if (!m_lyrics.attribution.isEmpty()) {
        return m_lyrics.attribution;
    }
    if (!m_activeProviderId.isEmpty() && m_pipeline) {
        LyricsProvider *p = m_pipeline->provider(m_activeProviderId);
        if (p) {
            return p->displayName();
        }
    }
    return QString();
}

QString LrcApplet::providerUrl() const
{
    if (isSpicyExpired()) {
        return QString();
    }
    if (!m_lyrics.attributionData.providerUrl.isEmpty()) {
        return m_lyrics.attributionData.providerUrl;
    }
    if (!m_lyrics.attributionUrl.isEmpty()) {
        return m_lyrics.attributionUrl;
    }
    return QString();
}

QVariantList LrcApplet::contributors() const
{
    if (isSpicyExpired()) {
        return QVariantList();
    }
    QVariantList list;
    for (const auto &c : m_lyrics.attributionData.contributors) {
        QVariantMap map;
        map.insert(QStringLiteral("role"), c.role);
        map.insert(QStringLiteral("name"), c.name);
        map.insert(QStringLiteral("url"), c.url);
        list.append(map);
    }
    return list;
}

QString LrcApplet::attributionHtml() const
{
    if (isSpicyExpired() || (!m_lyrics.isUsable() && m_lyrics.attributionData.isEmpty())) {
        return QString();
    }
    return SpicyLyrics::buildAttributionHtml(m_lyrics.attributionData);
}

QString LrcApplet::apiError() const
{
    if (!apiUnreachable()) {
        return QString();
    }
    if (m_spicy->lastErrorCode() == QLatin1String("application_suspended")) {
        return i18n("Spicy Lyrics application suspended; check the developer dashboard");
    }
    if (m_spicy->lastHttpStatus() == 401) {
        return i18n("Spicy Lyrics API key rejected");
    }
    if (m_spicy->lastHttpStatus() == 403) {
        return i18n("Spicy Lyrics access denied (HTTP 403)");
    }
    if (m_spicy->lastHttpStatus() == 429) {
        return i18n("Spicy Lyrics rate limit reached; try again later");
    }
    if (m_spicy->lastErrorCode() == QLatin1String("invalid_response")) {
        return i18n("Spicy Lyrics returned an invalid response");
    }
    return i18n("Lyrics service unreachable");
}

bool LrcApplet::isSpotify() const
{
    if (m_webPageSession) {
        return false;
    }
    return m_player.contains(QLatin1String("spotify"), Qt::CaseInsensitive)
        || m_watchedService.contains(QLatin1String("spotify"), Qt::CaseInsensitive)
        || !m_spotifyTrackId.isEmpty();
}

void LrcApplet::poll()
{
    if (!m_started) {
        return;
    }

    if (m_fromSpicy && isSpicyExpired()) {
        handleLyricsExpired();
        return;
    }

    if (m_pauseWhenIdle && m_playingKnown && !m_playing) {
        return;
    }

    considerPlayerSwitch();

    // If lyrics are already loaded and usable, or lookup has completed for this track, skip polling
    if (m_lyrics.isUsable() || m_lyricsLookupDone) {
        return;
    }

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

    if (m_fetchingFromSpicy || m_fetchingLrc || (m_process && m_process->state() != QProcess::NotRunning)) {
        return;
    }

    if (!m_trackTitle.isEmpty() && !m_lyrics.isUsable() && !m_lyricsLookupDone) {
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

    if (m_pauseWhenIdle && (!m_playingKnown || !m_playing) && !m_watchedService.isEmpty()) {
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

    // If the track changed while the process was running, discard output from the old track.
    if (!m_process || m_processGeneration != m_lookupGeneration) {
        return;
    }

    if (!m_available) {
        setAvailable(true);
        setError(QString());
        if (m_timer.interval() != m_pollInterval) {
            m_timer.start(m_pollInterval);
        }
    }

    // A track that the API timed better than lrc_tty does: ignore this answer.
    if (m_fromSpicy && m_lyrics.isUsable() && !isSpicyExpired()) {
        cancelProcess();
        return;
    }

    const QString output = QString::fromUtf8(m_process->readAllStandardOutput());
    const int exitCode = m_process->exitCode();
    const QProcess::ExitStatus exitStatus = m_process->exitStatus();

    Lyrics parsed;
    const qint64 durationMs = m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0;
    const bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0 && LrcParser::parseLrc(output, &parsed, durationMs));

    if (ok && parsed.isUsable()) {
        m_expiryTimer.stop();
        m_lyrics = parsed;
        m_fromSpicy = false;
        m_wordSynced = false;
        m_lineIndex = -1;
        m_lyricsLookupDone = true;
        cancelProcess();
        if (m_pauseHidden) {
            m_wordTimer.stop();
        } else {
            m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
            updateWord();
        }
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

    cancelProcess();
    m_lyricsLookupDone = true;
    setText(QString());
    setActive(false);
}

void LrcApplet::onProcessFailedToStart()
{
    const QString err = m_process ? m_process->errorString() : QStringLiteral("Failed to start process");
    qWarning() << "Process failed to start:" << err;
    cancelProcess();
    m_lyricsLookupDone = true;
    setError(err);
    if (QStandardPaths::findExecutable(QStringLiteral("lrc_tty")).isEmpty()) {
        if (!m_available) {
            return;
        }
        setAvailable(false);
        setText(QString());
        setActive(false);
        m_timer.start(kMissingBinaryRetryMs);
    }
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
    const bool wasPlaying = m_playingKnown && m_playing;
    if (m_playing == playing && m_playingKnown == known) {
        return;
    }

    if (wasPlaying && !playing && m_positionValid && m_positionClock.isValid()) {
        m_positionBaseMs += m_positionClock.elapsed();
        m_positionClock.invalidate();
    } else if (!wasPlaying && playing && m_positionValid) {
        m_positionClock.start();
    }

    m_playing = playing;
    m_playingKnown = known;
    Q_EMIT playingChanged();

    if (playing) {
        m_pauseTimer.stop();
        if (m_pauseHidden) {
            m_pauseHidden = false;
            Q_EMIT pauseHiddenChanged();
        }
        if (m_lyrics.isUsable() && m_positionValid) {
            updateWord();
        }
        poll();
    } else if (known && !playing) {
        if (wasPlaying) {
            // User paused active playback: give pauseHideDelay seconds grace period
            if (m_pauseHideDelay > 0 && !m_pauseHidden) {
                m_pauseTimer.start(m_pauseHideDelay * 1000);
            }
        } else {
            // Player started up or connected already paused: hide lyrics immediately if pauseHideDelay > 0
            if (m_pauseHideDelay > 0) {
                m_pauseTimer.stop();
                if (!m_pauseHidden) {
                    m_pauseHidden = true;
                    m_lineIndex = -1;
                    clearWordHighlight();
                    setText(QString());
                    setActive(false);
                    Q_EMIT pauseHiddenChanged();
                }
            }
        }
    } else if (!known) {
        m_pauseTimer.stop();
        if (m_pauseHidden) {
            m_pauseHidden = false;
            Q_EMIT pauseHiddenChanged();
        }
    }

    // Keep the timer running even when lyrics are not loaded yet so playhead
    // position is always tracked for accurate gapless transition detection.
    if (known && !playing) {
        if (!m_pauseHidden) {
            m_wordTimer.start(kIdleTickMs);
        } else {
            m_wordTimer.stop();
        }
    } else if (playing) {
        m_wordTimer.start(m_lyrics.isUsable() ? kWordTickMs : kIdleTickMs);
    }

    saveSyncState();
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
    m_expiryTimer.stop();
    m_lyrics = Lyrics();
    m_word.clear();
    m_wordStart = 0;
    m_wordEnd = 0;
    m_wordProgress = 0.0;
    m_wordSynced = false;
    m_fromSpicy = false;
    m_fetchingFromSpicy = false;
    m_fetchingLrc = false;
    m_lrclibKey.clear();
    m_lyricsLookupDone = false;
    m_missingReason = MissingReason::Unsynced;
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearBacking();
    if (m_playing && !m_pauseHidden) {
        m_wordTimer.start(kIdleTickMs);
    } else {
        m_wordTimer.stop();
    }
    setText(QString());
    setActive(false);
    Q_EMIT wordChanged();
    Q_EMIT lyricsChanged();
}

void LrcApplet::resetTrackPosition(bool isTrackTransition)
{
    m_expiryTimer.stop();
    m_expiryRetryCount = 0;
    clearBacking();
    if (m_positionWatcher) {
        delete m_positionWatcher;
        m_positionWatcher = nullptr;
    }
    m_positionMs = 0;
    m_positionBaseMs = 0;
    m_positionValid = false;
    m_positionClock.invalidate();
    m_positionRetries.invalidate();
    m_positionChecks.invalidate();
    m_positionFetchTimer.invalidate();
    m_prevTrackNaturalEndTimer.invalidate();
    m_trackJustChanged = isTrackTransition;
    m_trackChangeRetries = 0;
    m_zeroPositionPolls = 0;
    ++m_lookupGeneration;
}

namespace
{
QString syncStateFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    return dir + QStringLiteral("/plasma-lrc-sync-state.json");
}
} // namespace

void LrcApplet::saveSyncState() const
{
    if (m_watchedService.isEmpty() || (m_trackTitle.isEmpty() && m_mprisTrackId.isEmpty())) {
        return;
    }
    const qint64 currentPos = m_positionValid
        ? (m_positionBaseMs + (m_playing && m_positionClock.isValid() ? m_positionClock.elapsed() : 0))
        : -1;

    QJsonObject obj;
    obj.insert(QStringLiteral("service"), m_watchedService);
    obj.insert(QStringLiteral("trackId"), m_mprisTrackId);
    obj.insert(QStringLiteral("title"), m_trackTitle);
    obj.insert(QStringLiteral("artist"), m_trackArtist);
    obj.insert(QStringLiteral("lengthMs"), m_trackLengthUs > 0 ? (m_trackLengthUs / 1000) : 0);
    obj.insert(QStringLiteral("positionMs"), currentPos);
    obj.insert(QStringLiteral("gaplessLeadMs"), m_gaplessLeadMs);
    obj.insert(QStringLiteral("playing"), m_playing);
    obj.insert(QStringLiteral("timestampMs"), QDateTime::currentMSecsSinceEpoch());

    QFile file(syncStateFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

bool LrcApplet::restoreSyncState(const QString &rawTrackId, const QString &title, const QString &artist, qint64 *outGaplessLeadMs) const
{
    if (outGaplessLeadMs) {
        *outGaplessLeadMs = 0;
    }
    QFile file(syncStateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (obj.isEmpty()) {
        return false;
    }

    const QString savedService = obj.value(QStringLiteral("service")).toString();
    if (savedService != m_watchedService) {
        return false;
    }

    const qint64 savedTime = obj.value(QStringLiteral("timestampMs")).toVariant().toLongLong();
    const qint64 wallElapsedMs = QDateTime::currentMSecsSinceEpoch() - savedTime;
    if (wallElapsedMs < 0 || wallElapsedMs > 20000) {
        return false;
    }

    const bool savedPlaying = obj.value(QStringLiteral("playing")).toBool();
    if (!savedPlaying || !m_playing) {
        return false;
    }

    const QString savedTrackId = obj.value(QStringLiteral("trackId")).toString();
    const QString savedTitle = obj.value(QStringLiteral("title")).toString();
    const QString savedArtist = obj.value(QStringLiteral("artist")).toString();
    const bool sameTrack = (!rawTrackId.isEmpty() && rawTrackId == savedTrackId)
        || (!title.isEmpty() && title == savedTitle && artist == savedArtist);

    if (sameTrack) {
        if (outGaplessLeadMs) {
            *outGaplessLeadMs = obj.value(QStringLiteral("gaplessLeadMs")).toVariant().toLongLong();
        }
        return true;
    }

    // Different track: check if the previous track naturally finished while plasmashell was restarting!
    const qint64 savedLengthMs = obj.value(QStringLiteral("lengthMs")).toVariant().toLongLong();
    const qint64 savedPosMs = obj.value(QStringLiteral("positionMs")).toVariant().toLongLong();
    if (savedLengthMs > 0 && savedPosMs >= 0) {
        const qint64 estimatedOldPosMs = savedPosMs + wallElapsedMs;
        const qint64 oldRemainingMs = savedLengthMs - estimatedOldPosMs;
        if (oldRemainingMs > 0 && oldRemainingMs <= 2500) {
            if (outGaplessLeadMs) {
                *outGaplessLeadMs = qMin(oldRemainingMs, kSpotifyGaplessLeadMs);
            }
            return true;
        }
    }
    return false;
}

void LrcApplet::cancelProcess()
{
    m_watchdog.stop();
    m_fetchingLrc = false;

    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            connect(m_process, &QProcess::finished, m_process, &QObject::deleteLater);
            m_process->kill();
        } else {
            m_process->deleteLater();
        }
        m_process = nullptr;
    }
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
    cancelProcess();
    m_pipeline->cancel();
    m_activeProviderId.clear();
    m_spicy->cancel();
    m_lrclib->cancel();
    m_expiryTimer.stop();
    m_expiryRetryCount = 0;

    resetTrackPosition(false);
    clearLyrics();
    clearBacking();
    m_wordTimer.stop();

    m_spotifyTrackId.clear();
    m_mprisTrackId.clear();
    m_lrclibKey.clear();
    m_lyricsLookupDone = false;
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
    m_gaplessLeadMs = 0;
    m_manualSkipPending = false;
    m_pauseTimer.stop();
    const bool shouldBeHidden = (m_playingKnown && !m_playing && m_pauseHideDelay > 0);
    if (m_pauseHidden != shouldBeHidden) {
        m_pauseHidden = shouldBeHidden;
        Q_EMIT pauseHiddenChanged();
    }
}

void LrcApplet::onPauseTimeout()
{
    if (m_playingKnown && !m_playing) {
        m_pauseHidden = true;
        m_lineIndex = -1;
        m_wordTimer.stop();
        clearWordHighlight();
        clearBacking();
        setText(QString());
        setActive(false);
        Q_EMIT pauseHiddenChanged();
    }
}

void LrcApplet::armExpiryTimer()
{
    m_expiryTimer.stop();
    if (!m_fromSpicy) {
        return;
    }
    if (m_lyrics.validUntilMs <= 0) {
        m_expiryTimer.start(0);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 remainingMs = m_lyrics.validUntilMs - now;
    if (remainingMs <= 0) {
        m_expiryTimer.start(0);
        return;
    }
    constexpr qint64 maxInt = std::numeric_limits<int>::max();
    const int interval = static_cast<int>(std::clamp<qint64>(remainingMs, 0, maxInt));
    m_expiryTimer.start(interval);
}

void LrcApplet::onLyricsExpired()
{
    if (!m_fromSpicy || m_expiryGeneration != m_lookupGeneration) {
        return;
    }
    if (m_lyrics.validUntilMs > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now < m_lyrics.validUntilMs) {
            armExpiryTimer();
            return;
        }
    }
    handleLyricsExpired();
}

void LrcApplet::handleLyricsExpired()
{
    if (!m_fromSpicy || m_expiryGeneration != m_lookupGeneration) {
        return;
    }

    m_expiryTimer.stop();

    // Discard active lyrics and cached full/current lyric strings
    m_lyrics = Lyrics();
    m_fromSpicy = false;
    m_wordSynced = false;
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearWordHighlight();
    clearBacking();
    m_word.clear();
    m_wordStart = 0;
    m_wordEnd = 0;
    m_wordProgress = 0.0;
    setText(QString());
    setActive(false);
    Q_EMIT wordChanged();
    Q_EMIT lyricsChanged();

    // Refresh current track through normal source lookup or fallback without request loops
    if (m_watchedService.isEmpty() || m_trackTitle.isEmpty()) {
        return;
    }

    m_lyricsLookupDone = false;
    if (m_expiryRetryCount < 1) {
        m_expiryRetryCount++;
        requestLyrics();
    } else {
        m_fetchingFromSpicy = false;
        m_fetchingLrc = true;
        m_lrclibKey = m_lrclib->request(m_trackTitle, m_trackArtist, m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0, m_webPageSession);
    }
}

void LrcApplet::togglePlayPause()
{
    if (m_watchedService.isEmpty()) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(m_watchedService, s_playerPath, s_playerInterface, QStringLiteral("PlayPause"));
    QDBusConnection::sessionBus().send(msg);
}

void LrcApplet::nextTrack()
{
    m_manualSkipPending = true;
    m_gaplessLeadMs = 0;
    cancelProcess();
    m_pipeline->cancel();
    m_activeProviderId.clear();
    m_spicy->cancel();
    m_lrclib->cancel();
    resetTrackPosition(true);
    clearLyrics();
    if (m_watchedService.isEmpty()) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(m_watchedService, s_playerPath, s_playerInterface, QStringLiteral("Next"));
    QDBusConnection::sessionBus().send(msg);
}

void LrcApplet::previousTrack()
{
    m_manualSkipPending = true;
    m_gaplessLeadMs = 0;
    cancelProcess();
    m_pipeline->cancel();
    m_activeProviderId.clear();
    m_spicy->cancel();
    m_lrclib->cancel();
    resetTrackPosition(true);
    clearLyrics();
    if (m_watchedService.isEmpty()) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(m_watchedService, s_playerPath, s_playerInterface, QStringLiteral("Previous"));
    QDBusConnection::sessionBus().send(msg);
}

bool LrcApplet::isTrackBlacklisted(const QString &artist, const QString &title) const
{
    if (title.isEmpty()) {
        return false;
    }
    const QString fullTrack = normalizeBlacklistEntry(artist.isEmpty() ? title : (artist + QStringLiteral(" - ") + title));
    const QString normTitle = normalizeBlacklistEntry(title);

    const QStringList entries = m_blacklistedTracks.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QString item = normalizeBlacklistEntry(entry);
        if (item.isEmpty()) {
            continue;
        }
        if (fullTrack == item || normTitle == item || fullTrack.contains(item) || normTitle.contains(item)) {
            return true;
        }
    }
    return false;
}

bool LrcApplet::isCurrentTrackBlacklisted() const
{
    return isTrackBlacklisted(m_trackArtist, m_trackTitle);
}

void LrcApplet::toggleBlacklistCurrentTrack()
{
    if (m_trackTitle.isEmpty()) {
        return;
    }

    const QString trackKey = m_trackArtist.isEmpty()
        ? m_trackTitle.trimmed()
        : (m_trackArtist.trimmed() + QStringLiteral(" - ") + m_trackTitle.trimmed());

    QStringList entries = m_blacklistedTracks.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (int i = 0; i < entries.size(); ++i) {
        entries[i] = entries[i].trimmed();
    }

    const QString fullTrackNorm = normalizeBlacklistEntry(trackKey);
    const QString titleNorm = normalizeBlacklistEntry(m_trackTitle);

    bool removed = false;
    for (int i = entries.size() - 1; i >= 0; --i) {
        const QString item = normalizeBlacklistEntry(entries.at(i));
        if (item.isEmpty()) {
            continue;
        }
        if (fullTrackNorm == item || titleNorm == item || fullTrackNorm.contains(item) || titleNorm.contains(item)) {
            entries.removeAt(i);
            removed = true;
        }
    }

    if (removed) {
        // Unblacklist
        m_blacklistedTracks = entries.join(QStringLiteral("; "));
        saveBlacklist();
        // Reload lyrics
        m_lyricsLookupDone = false;
        requestLyrics();
    } else {
        // Blacklist
        entries.append(trackKey);
        m_blacklistedTracks = entries.join(QStringLiteral("; "));
        saveBlacklist();
        // Immediately drop lyrics from display
        cancelProcess();
        clearLyrics();
        m_lyricsLookupDone = true;
    }
}

void LrcApplet::saveBlacklist()
{
    config().group(s_configGroup).writeEntry(QStringLiteral("blacklistedTracks"), m_blacklistedTracks);
    config().sync();
    if (configuration()) {
        configuration()->insert(QStringLiteral("blacklistedTracks"), m_blacklistedTracks);
    }
    Q_EMIT trackBlacklistChanged();
    Q_EMIT settingsChanged();
}

QStringList LrcApplet::blacklistedTrackList() const
{
    QStringList result;
    const QStringList entries = m_blacklistedTracks.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty() && !result.contains(trimmed, Qt::CaseInsensitive)) {
            result.append(trimmed);
        }
    }
    return result;
}

void LrcApplet::addBlacklistTrack(const QString &track)
{
    const QString trimmed = track.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    QStringList entries = m_blacklistedTracks.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (int i = 0; i < entries.size(); ++i) {
        entries[i] = entries[i].trimmed();
        if (entries.at(i).compare(trimmed, Qt::CaseInsensitive) == 0) {
            return;
        }
    }

    entries.append(trimmed);
    m_blacklistedTracks = entries.join(QStringLiteral("; "));
    saveBlacklist();

    if (isCurrentTrackBlacklisted()) {
        cancelProcess();
        clearLyrics();
        m_lyricsLookupDone = true;
    }
}

void LrcApplet::removeBlacklistTrack(const QString &track)
{
    const QString trimmed = track.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    QStringList entries = m_blacklistedTracks.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    int removed = 0;
    for (int i = entries.size() - 1; i >= 0; --i) {
        if (entries.at(i).trimmed().compare(trimmed, Qt::CaseInsensitive) == 0) {
            entries.removeAt(i);
            ++removed;
        }
    }

    if (removed > 0) {
        m_blacklistedTracks = entries.join(QStringLiteral("; "));
        saveBlacklist();

        if (!isCurrentTrackBlacklisted()) {
            m_lyricsLookupDone = false;
            requestLyrics();
        }
    }
}

void LrcApplet::clearBlacklist()
{
    if (m_blacklistedTracks.isEmpty()) {
        return;
    }

    m_blacklistedTracks.clear();
    saveBlacklist();

    m_lyricsLookupDone = false;
    requestLyrics();
}

void LrcApplet::applyMetadata(const QVariantMap &metadata)
{
    const QStringList artists = trackArtists(metadata.value(QStringLiteral("xesam:artist")));
    const QString title = metadata.value(QStringLiteral("xesam:title")).toString();
    const QString artist = artists.join(QStringLiteral(", "));
    const QString rawTrackId = mprisTrackId(metadata.value(QStringLiteral("mpris:trackid")));
    const QString trackId = SpicyLyrics::trackIdFromMpris(rawTrackId);
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

    const bool hadPreviousTrack = !m_trackTitle.isEmpty() || !m_mprisTrackId.isEmpty();
    const bool trackChanged = (title != m_trackTitle || artist != m_trackArtist
        || (!trackId.isEmpty() && trackId != m_spotifyTrackId)
        || (!rawTrackId.isEmpty() && rawTrackId != m_mprisTrackId));

    if (trackChanged) {
        qint64 gaplessLead = 0;
        const bool isWebSession = looksLikeWebPageSession(rawTrackId, m_player);
        const bool isSpotifyPlayer = !isWebSession && (isSpotify() || !trackId.isEmpty());

        if (!hadPreviousTrack) {
            if (isSpotifyPlayer) {
                restoreSyncState(rawTrackId, title, artist, &gaplessLead);
            }
        } else if (isSpotifyPlayer && m_playing && !m_manualSkipPending) {
            const bool recentManualSeek = m_recentSeekTimer.isValid() && m_recentSeekTimer.elapsed() < 500;
            if (!recentManualSeek) {
                const bool naturalEndJustDetected = m_prevTrackNaturalEndTimer.isValid()
                    && m_prevTrackNaturalEndTimer.elapsed() <= 2000;
                bool nearEnd = false;
                qint64 remainingMs = 0;
                if (m_positionValid && m_trackLengthUs > 0) {
                    const qint64 currentPos = m_positionBaseMs + (m_positionClock.isValid() ? m_positionClock.elapsed() : 0);
                    remainingMs = (m_trackLengthUs / 1000) - currentPos;
                    nearEnd = (remainingMs > 0 && remainingMs <= 2500);
                }
                if (naturalEndJustDetected || nearEnd) {
                    if (remainingMs > 0) {
                        gaplessLead = qMin(remainingMs, kSpotifyGaplessLeadMs);
                    } else if (naturalEndJustDetected) {
                        gaplessLead = kSpotifyGaplessLeadMs;
                    }
                }
            }
        }
        m_manualSkipPending = false;

        cancelProcess();
        m_pipeline->cancel();
        m_activeProviderId.clear();
        m_spicy->cancel();
        m_lrclib->cancel();
        m_expiryTimer.stop();
        resetTrackPosition(hadPreviousTrack);
        clearLyrics();
        m_gaplessLeadMs = gaplessLead;

        setTrackInfo(trackInfo);
        m_webPageSession = isWebSession;
        m_trackArtist = artist;
        m_trackTitle = title;
        m_trackAlbum = metadata.value(QStringLiteral("xesam:album")).toString();
        m_trackUrl = metadata.value(QStringLiteral("xesam:url")).toString();
        m_trackLengthUs = lengthUs;
        m_spotifyTrackId = trackId;
        m_mprisTrackId = rawTrackId;

        if (m_playingKnown && !m_playing && m_pauseHideDelay > 0) {
            m_pauseHidden = true;
            m_pauseTimer.stop();
        }

        saveSyncState();
        Q_EMIT trackBlacklistChanged();

        requestPosition();
        requestLyrics();
    } else {
        setTrackInfo(trackInfo);
        m_trackAlbum = metadata.value(QStringLiteral("xesam:album")).toString();
        m_trackUrl = metadata.value(QStringLiteral("xesam:url")).toString();
        m_trackLengthUs = lengthUs;
        if (!m_lyrics.isUsable() && !m_lyricsLookupDone && !m_fetchingFromSpicy && !m_fetchingLrc) {
            requestLyrics();
        }
    }
}

void LrcApplet::requestLyrics()
{
    if (m_watchedService.isEmpty() || m_trackTitle.isEmpty() || m_lyricsLookupDone) {
        return;
    }

    if (isCurrentTrackBlacklisted()) {
        m_lyricsLookupDone = true;
        clearLyrics();
        return;
    }

    cancelProcess();
    m_pipeline->cancel();
    m_expiryTimer.stop();

    TrackMetadata meta;
    meta.title = m_trackTitle;
    meta.artist = m_trackArtist;
    meta.album = m_trackAlbum;
    meta.durationMs = m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0;
    meta.spotifyTrackId = m_spotifyTrackId;
    meta.mprisTrackId = m_mprisTrackId;
    meta.url = m_trackUrl;
    meta.isWebSession = m_webPageSession;

    TrackMetadata searchMeta = meta;
    normalizeTrackMetadata(&searchMeta);

    m_fetchingFromSpicy = false;
    m_fetchingLrc = true;

    m_pipeline->requestLyrics(searchMeta, ++m_lookupGeneration);
}

void LrcApplet::onPipelineResolved(quint64 generation, const Lyrics &lyrics, const QString &providerId)
{
    if (generation != m_lookupGeneration) {
        return;
    }

    m_activeProviderId = providerId;
    m_fromSpicy = (providerId == QLatin1String("spicy"));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool isInvalidOrExpired = m_fromSpicy && (lyrics.validUntilMs <= 0 || now >= lyrics.validUntilMs);

    if (isInvalidOrExpired) {
        m_expiryTimer.stop();
        m_lyrics = Lyrics();
        m_fromSpicy = false;
        m_wordSynced = false;
        m_lineIndex = -1;
        m_wordCursor = 0;
        clearWordHighlight();
        m_word.clear();
        m_wordStart = 0;
        m_wordEnd = 0;
        m_wordProgress = 0.0;
        setText(QString());
        setActive(false);
        Q_EMIT wordChanged();
        Q_EMIT lyricsChanged();
        return;
    }

    if (!m_available) {
        setAvailable(true);
        setError(QString());
    }

    // Syllable lyrics cannot be upgraded further; provisional Line lyrics keep fetching in flight.
    if (lyrics.type == LyricsType::Syllable) {
        m_fetchingLrc = false;
        m_fetchingFromSpicy = false;
        m_lyricsLookupDone = true;
    }

    m_lyrics = lyrics;
    m_wordSynced = (lyrics.type == LyricsType::Syllable);
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearWordHighlight();

    if (m_fromSpicy) {
        m_expiryGeneration = m_lookupGeneration;
        m_expiryRetryCount = 0;
        if (m_lyrics.validUntilMs > 0) {
            armExpiryTimer();
        } else {
            m_expiryTimer.stop();
        }
    } else {
        m_expiryTimer.stop();
    }

    requestPosition();
    if (m_pauseHidden) {
        m_wordTimer.stop();
    } else {
        m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
        updateWord();
    }
    Q_EMIT lyricsChanged();
}

void LrcApplet::onPipelineLookupFinished(quint64 generation)
{
    if (generation != m_lookupGeneration) {
        return;
    }

    m_fetchingLrc = false;
    m_fetchingFromSpicy = false;
    m_lyricsLookupDone = true;
}

void LrcApplet::onPipelineAllFailed(quint64 generation, MissingReason lastReason)
{
    if (generation != m_lookupGeneration) {
        return;
    }

    m_fetchingLrc = false;
    m_fetchingFromSpicy = false;
    m_activeProviderId.clear();
    m_missingReason = lastReason;

    // Safely clear lyrics and attribution/expiry state without changing generation
    m_expiryTimer.stop();
    m_lyrics = Lyrics();
    m_word.clear();
    m_wordStart = 0;
    m_wordEnd = 0;
    m_wordProgress = 0.0;
    m_wordSynced = false;
    m_fromSpicy = false;
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearWordHighlight();
    setText(QString());
    setActive(false);
    Q_EMIT wordChanged();
    Q_EMIT lyricsChanged();

    if (!QStandardPaths::findExecutable(QStringLiteral("lrc_tty")).isEmpty()) {
        fetchLrcLyrics(false);
    } else {
        m_lyricsLookupDone = true;
        if (m_playing && !m_pauseHidden) {
            m_wordTimer.start(kIdleTickMs);
        } else {
            m_wordTimer.stop();
        }
    }
}

QStringList LrcApplet::providerOrder() const
{
    return m_pipeline ? m_pipeline->providerOrder() : QStringList();
}

QVariantList LrcApplet::providers() const
{
    QVariantList result;
    if (!m_pipeline) {
        return result;
    }
    const QStringList order = m_pipeline->providerOrder();
    for (const QString &id : order) {
        LyricsProvider *p = m_pipeline->provider(id);
        if (!p) {
            continue;
        }
        QVariantMap map;
        map.insert(QStringLiteral("id"), p->id());
        map.insert(QStringLiteral("name"), p->displayName());
        map.insert(QStringLiteral("enabled"), p->isEnabled());
        map.insert(QStringLiteral("isConfigured"), p->isConfigured());
        map.insert(QStringLiteral("wordSync"), bool(p->capabilities() & LyricsProvider::Capability::WordSync));
        map.insert(QStringLiteral("lineSync"), bool(p->capabilities() & LyricsProvider::Capability::LineSync));
        map.insert(QStringLiteral("requiresKey"), bool(p->capabilities() & LyricsProvider::Capability::RequiresAuthKey));
        map.insert(QStringLiteral("requiresSpotifyId"), bool(p->capabilities() & LyricsProvider::Capability::RequiresSpotifyId));
        result.append(map);
    }
    return result;
}

void LrcApplet::setProviderEnabled(const QString &id, bool enabled)
{
    if (!m_pipeline) {
        return;
    }
    m_pipeline->setProviderEnabled(id, enabled);
    if (id == QLatin1String("spicy")) {
        m_useSpicy = enabled;
        config().group(s_configGroup).writeEntry(QStringLiteral("useSpicyLyrics"), enabled);
    }
    saveProviderConfig();
    if (m_started && !m_trackTitle.isEmpty()) {
        m_lyricsLookupDone = false;
        requestLyrics();
    }
}

void LrcApplet::moveProviderUp(int index)
{
    if (!m_pipeline) {
        return;
    }
    QStringList order = m_pipeline->providerOrder();
    if (index > 0 && index < order.size()) {
        order.swapItemsAt(index, index - 1);
        m_pipeline->setProviderOrder(order);
        saveProviderConfig();
        if (m_started && !m_trackTitle.isEmpty()) {
            m_lyricsLookupDone = false;
            requestLyrics();
        }
    }
}

void LrcApplet::moveProviderDown(int index)
{
    if (!m_pipeline) {
        return;
    }
    QStringList order = m_pipeline->providerOrder();
    if (index >= 0 && index < order.size() - 1) {
        order.swapItemsAt(index, index + 1);
        m_pipeline->setProviderOrder(order);
        saveProviderConfig();
        if (m_started && !m_trackTitle.isEmpty()) {
            m_lyricsLookupDone = false;
            requestLyrics();
        }
    }
}

void LrcApplet::saveProviderConfig()
{
    const QString orderStr = m_pipeline->providerOrder().join(QLatin1Char(','));
    m_providerOrderConfig = orderStr;
    config().group(s_configGroup).writeEntry(QStringLiteral("providerOrder"), orderStr);
    QStringList disabledList;
    for (LyricsProvider *p : m_pipeline->allProviders()) {
        if (!p->isEnabled()) {
            disabledList.append(p->id());
        }
    }
    const QString disabledStr = disabledList.join(QLatin1Char(','));
    m_disabledProvidersConfig = disabledStr;
    config().group(s_configGroup).writeEntry(QStringLiteral("disabledProviders"), disabledStr);
    config().sync();
    if (configuration()) {
        configuration()->insert(QStringLiteral("providerOrder"), orderStr);
        configuration()->insert(QStringLiteral("disabledProviders"), disabledStr);
    }
    Q_EMIT providerOrderChanged();
    Q_EMIT settingsChanged();
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

    cancelProcess();

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
        if (!cleanedTitle.isEmpty()) {
            if (!cleanedArtist.isEmpty()) {
                arguments << QStringLiteral("--artist") << cleanedArtist;
            }
            arguments << QStringLiteral("--title") << cleanedTitle;
            m_lastLookupWasCleaned = (cleanedTitle != m_trackTitle || cleanedArtist != m_trackArtist);
        } else {
            m_lastLookupWasCleaned = false;
        }
    } else if (rawFallback && trackBelongsToPlayer) {
        if (!m_trackTitle.isEmpty()) {
            arguments << QStringLiteral("--title") << m_trackTitle;
        }
        if (!m_trackArtist.isEmpty()) {
            arguments << QStringLiteral("--artist") << m_trackArtist;
        }
        m_lastLookupWasCleaned = false;
    } else {
        m_lastLookupWasCleaned = false;
    }

    if (trackBelongsToPlayer && m_trackLengthUs > 0) {
        arguments << QStringLiteral("--duration") << QString::number(m_trackLengthUs / 1000000);
    }

    m_fetchingLrc = true;
    m_processGeneration = ++m_lookupGeneration;
    m_watchdog.start(kWatchdogTimeoutMs);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::finished, this, [this] {
        onProcessFinished();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            onProcessFailedToStart();
        }
    });
    connect(m_process, &QProcess::stateChanged, this, [this] {
        if (m_process && m_process->state() == QProcess::NotRunning) {
            m_watchdog.stop();
        }
    });
    QString execPath = QStringLiteral("lrc_tty");
    const QString resolvedExec = QStandardPaths::findExecutable(execPath);
    if (!resolvedExec.isEmpty()) {
        execPath = resolvedExec;
    }
    m_process->start(execPath, arguments);
}

void LrcApplet::onSpicyLoaded(const QString &trackId, const Lyrics &lyrics)
{
    if (!m_fetchingFromSpicy || trackId != m_spotifyTrackId) {
        return;
    }
    m_fetchingFromSpicy = false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool isInvalidOrExpired = (lyrics.validUntilMs <= 0 || now >= lyrics.validUntilMs);

    if (isInvalidOrExpired) {
        // Reject missing or expired deadline before exposing API content
        m_expiryTimer.stop();
        m_lyrics = Lyrics();
        m_fromSpicy = false;
        m_wordSynced = false;
        m_lineIndex = -1;
        m_wordCursor = 0;
        clearWordHighlight();
        m_word.clear();
        m_wordStart = 0;
        m_wordEnd = 0;
        m_wordProgress = 0.0;
        setText(QString());
        setActive(false);
        Q_EMIT wordChanged();
        Q_EMIT lyricsChanged();

        // At most normal fallback if current track/player metadata allows, preserving retry count without loops
        if (!m_watchedService.isEmpty() && !m_trackTitle.isEmpty()) {
            m_lyricsLookupDone = false;
            m_fetchingLrc = true;
            m_lrclibKey = m_lrclib->request(m_trackTitle, m_trackArtist, m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0, m_webPageSession);
        } else {
            m_fetchingLrc = false;
            m_lyricsLookupDone = true;
        }
        return;
    }

    if (!m_available) {
        setAvailable(true);
        setError(QString());
    }

    m_lyricsLookupDone = true;
    m_lyrics = lyrics;
    m_fromSpicy = true;
    m_wordSynced = lyrics.type == LyricsType::Syllable;
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearWordHighlight();

    m_expiryGeneration = m_lookupGeneration;
    m_expiryRetryCount = 0;

    armExpiryTimer();

    requestPosition();
    if (m_pauseHidden) {
        m_wordTimer.stop();
    } else {
        m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
        updateWord();
    }
    Q_EMIT lyricsChanged();
}

void LrcApplet::onSpicyMissing(const QString &trackId, MissingReason reason)
{
    if (!m_fetchingFromSpicy || trackId != m_spotifyTrackId) {
        return;
    }
    m_expiryTimer.stop();
    m_fetchingFromSpicy = false;
    m_missingReason = reason;
    Q_EMIT lyricsChanged();

    // Fall back to native LRCLIB client if current track/player metadata allows
    if (!m_watchedService.isEmpty() && !m_trackTitle.isEmpty()) {
        m_lyricsLookupDone = false;
        m_fetchingLrc = true;
        m_lrclibKey = m_lrclib->request(m_trackTitle, m_trackArtist, m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0, m_webPageSession);
    } else {
        m_fetchingLrc = false;
        m_lyricsLookupDone = true;
    }
}

void LrcApplet::onLrclibLoaded(const QString &key, const Lyrics &lyrics)
{
    if (!m_fetchingLrc || key != m_lrclibKey) {
        return;
    }
    m_expiryTimer.stop();
    m_fetchingLrc = false;
    m_lyricsLookupDone = true;
    m_lyrics = lyrics;
    m_fromSpicy = false;
    m_wordSynced = false;
    m_lineIndex = -1;
    m_wordCursor = 0;
    clearWordHighlight();

    if (!m_available) {
        setAvailable(true);
        setError(QString());
    }

    requestPosition();
    if (m_pauseHidden) {
        m_wordTimer.stop();
    } else {
        m_wordTimer.start(m_playing ? kWordTickMs : kIdleTickMs);
        updateWord();
    }
    Q_EMIT lyricsChanged();
}

void LrcApplet::onLrclibMissing(const QString &key)
{
    if (!m_fetchingLrc || key != m_lrclibKey) {
        return;
    }
    m_fetchingLrc = false;
    m_lyricsLookupDone = true;
    if (m_playing && !m_pauseHidden) {
        m_wordTimer.start(kIdleTickMs);
    }
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
    m_positionFetchTimer.start();

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

    const qint64 rttMs = m_positionFetchTimer.isValid() ? m_positionFetchTimer.elapsed() : 0;
    const qint64 rttComp = m_playing ? qBound(0LL, rttMs / 2, 50LL) : 0LL;
    const qint64 reportedMs = (positionUs / 1000) + rttComp;

    // Single unified stale-position guard after track change:
    // 1) reportedMs exceeds the new track's length by > 1s, OR
    // 2) track just transitioned and player playhead hasn't reset to the start (< 2.5s) yet.
    const bool exceedsDuration = (m_trackLengthUs > 0 && reportedMs > (m_trackLengthUs / 1000) + 1000);
    const bool staleAfterTrackChange = (m_trackJustChanged && reportedMs > 2500);
    if (exceedsDuration || staleAfterTrackChange) {
        if (++m_trackChangeRetries <= 16) {
            QTimer::singleShot(50, this, [this] {
                m_positionRetries.invalidate();
                requestPosition();
            });
            return;
        }
        // Retries exhausted (e.g. user genuinely started or seeked mid-track):
        // clear gapless lead since we are not at the gapless transition boundary.
        m_gaplessLeadMs = 0;
    }

    // Guard against the race where Spotify's audio thread resets Position to ~0ms
    // at the end of a finishing track a few milliseconds BEFORE PropertiesChanged(Metadata)
    // for the next track arrives on D-Bus.
    if (!m_trackJustChanged && m_playing && m_positionValid && m_trackLengthUs > 0) {
        const qint64 expected = m_positionBaseMs + (m_positionClock.isValid() ? m_positionClock.elapsed() : 0);
        const qint64 remainingMs = (m_trackLengthUs / 1000) - expected;
        const bool recentSeek = m_recentSeekTimer.isValid() && m_recentSeekTimer.elapsed() < 500;
        if (!recentSeek && remainingMs >= -5000 && remainingMs <= 3500 && reportedMs < 2000 && expected > 5000) {
            if (!m_prevTrackNaturalEndTimer.isValid()) {
                m_prevTrackNaturalEndTimer.start();
            }
            // Wait up to 250ms for PropertiesChanged(Metadata) of the next track to arrive
            // before accepting the wrapped 0ms playhead (in case of Repeat-One).
            if (m_prevTrackNaturalEndTimer.elapsed() < 250) {
                QTimer::singleShot(50, this, [this] {
                    m_positionChecks.invalidate();
                    requestPosition();
                });
                return;
            }
            // If Metadata never followed, only an enabled repeat at the actual
            // end boundary can justify a same-track gapless compensation.
            m_prevTrackNaturalEndTimer.invalidate();
            if (m_repeatTrack && remainingMs <= 250) {
                m_gaplessLeadMs = isSpotify() ? kSpotifyGaplessLeadMs : 0;
                m_positionBaseMs = reportedMs;
                m_positionClock.start();
                m_positionValid = true;
                m_zeroPositionPolls = 0;
                m_lineIndex = -1;
                m_wordCursor = 0;
                clearWordHighlight();
                m_positionRetries.start();
                m_positionChecks.start();
                saveSyncState();
                if (m_lyrics.isUsable() && !m_pauseHidden) {
                    updateWord();
                }
                return;
            }
        }
    }

    // Spotify buffering / loading detection:
    // In Spotify's MPRIS implementation, Position is extrapolated as:
    //   pos_ms = state->position_as_of_timestamp_ms + (now - state->timestamp_ms) * state->playback_speed
    // While a newly started, skipped, or Autoplay track is buffering, PlaybackStatus is "Playing",
    // position_as_of_timestamp_ms == 0, and playback_speed == 0.0f, so Get("Position") returns
    // exact 0 (positionUs == 0) for 150-2500ms without emitting Seeked when playback_speed becomes 1.0f.
    // By contrast, a true gapless transition already pre-buffered the track in RAM and has positionUs > 0.
    if (m_playing && isSpotify() && positionUs == 0 && m_positionBaseMs == 0) {
        ++m_zeroPositionPolls;
        if (m_zeroPositionPolls >= 3) {
            // Stationary at 0 for >= 120ms: track is buffering from 0, NOT playing from gapless pre-buffer.
            m_gaplessLeadMs = 0;
        }
        if (m_zeroPositionPolls <= 45) {
            m_positionBaseMs = 0;
            m_positionClock.invalidate();
            m_trackChangeRetries = 0;
            m_positionValid = true;
            m_positionRetries.start();
            m_positionChecks.start();
            saveSyncState();
            if (m_lyrics.isUsable() && !m_pauseHidden) {
                updateWord();
            }
            QTimer::singleShot(60, this, [this] {
                if (m_playing && m_positionBaseMs == 0) {
                    m_positionRetries.invalidate();
                    requestPosition();
                }
            });
            return;
        }
    }
    m_zeroPositionPolls = 0;

    const bool jumped = positionJumped(reportedMs);
    if (jumped) {
        m_gaplessLeadMs = 0;
        m_wordCursor = 0;
        clearWordHighlight();
        if (m_pauseHidden) {
            m_pauseHidden = false;
            Q_EMIT pauseHiddenChanged();
            if (m_playingKnown && !m_playing && m_pauseHideDelay > 0) {
                m_pauseTimer.start(m_pauseHideDelay * 1000);
            }
        }
    }

    m_positionBaseMs = reportedMs;
    if (m_playing) {
        m_positionClock.start();
    } else {
        m_positionClock.invalidate();
    }
    m_trackJustChanged = false;
    m_trackChangeRetries = 0;
    m_positionValid = true;
    m_positionRetries.start();
    m_positionChecks.start();
    saveSyncState();

    if (m_lyrics.isUsable() && !m_pauseHidden) {
        updateWord();
    }
}

void LrcApplet::onPlayerSeeked(qint64 positionUs)
{
    if (m_watchedService.isEmpty() || positionUs < 0) {
        return;
    }

    if (m_positionWatcher) {
        delete m_positionWatcher;
        m_positionWatcher = nullptr;
    }

    m_zeroPositionPolls = 0;
    const qint64 seekMs = positionUs / 1000;

    // Check if this is a Repeat-One natural wrap at the end of a track
    if (m_repeatTrack && !m_trackJustChanged && m_playing && m_positionValid && m_trackLengthUs > 0) {
        const qint64 expected = m_positionBaseMs + (m_positionClock.isValid() ? m_positionClock.elapsed() : 0);
        const qint64 remainingMs = (m_trackLengthUs / 1000) - expected;
        // A seek near the end alone is not evidence of repeat-one. Require the
        // player's loop setting and an actual end boundary before compensating.
        if (remainingMs >= -5000 && remainingMs <= 250 && seekMs < 2000 && expected > 5000) {
            m_prevTrackNaturalEndTimer.start();
            m_gaplessLeadMs = isSpotify() ? kSpotifyGaplessLeadMs : 0;
            m_positionBaseMs = seekMs;
            m_positionClock.start();
            m_positionValid = true;
            m_lineIndex = -1;
            m_wordCursor = 0;
            clearWordHighlight();
            m_positionRetries.start();
            m_positionChecks.start();
            saveSyncState();
            if (m_lyrics.isUsable() && !m_pauseHidden) {
                updateWord();
            }
            return;
        }
    }

    // Spotify emits Seeked whenever an internal PlayerState update differs from its
    // extrapolated position by even 1ms. Only treat Seeked as a real manual seek
    // (wiping m_gaplessLeadMs) when the position actually jumped!
    const bool isRealJump = (!m_positionValid || positionJumped(seekMs))
        && !(m_trackJustChanged && seekMs < 2500);

    if (isRealJump) {
        m_recentSeekTimer.start();
        m_prevTrackNaturalEndTimer.invalidate();
        m_gaplessLeadMs = 0;
        m_wordCursor = 0;
        clearWordHighlight();
    }

    m_positionBaseMs = seekMs;
    if (m_playing) {
        m_positionClock.start();
    } else {
        m_positionClock.invalidate();
    }
    m_trackJustChanged = false;
    m_trackChangeRetries = 0;
    m_positionValid = true;
    m_positionRetries.start();
    m_positionChecks.start();

    if (m_pauseHidden) {
        m_pauseHidden = false;
        Q_EMIT pauseHiddenChanged();
        if (m_playingKnown && !m_playing && m_pauseHideDelay > 0) {
            m_pauseTimer.start(m_pauseHideDelay * 1000);
        }
    }

    saveSyncState();
    if (m_lyrics.isUsable() && !m_pauseHidden) {
        updateWord();
    }
}

/// True when the answer differs from where we thought playback was by more than a
/// seek is expected to move it.
bool LrcApplet::positionJumped(qint64 reportedMs) const
{
    // Nothing to compare against yet: the first reading of a track is not a jump.
    if (!m_positionValid) {
        return false;
    }

    const qint64 expected = m_positionBaseMs + (m_playing && m_positionClock.isValid() ? m_positionClock.elapsed() : 0);
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

void LrcApplet::setBackingState(const QString &text, int start, int end)
{
    if (m_backingText != text || m_backingWordStart != start || m_backingWordEnd != end) {
        m_backingText = text;
        m_backingWordStart = start;
        m_backingWordEnd = end;
        Q_EMIT backingChanged();
    }
}

void LrcApplet::clearBacking()
{
    setBackingState(QString(), 0, 0);
}

void LrcApplet::setLineIndex(int index)
{
    if (m_lineIndex != index) {
        m_lineIndex = index;
        Q_EMIT lineIndexChanged();
    }
}

namespace
{
struct ProjectedLine
{
    QString mainText;
    // Map main word index in line.words to (projectedGroupStart, projectedGroupEnd) in mainText
    QVector<QPair<int, int>> mainWordOffsets;

    struct BgPhrase {
        QString text;
        qint64 startMs = 0;
        qint64 endMs = 0;
        // Map word index in line.words to (phraseWordStart, phraseWordEnd) in BgPhrase::text
        QVector<QPair<int, int>> wordOffsets;
    };
    QVector<BgPhrase> bgPhrases;
};

ProjectedLine projectLineForSeparateRendering(const LyricLine &line)
{
    ProjectedLine result;
    const int numWords = line.words.size();
    result.mainWordOffsets.resize(numWords);
    for (int i = 0; i < numWords; ++i) {
        result.mainWordOffsets[i] = qMakePair(-1, -1);
    }

    if (line.isBackground) {
        // Entire line is background
        ProjectedLine::BgPhrase phrase;
        phrase.text = line.text;
        phrase.startMs = line.startMs;
        phrase.endMs = line.endMs;
        phrase.wordOffsets.resize(numWords);
        for (int i = 0; i < numWords; ++i) {
            const LyricWord &w = line.words.at(i);
            const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
            const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
            phrase.wordOffsets[i] = qMakePair(gStart, gEnd);
        }
        result.bgPhrases.append(phrase);
        return result;
    }

    bool hasBgWords = false;
    for (const LyricWord &w : line.words) {
        if (w.isBackground) {
            hasBgWords = true;
            break;
        }
    }

    if (!hasBgWords) {
        result.mainText = line.text;
        for (int i = 0; i < numWords; ++i) {
            const LyricWord &w = line.words.at(i);
            const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
            const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
            result.mainWordOffsets[i] = qMakePair(gStart, gEnd);
        }
        return result;
    }

    const int len = line.text.length();
    QVector<bool> isBgChar(len, false);
    for (const LyricWord &w : line.words) {
        if (w.isBackground) {
            const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
            const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
            const int s = qBound(0, gStart, len);
            const int e = qBound(s, gEnd, len);
            for (int i = s; i < e; ++i) {
                isBgChar[i] = true;
            }
        }
    }

    // Exclude adjacent whitespace appropriately
    int i = 0;
    while (i < len) {
        if (isBgChar[i]) {
            int bgStart = i;
            while (i < len && isBgChar[i]) {
                ++i;
            }
            int bgEnd = i;
            if (bgEnd == len) {
                if (bgStart > 0 && line.text.at(bgStart - 1).isSpace()) {
                    isBgChar[bgStart - 1] = true;
                }
            } else if (bgStart == 0) {
                if (bgEnd < len && line.text.at(bgEnd).isSpace()) {
                    isBgChar[bgEnd] = true;
                }
            } else {
                if (line.text.at(bgStart - 1).isSpace() && line.text.at(bgEnd).isSpace()) {
                    isBgChar[bgStart - 1] = true;
                }
            }
        } else {
            ++i;
        }
    }

    // Project main text and build mapping
    QVector<int> origToProj(len + 1, -1);
    for (int idx = 0; idx < len; ++idx) {
        if (!isBgChar[idx]) {
            origToProj[idx] = result.mainText.length();
            result.mainText.append(line.text.at(idx));
        }
    }
    origToProj[len] = result.mainText.length();

    for (int wIdx = 0; wIdx < numWords; ++wIdx) {
        const LyricWord &w = line.words.at(wIdx);
        if (!w.isBackground) {
            const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
            const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
            int s = qBound(0, gStart, len);
            int e = qBound(s, gEnd, len);
            int projS = -1;
            for (int k = s; k < e; ++k) {
                if (origToProj[k] >= 0) {
                    projS = origToProj[k];
                    break;
                }
            }
            int projE = -1;
            for (int k = e - 1; k >= s; --k) {
                if (origToProj[k] >= 0) {
                    projE = origToProj[k] + 1;
                    break;
                }
            }
            if (projS >= 0 && projE >= projS) {
                result.mainWordOffsets[wIdx] = qMakePair(projS, projE);
            } else {
                result.mainWordOffsets[wIdx] = qMakePair(-1, -1);
            }
        }
    }

    // Build background phrases from background words in this line
    int curGroup = -2;
    ProjectedLine::BgPhrase curPhrase;
    QVector<int> curPhraseWordIndices;

    auto finalizePhrase = [&]() {
        if (curPhraseWordIndices.isEmpty()) return;
        const LyricWord &firstW = line.words.at(curPhraseWordIndices.first());
        const LyricWord &lastW = line.words.at(curPhraseWordIndices.last());
        const int firstGStart = (firstW.groupStart >= 0) ? firstW.groupStart : firstW.textStart;
        const int lastGEnd = (lastW.groupEnd > lastW.groupStart) ? lastW.groupEnd : ((lastW.textEnd > lastW.textStart) ? lastW.textEnd : lastW.groupStart + lastW.text.length());
        const int s = qBound(0, firstGStart, len);
        const int e = qBound(s, lastGEnd, len);
        curPhrase.text = line.text.mid(s, e - s);
        curPhrase.startMs = firstW.startMs;
        curPhrase.endMs = lastW.endMs;
        curPhrase.wordOffsets.resize(numWords);
        for (int k = 0; k < numWords; ++k) {
            curPhrase.wordOffsets[k] = qMakePair(-1, -1);
        }
        for (int wIdx : curPhraseWordIndices) {
            const LyricWord &w = line.words.at(wIdx);
            const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
            const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
            curPhrase.wordOffsets[wIdx] = qMakePair(gStart - s, gEnd - s);
        }
        result.bgPhrases.append(curPhrase);
        curPhrase = ProjectedLine::BgPhrase();
        curPhraseWordIndices.clear();
        curGroup = -2;
    };

    for (int wIdx = 0; wIdx < numWords; ++wIdx) {
        const LyricWord &w = line.words.at(wIdx);
        if (!w.isBackground) {
            if (curGroup == -1) {
                finalizePhrase();
            }
            continue;
        }

        const int g = w.backgroundGroup;
        if (curGroup != -2 && (g >= 0 ? (g != curGroup) : (curGroup != -1))) {
            finalizePhrase();
        }
        curGroup = g;
        curPhraseWordIndices.append(wIdx);
    }
    finalizePhrase();

    return result;
}
} // namespace

void LrcApplet::updateWord()
{
    if (m_fromSpicy && isSpicyExpired()) {
        handleLyricsExpired();
        return;
    }

    if (!m_positionValid) {
        requestPosition();
        return;
    }

    // Keep polling Position every kPositionCheckMs even if lyrics are missing or
    // hidden on pause so that playhead state is always accurate when the track ends.
    if (!m_positionChecks.isValid() || m_positionChecks.elapsed() > kPositionCheckMs) {
        m_positionChecks.start();
        requestPosition();
    }

    if (m_pauseHidden || !m_lyrics.isUsable()) {
        return;
    }

    m_positionMs = m_positionBaseMs + (m_playing && m_positionClock.isValid() ? m_positionClock.elapsed() : 0) - m_gaplessLeadMs - kAudioLeadCompensationMs - m_lyricOffset;

    // Before the first line of the track (intro), clear any stale or phantom text.
    if (!m_lyrics.lines.isEmpty() && m_positionMs < m_lyrics.lines.first().startMs) {
        setLineIndex(-1);
        m_wordCursor = 0;
        clearWordHighlight();
        clearBacking();
        setText(QString());
        setActive(false);
        return;
    }

    int nextLineIdx = -1;
    const int rawLineIndex = m_lyrics.lineAt(m_positionMs, &nextLineIdx);

    int mainLineIndex = -1;
    int bgLineIndex = -1;

    const int latestStartedIdx = (nextLineIdx >= 0) ? (nextLineIdx - 1) : (m_lyrics.lines.size() - 1);

    if (rawLineIndex >= 0 && rawLineIndex < m_lyrics.lines.size()) {
        if (m_lyrics.lines.at(rawLineIndex).isBackground) {
            bgLineIndex = rawLineIndex;
            // Scan backwards from latestStartedIdx for an active main line
            for (int i = latestStartedIdx; i >= 0; --i) {
                const LyricLine &l = m_lyrics.lines.at(i);
                if (!l.isBackground && m_positionMs >= l.startMs && m_positionMs < l.endMs) {
                    mainLineIndex = i;
                    break;
                }
            }
        } else {
            mainLineIndex = rawLineIndex;
        }
    }

    // Scan BG backwards from latest started line index (nextLineIdx>=0? nextLineIdx-1:lines.size()-1),
    // no hard 8 bound, select latest active BG. Main remains lineAt primary.
    if (m_showBackingVocals && bgLineIndex < 0 && latestStartedIdx >= 0) {
        for (int i = latestStartedIdx; i >= 0; --i) {
            const LyricLine &l = m_lyrics.lines.at(i);
            if (l.isBackground && m_positionMs >= l.startMs && m_positionMs < l.endMs) {
                bgLineIndex = i;
                break;
            }
        }
    }

    if (m_showBackingVocals && bgLineIndex < 0 && mainLineIndex >= 0) {
        const LyricLine &main = m_lyrics.lines.at(mainLineIndex);
        for (int i = 0; i < m_lyrics.lines.size(); ++i) {
            const LyricLine &l = m_lyrics.lines.at(i);
            if (l.isBackground && l.startMs < main.endMs && l.endMs > main.startMs) {
                bgLineIndex = i;
                break;
            }
        }
    }

    // Check if both main and background are inactive
    if (mainLineIndex < 0 && (!m_showBackingVocals || bgLineIndex < 0)) {
        clearWordHighlight();

        if (m_text.isEmpty()) {
            clearBacking();
            setLineIndex(-1);
            setActive(false);
            return;
        }

        // Check if playback is past the last line (outro)
        if (!m_lyrics.lines.isEmpty() && m_positionMs > m_lyrics.lines.last().endMs) {
            if (m_positionMs > m_lyrics.lines.last().endMs + kTailGraceMs) {
                setText(QString());
                clearBacking();
                setLineIndex(-1);
                setActive(false);
            }
            return;
        }

        // Between lines: during an instrumental break (gap >= 2.5s), clear the display
        // after a 1.2s grace period so nothing is shown during the musical pause.
        if (nextLineIdx > 0 && nextLineIdx < m_lyrics.lines.size()) {
            const LyricLine &prevLine = m_lyrics.lines.at(nextLineIdx - 1);
            const LyricLine &nextLine = m_lyrics.lines.at(nextLineIdx);
            const qint64 gapDuration = nextLine.startMs - prevLine.endMs;

            if (gapDuration >= 2500) {
                if (m_positionMs >= prevLine.endMs + 1200) {
                    setText(QString());
                    clearBacking();
                    setLineIndex(-1);
                    setActive(false);
                }
                return;
            }
        }
        return;
    }

    if (!m_showBackingVocals) {
        clearBacking();
        const int activeLineIdx = (mainLineIndex >= 0) ? mainLineIndex : rawLineIndex;
        if (activeLineIdx < 0 || activeLineIdx >= m_lyrics.lines.size()) {
            clearWordHighlight();
            return;
        }
        const LyricLine &line = m_lyrics.lines.at(activeLineIdx);

        QString tsPrefix;
        if (m_showTimestamp) {
            const qint64 totalSecs = qMax(0LL, line.startMs) / 1000;
            const qint64 mins = totalSecs / 60;
            const qint64 secs = totalSecs % 60;
            tsPrefix = QStringLiteral("[%1:%2] ").arg(mins, 2, 10, QLatin1Char('0')).arg(secs, 2, 10, QLatin1Char('0'));
        }
        const QString displayLine = tsPrefix + line.text;
        const bool lineChanged = (activeLineIdx != m_lineIndex);
        if (displayLine != m_text) {
            setActive(true);
            setText(displayLine);
        } else if (lineChanged) {
            setActive(true);
            Q_EMIT textChanged();
        }

        if (lineChanged) {
            setLineIndex(activeLineIdx);
            m_wordCursor = 0;
            clearWordHighlight();
        }

        if (!m_wordSynced || line.words.isEmpty()) {
            clearWordHighlight();
            return;
        }

        QString currentWord;
        int currentStart = 0;
        int currentEnd = 0;
        double progress = 0.0;

        // Legacy single-highlight with primary priority via selector:
        // Use SINGLE wordAt Voice::Any, hold 80; selector already prioritizes activeMain.
        const int activeWordIdx = LyricSelection::wordAt(line, m_positionMs, LyricSelection::Voice::Any, kWordGapHoldMs);

        if (activeWordIdx >= 0 && activeWordIdx < line.words.size()) {
            const LyricWord &word = line.words.at(activeWordIdx);
            const int gStart = (word.groupStart >= 0) ? word.groupStart : word.textStart;
            const int gEnd = (word.groupEnd > gStart) ? word.groupEnd : ((word.textEnd > gStart) ? word.textEnd : gStart + word.text.length());
            currentWord = line.text.mid(gStart, gEnd - gStart);
            const int prefixOffset = tsPrefix.length();
            currentStart = gStart + prefixOffset;
            currentEnd = gEnd + prefixOffset;
            const qint64 span = word.endMs - word.startMs;
            progress = span > 0 ? static_cast<double>(m_positionMs - word.startMs) / static_cast<double>(span) : 0.0;
            progress = qBound(0.0, progress, 1.0);
        }

        if (currentWord != m_word || currentStart != m_wordStart || currentEnd != m_wordEnd) {
            m_word = currentWord;
            m_wordStart = currentStart;
            m_wordEnd = currentEnd;
            m_wordProgress = progress;
            Q_EMIT wordChanged();
        } else {
            m_wordProgress = progress;
        }
        return;
    }

    // Experimental separate rendering mode:
    // MAIN and BACK highlight SIMULTANEOUSLY and independently with independent cursors/properties per voice.
    if (mainLineIndex >= 0) {
        const bool lineChanged = (m_lineIndex != mainLineIndex);
        if (lineChanged) {
            m_wordCursor = 0;
            clearWordHighlight();
        }
        const LyricLine &mainLine = m_lyrics.lines.at(mainLineIndex);
        ProjectedLine projected = projectLineForSeparateRendering(mainLine);

        QString tsPrefix;
        if (m_showTimestamp && !projected.mainText.isEmpty()) {
            const qint64 totalSecs = qMax(0LL, mainLine.startMs) / 1000;
            const qint64 mins = totalSecs / 60;
            const qint64 secs = totalSecs % 60;
            tsPrefix = QStringLiteral("[%1:%2] ").arg(mins, 2, 10, QLatin1Char('0')).arg(secs, 2, 10, QLatin1Char('0'));
        }

        const QString displayLine = projected.mainText.isEmpty() ? QString() : (tsPrefix + projected.mainText);
        if (displayLine != m_text) {
            setText(displayLine);
        } else if (lineChanged) {
            Q_EMIT textChanged();
        }

        // Main highlight independently timed for Voice::Main
        QString currentWord;
        int currentStart = 0;
        int currentEnd = 0;
        double progress = 0.0;

        if (m_wordSynced) {
            const int activeMainIdx = LyricSelection::wordAt(mainLine, m_positionMs, LyricSelection::Voice::Main, kWordGapHoldMs);
            if (activeMainIdx >= 0 && activeMainIdx < projected.mainWordOffsets.size()) {
                const auto offsets = projected.mainWordOffsets.at(activeMainIdx);
                if (offsets.first >= 0 && offsets.second >= offsets.first) {
                    currentWord = projected.mainText.mid(offsets.first, offsets.second - offsets.first);
                    const int prefixOffset = tsPrefix.length();
                    currentStart = offsets.first + prefixOffset;
                    currentEnd = offsets.second + prefixOffset;
                    const LyricWord &w = mainLine.words.at(activeMainIdx);
                    const qint64 span = w.endMs - w.startMs;
                    progress = span > 0 ? static_cast<double>(m_positionMs - w.startMs) / static_cast<double>(span) : 0.0;
                    progress = qBound(0.0, progress, 1.0);
                }
            }
        }

        if (currentWord != m_word || currentStart != m_wordStart || currentEnd != m_wordEnd) {
            m_word = currentWord;
            m_wordStart = currentStart;
            m_wordEnd = currentEnd;
            m_wordProgress = progress;
            Q_EMIT wordChanged();
        } else {
            m_wordProgress = progress;
        }

        // Backing vocal independently timed for Voice::Background
        QString newBackingText;
        int newBackingStart = 0;
        int newBackingEnd = 0;

        if (!projected.bgPhrases.isEmpty()) {
            const int activeBgIdx = m_wordSynced ? LyricSelection::wordAt(mainLine, m_positionMs, LyricSelection::Voice::Background, kWordGapHoldMs) : -1;

            const ProjectedLine::BgPhrase *chosenPhrase = nullptr;
            if (activeBgIdx >= 0) {
                // Choose displayed BG phrase containing selected BG word
                for (const auto &phrase : projected.bgPhrases) {
                    if (activeBgIdx < phrase.wordOffsets.size() && phrase.wordOffsets.at(activeBgIdx).first >= 0) {
                        chosenPhrase = &phrase;
                        break;
                    }
                }
            }

            // If no active word was found, select latest active phrase by time
            if (!chosenPhrase) {
                for (int p = projected.bgPhrases.size() - 1; p >= 0; --p) {
                    const auto &phrase = projected.bgPhrases.at(p);
                    if (m_positionMs >= phrase.startMs && m_positionMs < phrase.endMs + kWordGapHoldMs) {
                        chosenPhrase = &phrase;
                        break;
                    }
                }
            }

            // If still no phrase by time, keep the line's backing phrase stable throughout the line
            // so backing text is present from line start ("изначально серое") without layout jumping.
            if (!chosenPhrase) {
                if (m_positionMs < projected.bgPhrases.first().startMs) {
                    chosenPhrase = &projected.bgPhrases.first();
                } else if (m_positionMs >= projected.bgPhrases.last().endMs) {
                    chosenPhrase = &projected.bgPhrases.last();
                } else {
                    for (const auto &phrase : projected.bgPhrases) {
                        if (m_positionMs < phrase.endMs) {
                            chosenPhrase = &phrase;
                            break;
                        }
                    }
                    if (!chosenPhrase) {
                        chosenPhrase = &projected.bgPhrases.last();
                    }
                }
            }

            if (chosenPhrase) {
                newBackingText = chosenPhrase->text;
                if (activeBgIdx >= 0 && activeBgIdx < chosenPhrase->wordOffsets.size()) {
                    const auto bgOffsets = chosenPhrase->wordOffsets.at(activeBgIdx);
                    if (bgOffsets.first >= 0) {
                        newBackingStart = bgOffsets.first;
                        newBackingEnd = bgOffsets.second;
                    }
                }
            }
        }

        if (!newBackingText.isEmpty()) {
            if (lineChanged && m_backingText == newBackingText && m_backingWordStart == newBackingStart && m_backingWordEnd == newBackingEnd) {
                Q_EMIT backingChanged();
            }
            setBackingState(newBackingText, newBackingStart, newBackingEnd);
        } else if (bgLineIndex >= 0) {
            // Background line alongside main line
            const LyricLine &bgLine = m_lyrics.lines.at(bgLineIndex);
            newBackingText = bgLine.text;
            if (m_wordSynced) {
                int activeBgIdx = LyricSelection::wordAt(bgLine, m_positionMs, LyricSelection::Voice::Background, kWordGapHoldMs);
                if (activeBgIdx < 0) {
                    activeBgIdx = LyricSelection::wordAt(bgLine, m_positionMs, LyricSelection::Voice::Any, kWordGapHoldMs);
                }
                if (activeBgIdx >= 0 && activeBgIdx < bgLine.words.size()) {
                    const LyricWord &w = bgLine.words.at(activeBgIdx);
                    const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
                    const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
                    newBackingStart = gStart;
                    newBackingEnd = gEnd;
                }
            }
            if (lineChanged && m_backingText == newBackingText && m_backingWordStart == newBackingStart && m_backingWordEnd == newBackingEnd) {
                Q_EMIT backingChanged();
            }
            setBackingState(newBackingText, newBackingStart, newBackingEnd);
        } else {
            clearBacking();
        }
        setLineIndex(mainLineIndex);
    } else if (bgLineIndex >= 0) {
        // Bg only line (no main line active)
        setLineIndex(bgLineIndex);
        setText(QString());
        clearWordHighlight();

        const LyricLine &bgLine = m_lyrics.lines.at(bgLineIndex);
        QString newBackingText = bgLine.text;
        int newBackingStart = 0;
        int newBackingEnd = 0;

        if (m_wordSynced) {
            int activeBgIdx = LyricSelection::wordAt(bgLine, m_positionMs, LyricSelection::Voice::Background, kWordGapHoldMs);
            if (activeBgIdx < 0) {
                activeBgIdx = LyricSelection::wordAt(bgLine, m_positionMs, LyricSelection::Voice::Any, kWordGapHoldMs);
            }
            if (activeBgIdx >= 0 && activeBgIdx < bgLine.words.size()) {
                const LyricWord &w = bgLine.words.at(activeBgIdx);
                const int gStart = (w.groupStart >= 0) ? w.groupStart : w.textStart;
                const int gEnd = (w.groupEnd > gStart) ? w.groupEnd : ((w.textEnd > gStart) ? w.textEnd : gStart + w.text.length());
                newBackingStart = gStart;
                newBackingEnd = gEnd;
            }
        }
        setBackingState(newBackingText, newBackingStart, newBackingEnd);
    }

    // Set active true when bg only and experimental non-empty; QML agent adapts hasLyrics
    if (!m_text.isEmpty() || !m_backingText.isEmpty()) {
        setActive(true);
    } else {
        setActive(false);
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
            for (const QString &pattern : ignored) {
                if (playerNameMatches(pattern, player)) {
                    return true;
                }
            }
            return false;
        });
    }
    if (m_candidates.isEmpty()) {
        if (!m_watchedService.isEmpty() || !m_player.isEmpty()) {
            clearDisplay();
            unwatchPlayer();
            setPlayer(QString());
        }
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
    if (m_candidates.isEmpty()) {
        m_candidateIndex = 0;
        return;
    }

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
            if (playing(m_candidates.at(i)) && playerNameMatches(m_preferredPlayer, m_candidates.at(i))) {
                m_candidates.move(i, 0);
                break;
            }
        }
    }

    // Where in the list to continue, and whether to jump to the top of it.
    //
    // A player that is playing is left alone unless the newcomer looks a lot more
    // like music or is a dedicated music player replacing a browser web page.
    const int currentIndex = m_candidates.indexOf(m_player);
    const int currentScore = m_playerScores.value(m_player, 0);
    const QString bestCandidate = m_candidates.first();
    const int bestScore = m_playerScores.value(bestCandidate, 0);
    const bool currentIsPlaying = m_playingKnown && m_playing;
    const bool bestIsPlaying = playing(bestCandidate);

    const bool bestIsWeb = looksLikeWebPageSession(QString(), bestCandidate);
    const bool currentIsWeb = looksLikeWebPageSession(QString(), m_player);
    const bool bestIsPreferred = !m_preferredPlayer.isEmpty() && playerNameMatches(m_preferredPlayer, bestCandidate);
    const bool currentIsPreferred = !m_preferredPlayer.isEmpty() && playerNameMatches(m_preferredPlayer, m_player);

    bool takeBest = currentIndex < 0;
    if (currentIndex >= 0) {
        if (bestIsPlaying && !currentIsPlaying) {
            takeBest = true;
        } else if (bestIsPreferred && bestIsPlaying && !currentIsPreferred) {
            takeBest = true;
        } else if (currentIsWeb && !bestIsWeb && bestIsPlaying) {
            takeBest = true;
        } else if (currentIsPlaying) {
            takeBest = bestScore > currentScore + kPlayerSwitchMargin;
        } else {
            takeBest = bestScore > 0 && bestCandidate != m_player;
        }
        m_candidateIndex = takeBest ? 0 : currentIndex;
    } else {
        m_candidateIndex = 0;
    }

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
    m_repeatTrack = false;
    delete m_propertyWatcher;
    m_propertyWatcher = nullptr;

    delete m_positionWatcher;
    m_positionWatcher = nullptr;

    cancelProcess();
    m_spicy->cancel();

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
    bus.disconnect(m_watchedService,
                   s_playerPath,
                   s_playerInterface,
                   QStringLiteral("Seeked"),
                   this,
                   SLOT(onPlayerSeeked(qint64)));
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
    bus.connect(m_watchedService,
                s_playerPath,
                s_playerInterface,
                QStringLiteral("Seeked"),
                this,
                SLOT(onPlayerSeeked(qint64)));

    requestPlayerProperties();
}

void LrcApplet::onPlayerPropertiesChanged(const QString &interfaceName, const QVariantMap &changed, const QStringList &invalidated)
{
    if (m_watchedService.isEmpty() || interfaceName != s_playerInterface) {
        return;
    }

    applyPlayerProperties(changed, invalidated);
}

void LrcApplet::onAnyPlayerPropertiesChanged(const QString &interfaceName, const QVariantMap &changed, const QStringList &invalidated)
{
    Q_UNUSED(invalidated);

    if (interfaceName != s_playerInterface) {
        return;
    }

    const auto it = changed.constFind(QStringLiteral("PlaybackStatus"));
    if (it != changed.constEnd()) {
        const QString status = it.value().toString();
        if (status.compare(QLatin1String("Playing"), Qt::CaseInsensitive) == 0) {
            m_candidatesTimer.invalidate();
            refreshPlayerCandidates();
            return;
        }
    }

    if (!m_playing && changed.contains(QStringLiteral("Metadata"))) {
        m_candidatesTimer.invalidate();
        refreshPlayerCandidates();
    }
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

    if (invalidated.contains(QStringLiteral("LoopStatus"))) {
        m_repeatTrack = false;
    }
    const auto loopIt = properties.constFind(QStringLiteral("LoopStatus"));
    if (loopIt != properties.constEnd()) {
        m_repeatTrack = loopIt.value().toString() == QLatin1String("Track");
    }

    if (invalidated.contains(QStringLiteral("PlaybackStatus"))) {
        setPlaying(false, false);
        return;
    }

    const bool wasPlayingKnown = m_playingKnown;
    const bool wasPlaying = m_playingKnown && m_playing;

    const auto statusIt = properties.constFind(QStringLiteral("PlaybackStatus"));
    bool hasStatus = (statusIt != properties.constEnd());
    bool nowPlaying = m_playing;
    QString statusStr;

    if (hasStatus) {
        statusStr = statusIt.value().toString();
        nowPlaying = statusStr.compare(QLatin1String("Playing"), Qt::CaseInsensitive) == 0;
        if (!nowPlaying) {
            m_gaplessLeadMs = 0;
        }
        // On initial GetAll attach (!wasPlayingKnown), set m_playing before applyMetadata
        // so applyMetadata knows the player is actively playing and can restore sync state.
        if (!wasPlayingKnown) {
            setPlaying(nowPlaying, true);
        }
    }

    const auto metadataIt = properties.constFind(QStringLiteral("Metadata"));
    if (metadataIt != properties.constEnd()) {
        const QVariantMap metadata = toVariantMap(metadataIt.value());
        applyMetadata(metadata);
    }

    if (hasStatus) {
        const bool statusChanged = (!wasPlayingKnown || wasPlaying != nowPlaying);
        if (statusChanged && !m_positionWatcher) {
            m_positionValid = false;
            m_positionRetries.invalidate();
        } else if (wasPlayingKnown && wasPlaying != nowPlaying) {
            if (m_positionWatcher) {
                delete m_positionWatcher;
                m_positionWatcher = nullptr;
            }
            m_positionValid = false;
            m_positionRetries.invalidate();
        }
        setPlaying(nowPlaying, true);
        if (statusChanged) {
            requestPosition();
        }

        if (statusStr.compare(QLatin1String("Stopped"), Qt::CaseInsensitive) == 0) {
            clearDisplay();
        }

        if ((wasPlaying && !nowPlaying) || statusStr.compare(QLatin1String("Stopped"), Qt::CaseInsensitive) == 0) {
            m_candidatesTimer.invalidate();
            considerPlayerSwitch();
        }
    }
}

K_PLUGIN_CLASS_WITH_JSON(LrcApplet, "metadata.json")

#include "lrcapplet.moc"
