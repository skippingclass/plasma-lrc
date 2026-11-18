/* SPDX-License-Identifier: MIT */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Window {
    id: root

    width: Kirigami.Units.gridUnit * 36
    height: Kirigami.Units.gridUnit * 26
    minimumWidth: Kirigami.Units.gridUnit * 26
    minimumHeight: Kirigami.Units.gridUnit * 18
    transientParent: null
    color: Kirigami.Theme.backgroundColor
    title: i18n("Choose lyrics match — Plasma LRC")

    property var plasmoidApplet: null
    property string songKey: ""
    readonly property var controller: plasmoidApplet ? plasmoidApplet.candidateController : null
    property double cooldownClock: Date.now()
    readonly property int retryAfterSeconds: controller && controller.status === "rate_limited"
        ? Math.max(0, Math.ceil((controller.retryAvailableAtMs - cooldownClock) / 1000))
        : 0

    function openWindow(appletObj) {
        if (appletObj) {
            root.plasmoidApplet = appletObj;
        }
        const currentSong = root.plasmoidApplet ? root.plasmoidApplet.songKey : "";
        if (!root.visible || root.songKey !== currentSong) {
            rememberChoice.checked = false;
            root.songKey = currentSong;
        }
        if (root.plasmoidApplet && root.plasmoidApplet.openChooseMatchWindow) {
            root.plasmoidApplet.openChooseMatchWindow();
        }
        root.show();
        root.requestActivate();
    }

    onClosing: {
        if (root.controller) {
            root.controller.close();
        }
    }

    Connections {
        target: root.controller
        function onStatusChanged() {
            root.cooldownClock = Date.now();
        }
        function onCandidateLyricsLoaded() {
            if (rememberChoice.checked && root.plasmoidApplet && root.plasmoidApplet.isManualCandidateSelected) {
                root.plasmoidApplet.rememberSongLyrics(root.songKey, root.plasmoidApplet.songLyricOffset);
            }
            root.close();
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: root.visible && root.retryAfterSeconds > 0
        onTriggered: root.cooldownClock = Date.now()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.smallSpacing

        // Header: Title and Track Info
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                text: i18n("Choose Lyrics Match")
                level: 2
            }

            Controls.Label {
                text: root.plasmoidApplet ? root.plasmoidApplet.trackInfo : ""
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
                visible: text.length > 0
            }
        }

        // Status banner / Error indicator
        Kirigami.InlineMessage {
            id: errorMessage
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            visible: root.controller ? (root.controller.status === "error") : false
            text: root.controller ? (root.controller.errorMessage || i18n("Failed to fetch lyrics")) : ""
            showCloseButton: false
        }

        // Non-blocking refresh warning message
        Kirigami.InlineMessage {
            id: refreshWarningMessage
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            visible: root.controller ? (root.controller.refreshErrorMessage.length > 0) : false
            text: root.controller ? root.controller.refreshErrorMessage : ""
            showCloseButton: true
            onVisibleChanged: {
                if (!visible && root.controller) {
                    root.controller.clearRefreshErrorMessage();
                }
            }
        }

        // Searching / Loading indicator
        RowLayout {
            Layout.fillWidth: true
            visible: root.controller ? (root.controller.searching || root.controller.loading) : false
            spacing: Kirigami.Units.smallSpacing

            Controls.BusyIndicator {
                running: parent.visible
                Layout.preferredWidth: Kirigami.Units.gridUnit * 1.5
                Layout.preferredHeight: Kirigami.Units.gridUnit * 1.5
            }

            Controls.Label {
                text: root.controller && root.controller.loading
                    ? i18n("Downloading selected lyrics…")
                    : i18n("Searching across providers…")
                Layout.fillWidth: true
            }
        }

        // No matches indicator
        Controls.Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: i18n("No matching lyrics found across enabled providers.")
            font.italic: true
            color: Kirigami.Theme.disabledTextColor
            visible: root.controller ? (root.controller.status === "nomatches") : false
        }

        // No response indicator
        Controls.Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: i18n("No response received from lyric providers. Check connection and try again.")
            font.italic: true
            color: Kirigami.Theme.disabledTextColor
            visible: root.controller ? (root.controller.status === "no_response") : false
        }

        Controls.Label {
            objectName: "rateLimitedMessage"
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            visible: root.controller !== null && root.controller.status === "rate_limited"
            text: root.retryAfterSeconds > 0
                ? i18np("Providers are rate limited. Try again in %1 second.",
                        "Providers are rate limited. Try again in %1 seconds.", root.retryAfterSeconds)
                : i18n("You can search again now.")
        }

        // All providers disabled indicator
        Controls.Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: i18n("All lyric providers are disabled in settings.")
            font.italic: true
            color: Kirigami.Theme.disabledTextColor
            visible: root.controller ? (root.controller.status === "all_disabled") : false
        }

        // Candidate List
        Controls.ScrollView {
            id: scrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller ? (root.controller.status !== "nomatches" && root.controller.status !== "no_response" && root.controller.status !== "all_disabled") : true

            ListView {
                id: candidateList
                model: root.controller ? root.controller.candidates : []
                currentIndex: root.controller ? root.controller.selectedIndex : -1
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                delegate: Controls.ItemDelegate {
                    id: delegateItem
                    required property var modelData
                    required property int index

                    width: candidateList.width
                    highlighted: root.controller && root.controller.selectedIndex === delegateItem.index

                    Controls.ToolTip.visible: delegateItem.hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    Controls.ToolTip.text: (delegateItem.modelData.title || "") + "\n" + (delegateItem.modelData.artist || "")

                    onClicked: {
                        if (root.controller) {
                            root.controller.selectedIndex = delegateItem.index;
                        }
                    }

                    onDoubleClicked: {
                        if (root.controller) {
                            root.controller.selectedIndex = delegateItem.index;
                            root.controller.useSelected();
                        }
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        // Provider Badge
                        Rectangle {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5.5
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 1.6
                            color: Kirigami.Theme.alternateBackgroundColor
                            radius: Kirigami.Units.smallSpacing

                            Controls.Label {
                                anchors.centerIn: parent
                                text: delegateItem.modelData.sourceName || ""
                                font.bold: true
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                elide: Text.ElideRight
                                width: parent.width - Kirigami.Units.smallSpacing * 2
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }

                        // Track Title & Artist
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Controls.Label {
                                text: delegateItem.modelData.title || ""
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Controls.Label {
                                text: delegateItem.modelData.artist || ""
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        // Duration
                        Controls.Label {
                            text: delegateItem.modelData.durationText || ""
                            font.family: "monospace"
                            color: Kirigami.Theme.disabledTextColor
                        }

                        // Sync Quality
                        Rectangle {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 4.2
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 1.4
                            color: {
                                const q = delegateItem.modelData.syncQualityText;
                                if (q === "Word") return Kirigami.Theme.positiveBackgroundColor || "#1b4d24";
                                if (q === "Line") return Kirigami.Theme.neutralBackgroundColor || "#23415c";
                                return Kirigami.Theme.backgroundColor;
                            }
                            radius: Kirigami.Units.smallSpacing
                            border.color: Kirigami.Theme.disabledTextColor
                            border.width: 1

                            Controls.Label {
                                anchors.centerIn: parent
                                text: delegateItem.modelData.syncQualityText || ""
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                color: {
                                    const q = delegateItem.modelData.syncQualityText;
                                    if (q === "Word") return Kirigami.Theme.positiveTextColor || "#a6e3a1";
                                    if (q === "Line") return Kirigami.Theme.neutralTextColor || "#89b4fa";
                                    return Kirigami.Theme.textColor;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Bottom Actions
        Controls.CheckBox {
            id: rememberChoice
            text: i18n("Remember for this song")
            visible: root.songKey.length > 0 && root.controller !== null && root.controller.hasUsableResults
            enabled: !root.controller || !root.controller.loading
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                text: i18n("Search again")
                icon.name: "view-refresh"
                enabled: root.controller !== null && !root.controller.searching && !root.controller.loading && root.retryAfterSeconds === 0
                onClicked: {
                    if (root.controller) {
                        root.controller.searchAgain();
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }

            Controls.Button {
                text: i18n("Close")
                icon.name: "dialog-close"
                onClicked: root.close()
            }

            Controls.Button {
                objectName: "useSelectedButton"
                text: i18n("Use selected")
                icon.name: "dialog-ok"
                highlighted: true
                enabled: root.controller ? (root.controller.selectedIndex >= 0
                    && (root.controller.status === "searching" || root.controller.status === "ready" || root.controller.status === "error")) : false
                onClicked: {
                    if (root.controller) {
                        root.controller.useSelected();
                    }
                }
            }
        }
    }
}
