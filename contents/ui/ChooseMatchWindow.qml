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
    readonly property var controller: plasmoidApplet ? plasmoidApplet.candidateController : null

    function openWindow(appletObj) {
        if (appletObj) {
            root.plasmoidApplet = appletObj;
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
        function onCandidateLyricsLoaded() {
            root.close();
        }
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
            text: i18n("No matching lyrics found.")
            font.italic: true
            color: Kirigami.Theme.disabledTextColor
            visible: root.controller ? (root.controller.status === "nomatches") : false
        }

        // Candidate List
        Controls.ScrollView {
            id: scrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.controller || root.controller.status !== "nomatches"

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
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                text: i18n("Search again")
                icon.name: "view-refresh"
                enabled: root.controller ? (!root.controller.searching && !root.controller.loading) : false
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
                text: i18n("Use selected")
                icon.name: "dialog-ok"
                highlighted: true
                enabled: root.controller ? (root.controller.selectedIndex >= 0 && root.controller.status === "ready") : false
                onClicked: {
                    if (root.controller) {
                        root.controller.useSelected();
                    }
                }
            }
        }
    }
}
