/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root

    readonly property bool vertical: Plasmoid.formFactor === PlasmaCore.Types.Vertical
    readonly property string lyricText: Plasmoid.text
    readonly property bool hasLyrics: Plasmoid.active && lyricText.length > 0
    readonly property int maxCharacters: Plasmoid.maxCharacters

    // Lyrics come from a third party and end up in rich text, so they have to
    // be escaped before anything is wrapped around them.
    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    // What has already been sung is dimmed, so the panel popup reads the way a
    // karaoke line does. A colour is avoided on purpose: any accent colour would
    // clash with the panel background.
    readonly property color dimmedColor: {
        const base = Kirigami.Theme.textColor;
        return Qt.rgba(base.r, base.g, base.b, 0.45);
    }

    // The word being sung is marked either with an underline (0), bold (1), or highlight color (2).
    readonly property color highlightColor: {
        const custom = Plasmoid.customHighlightColor ? Plasmoid.customHighlightColor.trim() : "";
        return custom.length > 0 ? custom : Kirigami.Theme.highlightColor;
    }

    function formatActiveWord(word) {
        const escaped = escapeHtml(word);
        if (Plasmoid.wordStyle === 1) {
            return "<b>" + escaped + "</b>";
        }
        if (Plasmoid.wordStyle === 2) {
            return "<font color=\"" + highlightColor + "\">" + escaped + "</font>";
        }
        return "<u>" + escaped + "</u>";
    }

    // The line with the word being sung marked, for the panel.
    readonly property string compactLine: {
        if (!Plasmoid.wordSynced || Plasmoid.word.length === 0) {
            return escapeHtml(lyricText);
        }
        return escapeHtml(lyricText.substring(0, Plasmoid.wordStart))
            + formatActiveWord(Plasmoid.word)
            + escapeHtml(lyricText.substring(Plasmoid.wordEnd));
    }

    // The line with the word being sung marked, for the popup: what has already
    // been sung is dimmed.
    readonly property string fullLine: {
        if (!Plasmoid.wordSynced || Plasmoid.word.length === 0) {
            return escapeHtml(lyricText);
        }
        const sung = "<font color=\"" + dimmedColor + "\">" + escapeHtml(lyricText.substring(0, Plasmoid.wordStart)) + "</font>";
        const current = formatActiveWord(Plasmoid.word);
        const rest = escapeHtml(lyricText.substring(Plasmoid.wordEnd));
        return sung + current + rest;
    }

    readonly property string displayText: {
        if (Plasmoid.pauseHidden) {
            return "";
        }
        if (hasLyrics) {
            return lyricText;
        }
        if (!Plasmoid.available) {
            return i18n("lrc_tty not found");
        }
        return Plasmoid.placeholderText;
    }
    readonly property string hintText: !Plasmoid.available //
        ? i18n("Could not run %1: %2", Plasmoid.binaryPath, Plasmoid.error)
        : displayText

    // The credit for the lyrics, in a short form for the panel and a full one for
    // the popup.
    readonly property bool hasCredit: Plasmoid.attribution.length > 0
    readonly property string shortCredit: {
        const separator = Plasmoid.attribution.indexOf(" · ");
        return "· " + (separator > 0 ? Plasmoid.attribution.substring(0, separator) : Plasmoid.attribution);
    }

    Plasmoid.backgroundHints: Plasmoid.location === PlasmaCore.Types.Desktop //
        ? PlasmaCore.Types.DefaultBackground
        : PlasmaCore.Types.NoBackground
    preferredRepresentation: compactRepresentation

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: Plasmoid.isCurrentTrackBlacklisted
                ? i18n("Unblacklist current track lyrics")
                : i18n("Blacklist current track lyrics")
            icon.name: Plasmoid.isCurrentTrackBlacklisted ? "edit-undo" : "list-remove"
            enabled: Plasmoid.trackInfo.length > 0
            onTriggered: Plasmoid.toggleBlacklistCurrentTrack()
        }
    ]

    // Which source the line actually came from. The tooltip says it, because
    // "no attribution suffix" and "Spicy Lyrics is not answering" look exactly
    // the same otherwise.
    readonly property string sourceName: Plasmoid.fromSpicyLyrics //
        ? i18n("Spicy Lyrics")
        : (Plasmoid.attribution.length > 0 ? Plasmoid.attribution : i18n("lrc_tty"))
    readonly property string attributionHint: {
        if (Plasmoid.isCurrentTrackBlacklisted) {
            return i18n("lyrics blacklisted for this track");
        }
        if (Plasmoid.fromSpicyLyrics) {
            return i18n("via %1", sourceName);
        }
        if (!Plasmoid.spicyConfigured) {
            return i18n("via %1 — Spicy Lyrics is off or has no key", sourceName);
        }
        if (Plasmoid.apiHasNoTrackId) {
            return i18n("via %1 — this player gives no Spotify track, so the API was not asked", sourceName);
        }
        if (Plasmoid.apiHasNoTimings) {
            return i18n("via %1 — the API has this track, but not in sync", sourceName);
        }
        if (Plasmoid.apiUnreachable) {
            return i18n("via %1 — the API could not be reached", sourceName);
        }
        return i18n("via %1 — the API does not know this track", sourceName);
    }

    toolTipMainText: !Plasmoid.available //
        ? i18n("lrc_tty not found")
        : (Plasmoid.showTrackInfo && Plasmoid.trackInfo.length > 0 ? Plasmoid.trackInfo : i18n("Now playing"))
    toolTipSubText: [!Plasmoid.available ? hintText : hintText + "  " + attributionHint,
                     // The name the settings need: "chromium" and the full
                     // "chromium.instance18422" mean the same thing, and nobody
                     // guesses that without being told.
                     Plasmoid.player.length > 0 ? i18n("player: %1", Plasmoid.player) : ""]
                    .filter(function (line) {
                        return line.length > 0;
                    })
                    .join("\n")

    compactRepresentation: MouseArea {
        id: compactArea

        hoverEnabled: true
        activeFocusOnTab: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton

        Accessible.role: Accessible.StaticText
        Accessible.name: root.displayText

        // The panel gives an applet the width it asks for and does not come back
        // to it, so an empty lyric once meant a widget a few pixels wide, with the
        // text wrapped to one letter per line. The floor keeps a usable width
        // whatever the content happens to be at that moment.
        readonly property int floorWidth: Kirigami.Units.gridUnit * 8

        implicitWidth: Math.max(compactLayout.implicitWidth, floorWidth)
        implicitHeight: compactLayout.implicitHeight

        Layout.minimumWidth: implicitWidth
        Layout.minimumHeight: implicitHeight
        Layout.maximumWidth: implicitWidth

        onClicked: function (mouse) {
            if (mouse.button === Qt.MiddleButton) {
                if (Plasmoid.middleClickAction === 1) {
                    Plasmoid.togglePlayPause();
                } else if (Plasmoid.middleClickAction === 2) {
                    Plasmoid.nextTrack();
                }
            } else if (mouse.button === Qt.LeftButton) {
                if (Plasmoid.clickAction === 1) {
                    Plasmoid.togglePlayPause();
                } else {
                    root.expanded = !root.expanded;
                }
            }
        }

        GridLayout {
            id: compactLayout

            anchors.fill: parent
            columns: 3
            columnSpacing: Kirigami.Units.smallSpacing * 2

            Kirigami.Icon {
                id: noteIcon

                // Follows the icon theme, so that it does not look out of place
                // in the panel.
                source: root.hasLyrics ? "audio-x-generic" : "media-playlist-repeat"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                Layout.alignment: Qt.AlignCenter
                visible: Plasmoid.showIcon
                opacity: root.hasLyrics ? 1.0 : 0.5
            }

            PlasmaComponents3.Label {
                id: lyricLabel

                text: root.hasLyrics ? root.compactLine : root.displayText
                textFormat: Text.RichText
                color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
                horizontalAlignment: Plasmoid.textAlignment === 1 ? Text.AlignLeft : Text.AlignHCenter
                Layout.alignment: Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) : Qt.AlignCenter
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : implicitWidth
            }

            NumberAnimation {
                id: lineFadeAnim
                target: lyricLabel
                property: "opacity"
                from: 0.25
                to: 1.0
                duration: 160
                easing.type: Easing.OutQuad
            }

            Connections {
                target: root
                function onLyricTextChanged() {
                    if (Plasmoid.fadeTransition && root.hasLyrics) {
                        lineFadeAnim.restart();
                    }
                }
            }

            PlasmaComponents3.Label {
                id: creditLabel

                // Tiny, and it expands to the full credit while the pointer is
                // on the widget.
                text: compactArea.containsMouse ? Plasmoid.attribution : root.shortCredit
                color: Kirigami.Theme.textColor
                opacity: compactArea.containsMouse ? 0.85 : 0.6
                elide: Text.ElideRight
                visible: root.hasCredit && !Plasmoid.compactPanel
                Layout.alignment: Qt.AlignVCenter
                // The lyric gives up width first: without a minimum the panel
                // squeezes this one down to nothing, and then there is no credit
                // at all to be seen.
                Layout.minimumWidth: implicitWidth
                Layout.maximumWidth: Math.ceil(characterWidth.advanceWidth) * 22
            }
        }
    }

    fullRepresentation: LyricPopup {
        id: fullView

        // The popup takes its size from the content. An explicit minimum or
        // maximum here makes the window open at that size instead, and Plasma
        // then remembers it in the panel configuration.

        trackInfo: Plasmoid.trackInfo
        playerName: Plasmoid.player
        showTrackInfo: Plasmoid.showTrackInfo

        line: root.fullLine
        placeholder: Plasmoid.isCurrentTrackBlacklisted ? i18n("Lyrics are blacklisted for this track") : root.displayText
        hasLyrics: root.hasLyrics

        attribution: root.escapeHtml(Plasmoid.attribution)
        attributionUrl: Plasmoid.attributionUrl
        // The popup is where the credit is spelled out in full, so it does not
        // follow the compact setting from the panel.
        showAttribution: root.hasCredit
        isBlacklisted: Plasmoid.isCurrentTrackBlacklisted
        onToggleBlacklist: Plasmoid.toggleBlacklistCurrentTrack()
    }

    // Only used to find out how wide a single character is, so that the compact
    // representation can be limited to a configurable number of characters.
    TextMetrics {
        id: characterWidth

        font: Kirigami.Theme.defaultFont
        text: "0"
    }
}