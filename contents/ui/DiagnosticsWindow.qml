/* SPDX-License-Identifier: MIT */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Window {
    id: root

    width: Kirigami.Units.gridUnit * 38
    height: Kirigami.Units.gridUnit * 32
    minimumWidth: Kirigami.Units.gridUnit * 28
    minimumHeight: Kirigami.Units.gridUnit * 20
    transientParent: null
    color: Kirigami.Theme.backgroundColor
    title: i18n("Diagnostics — Plasma LRC")

    property var plasmoidApplet: null
    property var diagData: ({})
    property var searchData: ({})
    property var syncData: ({})
    property var providerList: []

    function refreshData() {
        var app = root.plasmoidApplet;
        if (!app && typeof Plasmoid !== "undefined") {
            app = Plasmoid;
        }
        if (app && app.diagnosticData) {
            var data = app.diagnosticData();
            root.diagData = data || {};
            root.searchData = (data && data.search) ? data.search : {};
            root.syncData = (data && data.sync) ? data.sync : {};
            root.providerList = (data && data.search && data.search.providers) ? data.search.providers : [];
        }
    }

    function openDiagnostics(appletObj) {
        if (appletObj) {
            root.plasmoidApplet = appletObj;
        }
        refreshData();
        root.show();
        root.requestActivate();
    }

    Timer {
        id: refreshTimer
        interval: 500
        repeat: true
        running: root.visible
        onTriggered: root.refreshData()
    }

    Timer {
        id: copyFeedbackTimer
        interval: 1500
        repeat: false
    }

    onVisibleChanged: {
        if (visible) {
            refreshData();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.smallSpacing

        // Top bar: Tabs and Actions
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.TabBar {
                id: tabGroup
                Layout.fillWidth: true

                Controls.TabButton {
                    text: i18n("Search")
                    icon.name: "system-search"
                }
                Controls.TabButton {
                    text: i18n("Synchronization")
                    icon.name: "media-playback-start"
                }
            }

            Controls.Button {
                text: i18n("Refresh")
                icon.name: "view-refresh"
                onClicked: root.refreshData()
            }

            Controls.Button {
                id: copyBtn
                text: copyFeedbackTimer.running ? i18n("Copied!") : i18n("Copy diagnostic report")
                icon.name: copyFeedbackTimer.running ? "checkmark" : "edit-copy"
                onClicked: {
                    var app = root.plasmoidApplet;
                    if (!app && typeof Plasmoid !== "undefined") {
                        app = Plasmoid;
                    }
                    if (app && app.copyDiagnosticReport) {
                        app.copyDiagnosticReport();
                        copyFeedbackTimer.restart();
                    }
                }
            }
        }

        StackLayout {
            id: contentStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabGroup.currentIndex

            // TAB 1: SEARCH
            Controls.ScrollView {
                id: searchScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: searchScroll.availableWidth
                    spacing: Kirigami.Units.largeSpacing

                    // 1. Metadata
                    Kirigami.Heading {
                        text: i18n("Track Metadata")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("Raw title:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.rawTitle || i18n("(none)")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            text: i18n("Raw artist:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.rawArtist || i18n("(none)")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            text: i18n("Raw album:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.rawAlbum || i18n("(none)")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            text: i18n("Raw duration:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.rawDurationFormatted || i18n("Unknown")
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Normalized title:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.normalizedTitle || i18n("(none)")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            text: i18n("Normalized artist:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.normalizedArtist || i18n("(none)")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    // 2. Current Selection
                    Kirigami.Heading {
                        text: i18n("Current Lyrics Selection")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("Active source:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.searchData.currentSource || i18n("None")) + " (" + (root.searchData.currentProviderId || "none") + ")"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Sync quality:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.searchData.syncQuality || i18n("None")
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Search generation:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.searchData.searchGeneration !== undefined ? root.searchData.searchGeneration : 0) + (root.searchData.isSearching ? " " + i18n("(search active…)") : " " + i18n("(finished)"))
                            Layout.fillWidth: true
                        }
                    }

                    // 3. Provider Pipeline Table
                    Kirigami.Heading {
                        text: i18n("Provider Pipeline Status")
                        level: 3
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Repeater {
                            model: root.providerList

                            delegate: Kirigami.AbstractCard {
                                id: card
                                required property var modelData
                                Layout.fillWidth: true

                                header: RowLayout {
                                    spacing: Kirigami.Units.smallSpacing

                                    Kirigami.Heading {
                                        text: card.modelData.name || card.modelData.id
                                        level: 4
                                        Layout.fillWidth: true
                                    }

                                    Controls.Label {
                                        text: (card.modelData.status || "").toUpperCase()
                                        font.bold: true
                                        color: {
                                            var st = card.modelData.status || "";
                                            if (st === "selected") return Kirigami.Theme.positiveTextColor;
                                            if (st === "provisional") return Kirigami.Theme.neutralTextColor;
                                            if (st === "searching") return Kirigami.Theme.focusColor;
                                            if (st === "failed") return Kirigami.Theme.negativeTextColor;
                                            if (st === "disabled") return Kirigami.Theme.disabledTextColor;
                                            return Kirigami.Theme.textColor;
                                        }
                                    }
                                }

                                contentItem: ColumnLayout {
                                    spacing: Kirigami.Units.smallSpacing / 2

                                    RowLayout {
                                        Controls.Label {
                                            text: i18n("Quality:")
                                            font.bold: true
                                        }
                                        Controls.Label {
                                            text: card.modelData.quality || i18n("None")
                                        }

                                        Item { Layout.preferredWidth: Kirigami.Units.largeSpacing }

                                        Controls.Label {
                                            text: i18n("Cache:")
                                            font.bold: true
                                        }
                                        Controls.Label {
                                            text: card.modelData.cache || i18n("None")
                                        }
                                    }

                                    RowLayout {
                                        visible: card.modelData.reason && card.modelData.reason.length > 0
                                        Controls.Label {
                                            text: i18n("Details / Reason:")
                                            font.bold: true
                                            color: Kirigami.Theme.disabledTextColor
                                        }
                                        Controls.Label {
                                            text: card.modelData.reason || ""
                                            color: Kirigami.Theme.disabledTextColor
                                            wrapMode: Text.Wrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // TAB 2: SYNCHRONIZATION
            Controls.ScrollView {
                id: syncScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: syncScroll.availableWidth
                    spacing: Kirigami.Units.largeSpacing

                    // 1. Playback
                    Kirigami.Heading {
                        text: i18n("Playback & MPRIS")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("Player:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.syncData.player || i18n("None")
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Playback status:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.syncData.playbackStatus || i18n("Unknown")
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Position valid:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: root.syncData.positionValid ? i18n("Yes") : i18n("No (no valid position)")
                            color: root.syncData.positionValid ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                            Layout.fillWidth: true
                        }
                    }

                    // 2. Position Calculation
                    Kirigami.Heading {
                        text: i18n("Position Calculation (milliseconds)")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("Base MPRIS position:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.syncData.baseMprisPositionMs !== undefined ? root.syncData.baseMprisPositionMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Measurement age:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "+" + (root.syncData.measurementAgeMs !== undefined ? root.syncData.measurementAgeMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Extrapolated position:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.syncData.extrapolatedPositionMs !== undefined ? root.syncData.extrapolatedPositionMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Gapless lead buffer:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "-" + (root.syncData.gaplessLeadMs !== undefined ? root.syncData.gaplessLeadMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Audio lead compensation:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "-" + (root.syncData.audioLeadCompensationMs !== undefined ? root.syncData.audioLeadCompensationMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("User lyric offset:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "-" + (root.syncData.userOffsetMs !== undefined ? root.syncData.userOffsetMs : 0) + " ms"
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Final lyrics position:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.syncData.finalPositionMs !== undefined ? root.syncData.finalPositionMs : 0) + " ms"
                            font.bold: true
                            Layout.fillWidth: true
                        }
                    }

                    Controls.Label {
                        text: root.syncData.calculationFormula || ""
                        font.family: "monospace"
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        color: Kirigami.Theme.disabledTextColor
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    // 3. Vocals & Cursors
                    Kirigami.Heading {
                        text: i18n("Vocal & Line Cursors")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("Line index:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: (root.syncData.lineIndex !== undefined ? root.syncData.lineIndex : -1) + " / " + (root.syncData.totalLines !== undefined ? root.syncData.totalLines : 0)
                            Layout.fillWidth: true
                        }

                        Controls.Label {
                            text: i18n("Primary vocal word:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "\"" + (root.syncData.primaryWord || "") + "\" [range: " + (root.syncData.primaryWordStart || 0) + ".." + (root.syncData.primaryWordEnd || 0) + ", cursor: " + (root.syncData.primaryWordCursor || 0) + ", sungEnd: " + (root.syncData.primarySungEnd || 0) + "]"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            text: i18n("Backing vocal phrase:")
                            font.bold: true
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: "\"" + (root.syncData.backingText || "") + "\" [range: " + (root.syncData.backingWordStart || 0) + ".." + (root.syncData.backingWordEnd || 0) + ", sungEnd: " + (root.syncData.backingSungEnd || 0) + "]"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    // 4. State Flags
                    Kirigami.Heading {
                        text: i18n("State Flags")
                        level: 3
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label { text: i18n("Paused:"); font.bold: true }
                        Controls.Label { text: root.syncData.isPaused ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Pause hidden:"); font.bold: true }
                        Controls.Label { text: root.syncData.isPauseHidden ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Discarded:"); font.bold: true }
                        Controls.Label { text: root.syncData.isDiscarded ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Blacklisted:"); font.bold: true }
                        Controls.Label { text: root.syncData.isBlacklisted ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Intro:"); font.bold: true }
                        Controls.Label { text: root.syncData.isIntro ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Break:"); font.bold: true }
                        Controls.Label { text: root.syncData.isBreak ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Lyrics usable:"); font.bold: true }
                        Controls.Label { text: root.syncData.hasLyrics ? i18n("Yes") : i18n("No") }

                        Controls.Label { text: i18n("Word synced:"); font.bold: true }
                        Controls.Label { text: root.syncData.wordSynced ? i18n("Yes") : i18n("No") }
                    }
                }
            }
        }
    }
}
