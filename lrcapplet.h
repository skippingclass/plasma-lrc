/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Plasma/Applet>

#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "spicylyrics.h"

class QDBusPendingCallWatcher;

/**
 * Panel widget showing the lyric line that is currently being sung.
 *
 * Two sources are used. lrc_tty(1) is asked for the current line on a timer,
 * the way lrc_tty --lines 1 --raw does it, and covers every MPRIS player
 * through lrclib.net. When the player is the Spotify desktop client its track
 * id is known, so the Spicy Lyrics API is asked as well: it answers with
 * community-made word-level syncs, which also lets the word being sung right
 * now be highlighted.
 *
 * A bit of MPRIS/DBus bookkeeping is done on the side to figure out which
 * player is worth talking to, where playback is, and what the tooltip shows.
 */
class LrcApplet : public Plasma::Applet
{
    Q_OBJECT

    // State coming from lrc_tty
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)
    Q_PROPERTY(QString error READ error NOTIFY availableChanged)

    // Word-level state, empty when the source has no word timings
    Q_PROPERTY(QString word READ word NOTIFY wordChanged)
    Q_PROPERTY(int wordStart READ wordStart NOTIFY wordChanged)
    Q_PROPERTY(int wordEnd READ wordEnd NOTIFY wordChanged)
    Q_PROPERTY(double wordProgress READ wordProgress NOTIFY wordChanged)
    Q_PROPERTY(bool wordSynced READ isWordSynced NOTIFY lyricsChanged)

    // Who the lyrics came from, required to be shown when the API is used
    Q_PROPERTY(QString attribution READ attribution NOTIFY lyricsChanged)
    Q_PROPERTY(QString attributionUrl READ attributionUrl NOTIFY lyricsChanged)
    Q_PROPERTY(bool fromSpicyLyrics READ fromSpicyLyrics NOTIFY lyricsChanged)
    /// Whether the API source can be used at all, i.e. it is on and has a key.
    Q_PROPERTY(bool spicyConfigured READ isSpicyConfigured NOTIFY settingsChanged)
    // Why the API did not provide the line, so that the tooltip can say it
    // instead of blaming the API for a track it simply does not have.
    Q_PROPERTY(bool apiHasNoTimings READ apiHasNoTimings NOTIFY lyricsChanged)
    Q_PROPERTY(bool apiUnreachable READ apiUnreachable NOTIFY lyricsChanged)

    // Player information coming from MPRIS
    Q_PROPERTY(QString player READ player NOTIFY playerChanged)
    Q_PROPERTY(QString trackInfo READ trackInfo NOTIFY trackInfoChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)

    // Configuration, mirrored here so that QML does not need to deal with
    // KConfigPropertyMap at all.
    Q_PROPERTY(QString placeholderText READ placeholderText NOTIFY settingsChanged)
    Q_PROPERTY(QString binaryPath READ binaryPath NOTIFY settingsChanged)
    Q_PROPERTY(int maxCharacters READ maxCharacters NOTIFY settingsChanged)
    Q_PROPERTY(bool showIcon READ showIcon NOTIFY settingsChanged)
    Q_PROPERTY(bool showTrackInfo READ showTrackInfo NOTIFY settingsChanged)
    Q_PROPERTY(bool compactCredit READ compactCredit NOTIFY settingsChanged)

public:
    explicit LrcApplet(QObject *parent, const KPluginMetaData &data, const QVariantList &args);
    ~LrcApplet() override;

    void configChanged() override;
    void constraintsEvent(Constraints constraints) override;

    QString text() const
    {
        return m_text;
    }
    /// Why lrc_tty could not be run, empty when everything is fine.
    QString error() const
    {
        return m_error;
    }
    /// The word being sung right now, empty when there are no word timings.
    QString word() const
    {
        return m_word;
    }
    /// Where that word starts inside text().
    int wordStart() const
    {
        return m_wordStart;
    }
    /// Where that word ends inside text().
    int wordEnd() const
    {
        return m_wordEnd;
    }
    /// How far through that word playback is, 0.0 to 1.0.
    double wordProgress() const
    {
        return m_wordProgress;
    }
    bool isWordSynced() const
    {
        return m_wordSynced;
    }
    QString attribution() const;
    QString attributionUrl() const;
    bool fromSpicyLyrics() const
    {
        return m_fromSpicy;
    }
    bool isSpicyConfigured() const
    {
        return m_spicy->isEnabled();
    }
    /// The API has the track, but only text without timings.
    bool apiHasNoTimings() const
    {
        return m_missingReason == MissingReason::Unsynced;
    }
    /// No answer at all: no network, key rejected, rate limited.
    bool apiUnreachable() const
    {
        return m_missingReason == MissingReason::Unreachable;
    }

    bool isActive() const
    {
        return m_active;
    }
    bool isAvailable() const
    {
        return m_available;
    }
    QString player() const
    {
        return m_player;
    }
    QString trackInfo() const
    {
        return m_trackInfo;
    }
    bool isPlaying() const
    {
        return !m_playingKnown || m_playing;
    }

    QString placeholderText() const;
    QString binaryPath() const;
    int maxCharacters() const;
    bool showIcon() const;
    bool showTrackInfo() const;
    /// Whether the panel shows the short credit next to the lyrics.
    bool compactCredit() const;

