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

class QDBusPendingCallWatcher;

/**
 * Panel widget showing the lyric line that is currently being sung.
 *
 * All the lyrics handling is delegated to lrc_tty(1), this applet only
 * periodically runs `lrc_tty --lines 1 --raw` and exposes whatever it printed
 * to QML. A bit of MPRIS/DBus bookkeeping is done on the side to figure out
 * which player is worth talking to and to show the track in the tooltip.
 */
class LrcApplet : public Plasma::Applet
{
    Q_OBJECT

    // State coming from lrc_tty
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)
    Q_PROPERTY(QString error READ error NOTIFY availableChanged)

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

Q_SIGNALS:
    void textChanged();
    void activeChanged();
    void availableChanged();
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
    void readSettings();

    QTimer m_timer;
    QTimer m_watchdog;
    QProcess *m_process;

    QString m_text;
    QString m_error;
    QString m_player;
    QString m_trackInfo;
    QString m_watchedService;
    QDBusPendingCallWatcher *m_propertyWatcher;
    QDBusPendingCallWatcher *m_namesWatcher;

    QString m_binaryPath;
    QString m_configuredPlayer;
    QString m_placeholderText;
    int m_pollInterval;
    int m_maxCharacters;
    bool m_showTimestamp;
    bool m_showIcon;
    bool m_showTrackInfo;
    bool m_pauseWhenIdle;

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
};
