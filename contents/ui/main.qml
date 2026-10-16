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
    readonly property bool hasLyrics: !Plasmoid.pauseHidden && Plasmoid.active && lyricText.length > 0
    readonly property int maxCharacters: Plasmoid.maxCharacters

    // Lyrics come from a third party and end up in rich text, so they have to
    // be escaped before anything is wrapped around them.
    function escapeHtml(text) {
        if (!text) return "";
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#39;");
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
        if (/^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(custom)) {
            return custom;
        }
        return Kirigami.Theme.highlightColor;
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

    function plainLine(lineText) {
        if (!hasLyrics) {
            return displayText;
        }
        const text = lineText !== undefined ? lineText : lyricText;
        if (maxCharacters > 0 && text.length > maxCharacters) {
            return text.substring(0, maxCharacters) + "…";
        }
        return text;
    }

    readonly property string truncatedPlainLine: plainLine(lyricText)

    function formatCompactLine(lineText) {
        const text = lineText !== undefined ? lineText : lyricText;
        const limit = (maxCharacters > 0 && text.length > maxCharacters) ? maxCharacters : text.length;
        const suffix = (maxCharacters > 0 && text.length > maxCharacters) ? "…" : "";
        const visibleText = text.substring(0, limit);

        if (!Plasmoid.wordSynced || Plasmoid.word.length === 0 || Plasmoid.wordStart >= limit || text !== lyricText) {
            return escapeHtml(visibleText + suffix);
        }
        const wStart = Math.max(0, Math.min(Plasmoid.wordStart, limit));
        const wEnd = Math.max(wStart, Math.min(Plasmoid.wordEnd, limit));
        const activeSlice = visibleText.substring(wStart, wEnd);
        return escapeHtml(visibleText.substring(0, wStart))
            + formatActiveWord(activeSlice)
            + escapeHtml(visibleText.substring(wEnd) + suffix);
    }

    // The line with the word being sung marked, for the panel.
    readonly property string compactLine: formatCompactLine(lyricText)

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
        return Plasmoid.placeholderText;
    }
    readonly property string hintText: displayText

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

    // Which source the line actually came from.
    readonly property string sourceName: Plasmoid.fromSpicyLyrics //
        ? i18n("Spicy Lyrics")
        : (Plasmoid.attribution.length > 0 ? Plasmoid.attribution : i18n("LRCLIB"))
    readonly property string attributionHint: {
        if (Plasmoid.isCurrentTrackBlacklisted) {
            return i18n("Lyrics blacklisted for this track");
        }
        if (root.hasLyrics) {
            return i18n("via %1", sourceName);
        }
        if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured) {
            if (Plasmoid.apiHasNoTimings) {
                return i18n("No synchronized lyrics available");
            }
            if (Plasmoid.apiUnreachable) {
                return i18n("Lyrics service unreachable");
            }
        }
        return i18n("No lyrics found for this track");
    }

    toolTipMainText: {
        if (Plasmoid.trackInfo.length > 0) {
            return Plasmoid.trackInfo;
        }
        return i18n("Plasma LRC");
    }
    toolTipSubText: {
        if (Plasmoid.trackInfo.length === 0) {
            return i18n("No player active");
        }
        if (Plasmoid.pauseHidden) {
            const pausedLines = [i18n("Playback paused")];
            if (Plasmoid.player.length > 0) {
                pausedLines.push(i18n("player: %1", Plasmoid.player));
            }
            return pausedLines.join("\n");
        }

        const lines = [];
        if (root.hasLyrics) {
            lines.push(root.lyricText);
            lines.push(attributionHint);
        } else {
            lines.push(attributionHint);
        }
        if (Plasmoid.player.length > 0) {
            lines.push(i18n("player: %1", Plasmoid.player));
        }
        return lines.filter(Boolean).join("\n");
    }

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
        readonly property int floorWidth: Plasmoid.pauseHidden ? 0 : Kirigami.Units.gridUnit * 8

        implicitWidth: Plasmoid.pauseHidden ? compactLayout.implicitWidth : Math.max(compactLayout.implicitWidth, floorWidth)
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
                visible: Plasmoid.showIcon && !Plasmoid.pauseHidden
                opacity: root.hasLyrics ? 1.0 : 0.5
            }

            TextMetrics {
                id: lineMetrics
                font: Kirigami.Theme.defaultFont
                text: root.plainLine(lyricContainer.displayedLyric)
            }

            Item {
                id: lyricContainer

                property string displayedLyric: root.lyricText

                readonly property real targetWidth: Math.ceil(lineMetrics.advanceWidth) + 4
                property real animatedWidth: targetWidth

                Behavior on animatedWidth {
                    enabled: Plasmoid.fadeTransition && root.hasLyrics
                    NumberAnimation {
                        duration: 160
                        easing.type: Easing.OutQuad
                    }
                }

                implicitWidth: animatedWidth
                implicitHeight: lyricLabel.implicitHeight

                Layout.preferredWidth: animatedWidth
                Layout.preferredHeight: implicitHeight
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : implicitWidth
                Layout.alignment: Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) : Qt.AlignCenter

                PlasmaComponents3.Label {
                    id: lyricLabel
                    anchors.fill: parent
                    text: root.hasLyrics ? root.formatCompactLine(lyricContainer.displayedLyric) : root.escapeHtml(root.displayText)
                    textFormat: Text.RichText
                    color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    horizontalAlignment: Plasmoid.textAlignment === 1 ? Text.AlignLeft : Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    opacity: 1.0
                }

                SequentialAnimation {
                    id: lineFadeAnim

                    NumberAnimation {
                        target: lyricLabel
                        property: "opacity"
                        to: 0.2
                        duration: 70
                        easing.type: Easing.InQuad
                    }
                    ScriptAction {
                        script: {
                            lyricContainer.displayedLyric = root.lyricText;
                        }
                    }
                    NumberAnimation {
                        target: lyricLabel
                        property: "opacity"
                        to: 1.0
                        duration: 90
                        easing.type: Easing.OutQuad
                    }
                }

                Connections {
                    target: root

                    function onLyricTextChanged() {
                        if (!Plasmoid.fadeTransition || !root.hasLyrics || lyricContainer.displayedLyric.length === 0) {
                            lineFadeAnim.stop();
                            lyricContainer.displayedLyric = root.lyricText;
                            lyricLabel.opacity = 1.0;
                        } else {
                            lineFadeAnim.restart();
                        }
                    }

                    function onHasLyricsChanged() {
                        if (!root.hasLyrics) {
                            lineFadeAnim.stop();
                            lyricContainer.displayedLyric = "";
                            lyricLabel.opacity = 1.0;
                        } else if (lyricContainer.displayedLyric !== root.lyricText) {
                            lineFadeAnim.stop();
                            lyricContainer.displayedLyric = root.lyricText;
                            lyricLabel.opacity = 1.0;
                        }
                    }
                }
            }

            PlasmaComponents3.Label {
                id: creditLabel

                // Tiny, and it expands to the full credit while the pointer is
                // on the widget.
                text: root.shortCredit
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
        placeholder: {
            if (Plasmoid.isCurrentTrackBlacklisted) {
                return i18n("Lyrics are blacklisted for this track");
            }
            if (Plasmoid.pauseHidden) {
                return i18n("Playback paused");
            }
            if (Plasmoid.trackInfo.length > 0) {
                if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured && Plasmoid.apiHasNoTimings) {
                    return i18n("No synchronized lyrics available");
                }
                return i18n("No lyrics available for this track");
            }
            return i18n("No active player");
        }
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