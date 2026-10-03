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
            columns: root.vertical ? 1 : 2
            rows: root.vertical ? 2 : 1
            columnSpacing: Kirigami.Units.smallSpacing * 2
            rowSpacing: Kirigami.Units.smallSpacing

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

                text: root.displayText
                color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
                Layout.alignment: Qt.AlignCenter
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : implicitWidth
            }
        }
    }

    fullRepresentation: ColumnLayout {
        id: fullView

        Layout.minimumWidth: Kirigami.Units.gridUnit * 14
        Layout.minimumHeight: Kirigami.Units.gridUnit * 4
        Layout.maximumWidth: Kirigami.Units.gridUnit * 24
        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents3.Label {
            Layout.fillWidth: true

            text: Plasmoid.trackInfo
            color: Kirigami.Theme.textColor
            opacity: 0.75
            elide: Text.ElideRight
            visible: Plasmoid.showTrackInfo && text.length > 0
        }

        PlasmaComponents3.Label {
            Layout.fillWidth: true
            Layout.fillHeight: true

            text: root.displayText
            color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Only used to find out how wide a single character is, so that the compact
    // representation can be limited to a configurable number of characters.
    TextMetrics {
        id: characterWidth

        font: lyricLabel.font
        text: "0"
    }
}
