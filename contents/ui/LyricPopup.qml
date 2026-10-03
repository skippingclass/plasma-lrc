/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

/**
 * What a click on the widget shows: the track, the line with the word being sung
 * highlighted, and the credit for the lyrics.
 *
 * The text is passed in already prepared for rich text, because it comes from a
 * third party and has to be escaped somewhere that can see all of it.
 */
ColumnLayout {
    id: root

    /// Track as MPRIS reports it.
    property string trackInfo
    property bool showTrackInfo: true

    /// The line, with the current word already marked up.
    property string line
    /// Shown instead of the line when there is nothing to sing along with.
    property string placeholder
    property bool hasLyrics: true

    /// Who the lyrics came from, when it is worth naming.
    property string attribution
    property string attributionUrl
    property bool showAttribution: false

    spacing: Kirigami.Units.smallSpacing

    PlasmaComponents3.Label {
        Layout.fillWidth: true

        text: root.trackInfo
        color: Kirigami.Theme.textColor
        opacity: 0.75
        elide: Text.ElideRight
        visible: root.showTrackInfo && text.length > 0
    }

    PlasmaComponents3.Label {
        Layout.fillWidth: true
        Layout.fillHeight: true

        text: root.hasLyrics ? root.line : root.placeholder
        textFormat: Text.RichText
        color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
    }

    PlasmaComponents3.Label {
        Layout.fillWidth: true

        text: root.attributionUrl.length > 0 //
            ? "<a href=\"" + root.attributionUrl + "\">" + root.attribution + "</a>"
            : root.attribution
        textFormat: Text.StyledText
        color: Kirigami.Theme.textColor
        opacity: 0.6
        font.pointSize: Math.max(6, font.pointSize - 1)
        wrapMode: Text.Wrap
        visible: root.showAttribution
        onLinkActivated: function (link) {
            Qt.openUrlExternally(link);
        }
    }
}