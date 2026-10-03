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

    // The line with the word being sung underlined, for the panel.
    readonly property string compactLine: {
        if (!Plasmoid.wordSynced || Plasmoid.word.length === 0) {
            return escapeHtml(lyricText);
        }
        return escapeHtml(lyricText.substring(0, Plasmoid.wordStart))
            + "<u>" + escapeHtml(Plasmoid.word) + "</u>"
            + escapeHtml(lyricText.substring(Plasmoid.wordEnd));
    }

    // The line with the word being sung marked, for the popup.
    readonly property string fullLine: {
        if (!Plasmoid.wordSynced || Plasmoid.word.length === 0) {
            return escapeHtml(lyricText);
        }
        const sung = "<font color=\"" + dimmedColor + "\">" + escapeHtml(lyricText.substring(0, Plasmoid.wordStart)) + "</font>";
        const current = "<b>" + escapeHtml(Plasmoid.word) + "</b>";
        const rest = escapeHtml(lyricText.substring(Plasmoid.wordEnd));
        return sung + current + rest;
    }

    readonly property string displayText: {
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

    // Attribution is a condition of using the Spicy Lyrics API: the provider is
    // named next to the lyrics, and the contributor is linked where shown.
    readonly property bool showAttribution: Plasmoid.showAttribution //
        && Plasmoid.fromSpicyLyrics //
        && Plasmoid.attribution.length > 0
    readonly property string shortAttribution: {
        const separator = Plasmoid.attribution.indexOf(" · ");
        return "· " + (separator > 0 ? Plasmoid.attribution.substring(0, separator) : Plasmoid.attribution);
    }

    Plasmoid.backgroundHints: Plasmoid.location === PlasmaCore.Types.Desktop //
        ? PlasmaCore.Types.DefaultBackground
        : PlasmaCore.Types.NoBackground
    preferredRepresentation: compactRepresentation

    toolTipMainText: !Plasmoid.available //
        ? i18n("lrc_tty not found")
        : (Plasmoid.showTrackInfo && Plasmoid.trackInfo.length > 0 ? Plasmoid.trackInfo : i18n("Now playing"))
    toolTipSubText: hintText

    compactRepresentation: MouseArea {
        id: compactArea

        hoverEnabled: true
        activeFocusOnTab: true
        acceptedButtons: Qt.LeftButton

        Accessible.role: Accessible.StaticText
        Accessible.name: root.displayText

        implicitWidth: compactLayout.implicitWidth
        implicitHeight: compactLayout.implicitHeight

        Layout.minimumWidth: implicitWidth
        Layout.minimumHeight: implicitHeight
        Layout.maximumWidth: implicitWidth

        onClicked: root.expanded = !root.expanded

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
                Layout.alignment: Qt.AlignCenter
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : implicitWidth
            }

            PlasmaComponents3.Label {
                id: attributionLabel

                // Tiny, and it expands to the full credit while the pointer is
                // on the widget.
                text: compactArea.containsMouse ? Plasmoid.attribution : root.shortAttribution
                color: Kirigami.Theme.textColor
                opacity: compactArea.containsMouse ? 0.85 : 0.5
                elide: Text.ElideRight
                visible: root.showAttribution
                Layout.alignment: Qt.AlignVCenter
                Layout.maximumWidth: Math.ceil(characterWidth.advanceWidth) * 22
            }
        }
    }

    fullRepresentation: LyricPopup {
        id: fullView

        Layout.minimumWidth: Kirigami.Units.gridUnit * 14
        Layout.minimumHeight: Kirigami.Units.gridUnit * 4
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24

        trackInfo: Plasmoid.trackInfo
        showTrackInfo: Plasmoid.showTrackInfo

        line: root.fullLine
        placeholder: root.displayText
        hasLyrics: root.hasLyrics

        attribution: root.escapeHtml(Plasmoid.attribution)
        attributionUrl: Plasmoid.attributionUrl
        showAttribution: root.showAttribution
    }

    // Only used to find out how wide a single character is, so that the compact
    // representation can be limited to a configurable number of characters.
    TextMetrics {
        id: characterWidth

        font: lyricLabel.font
        text: "0"
    }
}