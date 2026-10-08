/*
 * SPDX-FileCopyrightText: 2026 skippingclass
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.plasma.plasmoid
import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

// SimpleKCM is what a Plasma 6 config page is supposed to be rooted in: the
// dialog sets `title` on it, and it brings its own scrolling and padding.
KCM.SimpleKCM {
    id: root

    property alias cfg_blacklistedTracks: rawField.text

    readonly property var targetApplet: {
        if (typeof Plasmoid !== "undefined" && Plasmoid) {
            if (Plasmoid.applet) {
                return Plasmoid.applet;
            }
            return Plasmoid;
        }
        return null;
    }

    readonly property string currentTrack: {
        if (targetApplet && targetApplet.trackInfo && targetApplet.trackInfo.length > 0) {
            return targetApplet.trackInfo;
        }
        return "";
    }

    QQC2.TextField {
        id: rawField
        visible: false
        onTextChanged: {
            if (!syncingToRaw) {
                syncFromRaw();
            }
        }
    }

    property bool syncingToRaw: false

    ListModel {
        id: tracksModel
    }

    function syncFromRaw() {
        tracksModel.clear();
        let sourceText = rawField.text;
        if ((!sourceText || sourceText.trim().length === 0) && targetApplet && targetApplet.blacklistedTracks) {
            sourceText = targetApplet.blacklistedTracks;
            syncingToRaw = true;
            rawField.text = sourceText;
            syncingToRaw = false;
        }

        if (!sourceText) {
            return;
        }

        const parts = sourceText.split(";");
        for (let i = 0; i < parts.length; ++i) {
            const item = parts[i].trim();
            if (item.length > 0) {
                tracksModel.append({ "track": item });
            }
        }
    }

    function syncToRaw() {
        syncingToRaw = true;
        let items = [];
        for (let i = 0; i < tracksModel.count; ++i) {
            items.push(tracksModel.get(i).track);
        }
        rawField.text = items.join("; ");
        syncingToRaw = false;
    }

    Component.onCompleted: {
        syncFromRaw();
    }

    Connections {
        target: root.targetApplet
        ignoreUnknownSignals: true
        function onTrackBlacklistChanged() {
            if (root.targetApplet && root.targetApplet.blacklistedTracks !== undefined) {
                root.syncingToRaw = true;
                rawField.text = root.targetApplet.blacklistedTracks;
                root.syncingToRaw = false;
                root.syncFromRaw();
            }
        }
    }

    function addTrack(track) {
        const trimmed = track.trim();
        if (trimmed.length === 0) {
            return;
        }
        for (let i = 0; i < tracksModel.count; ++i) {
            if (tracksModel.get(i).track.toLowerCase() === trimmed.toLowerCase()) {
                return;
            }
        }
        tracksModel.append({ "track": trimmed });
        syncToRaw();

        if (targetApplet && typeof targetApplet.addBlacklistTrack === "function") {
            targetApplet.addBlacklistTrack(trimmed);
        }
    }

    function removeTrack(index) {
        if (index >= 0 && index < tracksModel.count) {
            const trackName = tracksModel.get(index).track;
            tracksModel.remove(index);
            syncToRaw();

            if (targetApplet && typeof targetApplet.removeBlacklistTrack === "function") {
                targetApplet.removeBlacklistTrack(trackName);
            }
        }
    }

    function clearAll() {
        tracksModel.clear();
        syncToRaw();

        if (targetApplet && typeof targetApplet.clearBlacklist === "function") {
            targetApplet.clearBlacklist();
        }
    }

    Kirigami.FormLayout {
        id: formLayout

        RowLayout {
            Kirigami.FormData.label: i18n("Add track:")
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: newTrackField
                Layout.fillWidth: true
                placeholderText: i18n("Artist - Title (or song title)")
                onAccepted: {
                    if (text.trim().length > 0) {
                        root.addTrack(text);
                        text = "";
                    }
                }
            }

            QQC2.Button {
                text: i18n("Add")
                icon.name: "list-add"
                enabled: newTrackField.text.trim().length > 0
                onClicked: {
                    root.addTrack(newTrackField.text);
                    newTrackField.text = "";
                }
            }
        }

        QQC2.Button {
            visible: root.currentTrack.length > 0
            text: i18n("Add currently playing: %1", root.currentTrack)
            icon.name: "media-optical-audio"
            Layout.fillWidth: true
            onClicked: {
                root.addTrack(root.currentTrack);
            }
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Tracks matching these names will never show lyrics (case-insensitive substring match). You can also blacklist or unblacklist songs directly from the widget context menu.")
        }

        Kirigami.Separator {
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.columnSpan: 2
            Layout.fillWidth: true

            Kirigami.Heading {
                level: 3
                text: tracksModel.count === 0
                    ? i18n("Blacklisted Tracks")
                    : i18n("Blacklisted Tracks (%1)", tracksModel.count)
                Layout.fillWidth: true
            }

            QQC2.Button {
                text: i18n("Clear All")
                icon.name: "edit-clear-all"
                visible: tracksModel.count > 0
                onClicked: root.clearAll()
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            visible: tracksModel.count === 0
            icon.name: "media-optical-audio"
            text: i18n("No tracks blacklisted")
            explanation: i18n("When lyrics for a track are broken or unsynced, blacklist the track to suppress it.")
        }

        Repeater {
            model: tracksModel

            delegate: Rectangle {
                id: itemDelegate
                required property string track
                required property int index

                Layout.columnSpan: 2
                Layout.fillWidth: true
                implicitHeight: rowLayout.implicitHeight + Kirigami.Units.smallSpacing * 2
                radius: Kirigami.Units.smallSpacing
                color: Kirigami.Theme.alternateBackgroundColor
                border.width: 1
                border.color: Kirigami.Theme.separatorColor

                RowLayout {
                    id: rowLayout
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: "audio-x-generic"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: Kirigami.Units.iconSizes.small
                    }

                    QQC2.Label {
                        text: itemDelegate.track
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    QQC2.ToolButton {
                        icon.name: "edit-delete"
                        QQC2.ToolTip.visible: hovered
                        QQC2.ToolTip.text: i18n("Remove from blacklist")
                        onClicked: root.removeTrack(itemDelegate.index)
                    }
                }
            }
        }
    }
}