Q_SIGNALS:
    void textChanged();
    void activeChanged();
    void availableChanged();
    void wordChanged();
    void lyricsChanged();
    void playerChanged();
    void trackInfoChanged();
    void playingChanged();
    void settingsChanged();

public Q_SLOTS:
    /// Forget everything we know and ask lrc_tty right away.
    void refresh();

private Q_SLOTS:
    void onPlayerPropertiesChanged(const QString &interfaceName, const QVariantMap &changed, const QStringList &invalidated);
    void onPlayerPropertiesFetched();
    void onPlayerListFetched();
    void onServiceOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void onSpicyLoaded(const QString &trackId, const Lyrics &lyrics);
    void onSpicyMissing(const QString &trackId, MissingReason reason);
    void onPositionFetched();
    void updateWord();

private:
    void startPolling();
    void poll();
    void onProcessFinished();
    void onProcessFailedToStart();

    void setText(const QString &text);
    void setActive(bool active);
    void setAvailable(bool available);
    void setError(const QString &error);
    void setPlaying(bool playing, bool known);
    void setTrackInfo(const QString &trackInfo);
    void setPlayer(const QString &player);

    /// Called when the watched player reports a new Metadata map.
    void applyMetadata(const QVariantMap &metadata);
    /// Asks MPRIS for Position, and interpolates from there.
    void requestPosition();
    void requestSpicyLyrics();
    void clearLyrics();

    /// Players to ask, in order of preference.
    QStringList playerCandidates();
    void refreshPlayerCandidates();
    void invalidatePlayerCandidates();

    void watchPlayer();
    void unwatchPlayer();
    void requestPlayerProperties();
    void applyPlayerProperties(const QVariantMap &properties, const QStringList &invalidated);

    QString setting(const QString &key, const QString &defaultValue = QString()) const;
    int intSetting(const QString &key, int defaultValue) const;
    bool boolSetting(const QString &key, bool defaultValue) const;
    /// API key from SPICY_LYRICS_SECRET_KEY, or from the widget config.
    QString spicyKeySetting() const;
    void readSettings();

    QTimer m_timer;
    QTimer m_watchdog;
    /// Fast enough for a word highlight to look like a highlight.
    QTimer m_wordTimer;
    QProcess *m_process;
    SpicyLyrics *m_spicy;

    QString m_text;
    QString m_error;
    QString m_player;
    QString m_trackInfo;
    QString m_watchedService;
    QDBusPendingCallWatcher *m_propertyWatcher;
    QDBusPendingCallWatcher *m_namesWatcher;
    QDBusPendingCallWatcher *m_positionWatcher;

    QString m_binaryPath;
    QString m_configuredPlayer;
    QString m_placeholderText;
    int m_pollInterval;
    int m_maxCharacters;
    bool m_showTimestamp;
    bool m_showIcon;
    bool m_showTrackInfo;
    bool m_compactCredit;
    bool m_pauseWhenIdle;

    QString m_spicyKey;
    bool m_useSpicy;

    QStringList m_candidates;
    QElapsedTimer m_candidatesTimer;

    int m_candidateIndex;
    int m_lastGoodCandidate;
    int m_noLyricsCount;

    bool m_active;
    bool m_available;
    bool m_playing;
    bool m_playingKnown;
    bool m_started;

    // Word-level lyrics of the current track
    Lyrics m_lyrics;
    QString m_spotifyTrackId;
    QString m_word;
    int m_wordStart;
    int m_wordEnd;
    double m_wordProgress;
    bool m_wordSynced;
    bool m_fromSpicy;
    MissingReason m_missingReason;

    qint64 m_positionMs;
    qint64 m_positionBaseMs;
    QElapsedTimer m_positionClock;
    /// Rate limits asking the player for its position again.
    QElapsedTimer m_positionRetries;
    bool m_positionValid;
    int m_lineIndex;
};