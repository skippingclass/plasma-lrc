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

    readonly property bool hasCredit: (Plasmoid.attributionHtml.length > 0 || Plasmoid.attribution.length > 0)
    readonly property bool hasPanelCredit: hasCredit && Plasmoid.fromSpicyLyrics
    readonly property bool isLayoutBelow: !vertical && hasPanelCredit && Plasmoid.attributionLayout === "Below"

    FontMetrics {
        id: defaultFontMetrics
        font: Kirigami.Theme.defaultFont
    }

    FontMetrics {
        id: smallFontMetrics
        font: Kirigami.Theme.smallFont
    }

    readonly property real creditRowHeight: Math.ceil(smallFontMetrics.height)

    readonly property font lyricBelowFont: {
        if (!root.isLayoutBelow || !root.hasLyrics || !root.hasPanelCredit) {
            return Kirigami.Theme.defaultFont;
        }
        if (defaultFontMetrics.height + smallFontMetrics.height <= 30) {
            return Kirigami.Theme.defaultFont;
        }
        const sf = Kirigami.Theme.smallFont;
        const maxLyricHeight = Math.floor(30 - creditRowHeight);
        if (smallFontMetrics.height > maxLyricHeight && maxLyricHeight >= 8) {
            return Qt.font({
                family: sf.family,
                pixelSize: maxLyricHeight,
                weight: sf.weight
            });
        }
        return sf;
    }

    FontMetrics {
        id: lyricFontMetrics
        font: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont
    }

    readonly property real lyricRowHeight: Math.max(Math.ceil(lyricFontMetrics.height), 30 - creditRowHeight)

    Plasmoid.backgroundHints: Plasmoid.location === PlasmaCore.Types.Desktop //
        ? PlasmaCore.Types.DefaultBackground
        : PlasmaCore.Types.NoBackground
    preferredRepresentation: compactRepresentation

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Copy current lyric line")
            icon.name: "edit-copy"
            visible: !Plasmoid.fromSpicyLyrics
            enabled: !Plasmoid.fromSpicyLyrics && Plasmoid.text.trim().length > 0
            onTriggered: Plasmoid.copyCurrentLine()
        },
        PlasmaCore.Action {
            text: i18n("Open song lyrics…")
            icon.name: "text-x-generic"
            enabled: Plasmoid.fullLyrics.length > 0
            onTriggered: lyricsWindow.openSong(Plasmoid.trackInfo, Plasmoid.fullLyrics,
                Plasmoid.currentLyricOffset(), Plasmoid.attribution, Plasmoid.attributionUrl, Plasmoid.attributionHtml,
                Plasmoid.lyricsValidUntilMs, Plasmoid.fromSpicyLyrics)
        },
        PlasmaCore.Action {
            text: Plasmoid.isCurrentTrackBlacklisted
                ? i18n("Unblacklist current track lyrics")
                : i18n("Blacklist current track lyrics")
            icon.name: Plasmoid.isCurrentTrackBlacklisted ? "edit-undo" : "list-remove"
            enabled: Plasmoid.trackInfo.length > 0
            onTriggered: Plasmoid.toggleBlacklistCurrentTrack()
        }
    ]

    SongLyricsWindow {
        id: lyricsWindow
    }

    // Which source the line actually came from.
    readonly property string sourceName: {
        if (Plasmoid.provider.length > 0) {
            return Plasmoid.provider;
        }
        if (Plasmoid.attribution.length > 0) {
            return Plasmoid.attribution;
        }
        return Plasmoid.fromSpicyLyrics ? i18n("Unknown") : i18n("LRCLIB");
    }
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
                return Plasmoid.apiError;
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
        if (Plasmoid.player.length > 0) {
            lines.push(i18n("player: %1", Plasmoid.player));
        }
        lines.push(attributionHint);
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

        readonly property bool showIconItem: Plasmoid.showIcon && !Plasmoid.pauseHidden
        readonly property real iconReservedWidth: showIconItem ? Kirigami.Units.iconSizes.small : 0
        readonly property bool creditItemVisible: root.hasLyrics && root.hasPanelCredit
        readonly property real creditFullWidth: creditItemVisible ? Math.ceil(creditLabel.implicitWidth) : 0

        readonly property real lyricDesiredWidth: Plasmoid.pauseHidden ? 0 : lyricContainer.animatedWidth
        readonly property real usefulLyricWidth: Plasmoid.pauseHidden ? 0 : Math.min(floorWidth, lyricDesiredWidth)

        readonly property real spacing: compactLayout.columnSpacing

        // Below mode: noteIcon is in column 0 (spanning 2 rows), lyric and credit share column 1.
        readonly property real belowDesiredWidth: (showIconItem ? (iconReservedWidth + spacing) : 0)
            + Math.max(lyricDesiredWidth, creditFullWidth)
        readonly property real belowMinWidth: (showIconItem ? (iconReservedWidth + spacing) : 0)
            + Math.max(usefulLyricWidth, creditFullWidth)

        // Right mode: [icon?] [lyric] [credit?] in separate columns.
        readonly property int rightItemCount: (showIconItem ? 1 : 0) + 1 + (creditItemVisible ? 1 : 0)
        readonly property real rightGapsWidth: Math.max(0, rightItemCount - 1) * spacing
        readonly property real rightDesiredWidth: (showIconItem ? iconReservedWidth : 0)
            + lyricDesiredWidth
            + (creditItemVisible ? creditFullWidth : 0)
            + rightGapsWidth
        readonly property real rightMinWidth: (showIconItem ? iconReservedWidth : 0)
            + usefulLyricWidth
            + (creditItemVisible ? creditFullWidth : 0)
            + rightGapsWidth

        readonly property real desiredTotalWidth: {
            if (Plasmoid.pauseHidden) {
                return 0;
            }
            if (root.vertical) {
                return compactLayout.implicitWidth;
            }
            return root.isLayoutBelow ? belowDesiredWidth : rightDesiredWidth;
        }

        readonly property real minTotalWidth: {
            if (Plasmoid.pauseHidden) {
                return 0;
            }
            if (root.vertical) {
                return 0;
            }
            return root.isLayoutBelow ? belowMinWidth : rightMinWidth;
        }

        implicitWidth: desiredTotalWidth
        implicitHeight: compactLayout.implicitHeight

        Layout.preferredWidth: desiredTotalWidth
        Layout.minimumWidth: minTotalWidth
        Layout.maximumWidth: Math.max(desiredTotalWidth, minTotalWidth)
        Layout.preferredHeight: implicitHeight
        Layout.minimumHeight: implicitHeight

        onClicked: function (mouse) {
            if (creditLabel.visible) {
                const pt = creditLabel.mapFromItem(compactArea, mouse.x, mouse.y);
                if (creditLabel.hoveredLink.length > 0 || creditLabel.linkAt(pt.x, pt.y).length > 0) {
                    return;
                }
            }
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
            columns: root.vertical ? 1 : (root.isLayoutBelow ? (noteIcon.visible ? 2 : 1) : (noteIcon.visible ? (root.hasPanelCredit ? 3 : 2) : (root.hasPanelCredit ? 2 : 1)))
            rows: root.vertical ? (root.hasPanelCredit ? 3 : 2) : (root.isLayoutBelow ? 2 : 1)
            columnSpacing: Kirigami.Units.smallSpacing * 2
            rowSpacing: 0

            Kirigami.Icon {
                id: noteIcon

                // Follows the icon theme, so that it does not look out of place
                // in the panel.
                source: root.hasLyrics ? "audio-x-generic" : "media-playlist-repeat"
                Layout.row: 0
                Layout.column: 0
                Layout.rowSpan: root.isLayoutBelow ? 2 : 1
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                Layout.minimumWidth: Kirigami.Units.iconSizes.small
                Layout.alignment: Qt.AlignCenter
                visible: Plasmoid.showIcon && !Plasmoid.pauseHidden
                opacity: root.hasLyrics ? 1.0 : 0.5
            }

            TextMetrics {
                id: lineMetrics
                font: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont
                text: root.plainLine(lyricContainer.displayedLyric)
            }

            Item {
                id: lyricContainer

                property string displayedLyric: root.lyricText

                readonly property real maxLyricWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : Infinity
                readonly property real targetWidth: root.maxCharacters > 0 //
                    ? Math.min(Math.ceil(lineMetrics.advanceWidth) + 4, maxLyricWidth)
                    : Math.ceil(lineMetrics.advanceWidth) + 4
                property real animatedWidth: targetWidth

                Behavior on animatedWidth {
                    enabled: Plasmoid.fadeTransition && root.hasLyrics
                    NumberAnimation {
                        duration: 160
                        easing.type: Easing.OutQuad
                    }
                }

                implicitWidth: animatedWidth
                implicitHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricRowHeight : lyricLabel.implicitHeight

                clip: true

                Layout.row: root.vertical ? 1 : 0
                Layout.column: root.vertical ? 0 : (noteIcon.visible ? 1 : 0)
                Layout.fillWidth: true
                Layout.fillHeight: root.isLayoutBelow
                Layout.minimumWidth: root.vertical ? 0 : compactArea.usefulLyricWidth
                Layout.preferredWidth: animatedWidth
                Layout.preferredHeight: implicitHeight
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? maxLyricWidth
                    : Math.max(implicitWidth, Layout.minimumWidth)
                Layout.alignment: Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) :
                    (root.isLayoutBelow ? (Qt.AlignHCenter | Qt.AlignVCenter) : Qt.AlignCenter)

                PlasmaComponents3.Label {
                    id: lyricLabel
                    anchors.fill: parent
                    text: root.hasLyrics ? root.formatCompactLine(lyricContainer.displayedLyric) : root.escapeHtml(root.displayText)
                    textFormat: Text.StyledText
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    font: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont
                    color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    horizontalAlignment: Plasmoid.textAlignment === 1 ? Text.AlignLeft : Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    opacity: 1.0
                }

                NumberAnimation {
                    id: lineFadeAnim
                    target: lyricLabel
                    property: "opacity"
                    from: 0.35
                    to: 1.0
                    duration: 80
                    easing.type: Easing.OutQuad
                }

                Connections {
                    target: root

                    function onLyricTextChanged() {
                        lineFadeAnim.stop();
                        lyricContainer.displayedLyric = root.lyricText;
                        if (!Plasmoid.fadeTransition || !root.hasLyrics || root.lyricText.length === 0) {
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

            AttributionLabel {
                id: creditLabel

                attributionHtml: Plasmoid.attributionHtml
                fallbackText: Plasmoid.attribution
                fallbackUrl: Plasmoid.attributionUrl
                visible: root.hasLyrics && root.hasPanelCredit
                font: Kirigami.Theme.smallFont
                opacity: compactArea.containsMouse ? 0.95 : 0.75
                wrapMode: root.vertical ? Text.Wrap : Text.NoWrap

                Layout.row: (root.isLayoutBelow || root.vertical) ? (root.vertical ? 2 : 1) : 0
                Layout.column: root.vertical ? 0 : (root.isLayoutBelow ? (noteIcon.visible ? 1 : 0) : (noteIcon.visible ? 2 : 1))
                Layout.alignment: root.vertical ? Qt.AlignHCenter :
                    (root.isLayoutBelow ?
                        (Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) : (Qt.AlignHCenter | Qt.AlignVCenter)) :
                        Qt.AlignVCenter)
                Layout.preferredHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.creditRowHeight : implicitHeight
                Layout.minimumHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.creditRowHeight : 0
                Layout.fillWidth: false
                Layout.fillHeight: false
                Layout.minimumWidth: root.vertical ? 0 : implicitWidth
                Layout.preferredWidth: implicitWidth
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
                if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured && Plasmoid.apiUnreachable) {
                    return Plasmoid.apiError;
                }
                if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured && Plasmoid.apiHasNoTimings) {
                    return i18n("No synchronized lyrics available");
                }
                return i18n("No lyrics available for this track");
            }
            return i18n("No active player");
        }
        hasLyrics: root.hasLyrics

        attribution: Plasmoid.attribution
        attributionUrl: Plasmoid.attributionUrl
        attributionHtml: Plasmoid.attributionHtml
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
