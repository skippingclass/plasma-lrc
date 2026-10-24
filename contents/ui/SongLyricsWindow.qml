/* SPDX-License-Identifier: MIT */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Window {
    id: root

    width: Kirigami.Units.gridUnit * 30
    height: Kirigami.Units.gridUnit * 34
    minimumWidth: Kirigami.Units.gridUnit * 22
    minimumHeight: Kirigami.Units.gridUnit * 14
    transientParent: null
    color: Kirigami.Theme.backgroundColor
    title: i18n("Song lyrics — Plasma LRC")

    property string trackInfo: ""
    property string attribution: ""
    property string attributionUrl: ""
    property string attributionHtml: ""
    property bool apiLyrics: false
    property double validUntilMs: 0
    property bool isExpired: false

    readonly property real maxTimerInterval: 2147483647
    readonly property string expirationPlaceholder: i18n("Lyrics expired. Please reopen for fresh lyrics.")
    readonly property string placeholderText: isExpired ? expirationPlaceholder : ""

    readonly property alias lyricsTextItem: lyricsText
    readonly property alias copySelectionButtonItem: copySelectionButton
    readonly property alias copyAllButtonItem: copyAllButton

    function expireSnapshot() {
        expiryTimer.stop();
        root.isExpired = true;
        root.trackInfo = "";
        root.attribution = "";
        root.attributionUrl = "";
        root.attributionHtml = "";
        lyricsText.text = "";
        lyricsText.deselect();
    }

    function discardApiSnapshot() {
        expiryTimer.stop();
        root.isExpired = false;
        root.apiLyrics = false;
        root.validUntilMs = 0;
        root.trackInfo = "";
        root.attribution = "";
        root.attributionUrl = "";
        root.attributionHtml = "";
        lyricsText.text = "";
        lyricsText.deselect();
    }

    function checkExpiry() {
        if (!root.apiLyrics) {
            expiryTimer.stop();
            return;
        }
        if (root.isExpired) {
            expiryTimer.stop();
            return;
        }
        if (root.validUntilMs <= 0 || isNaN(root.validUntilMs)) {
            expireSnapshot();
            return;
        }
        const now = Date.now();
        const remaining = root.validUntilMs - now;
        if (remaining <= 0) {
            expireSnapshot();
        } else {
            expiryTimer.interval = Math.min(remaining, maxTimerInterval);
            expiryTimer.restart();
        }
    }

    Timer {
        id: expiryTimer
        repeat: false
        onTriggered: root.checkExpiry()
    }

    onVisibleChanged: {
        if (visible) {
            checkExpiry();
        } else {
            if (root.apiLyrics) {
                discardApiSnapshot();
            }
        }
    }

    onActiveChanged: {
        if (active) {
            checkExpiry();
        }
    }

    onClosing: function(close) {
        if (root.apiLyrics) {
            discardApiSnapshot();
        }
    }

    Connections {
        target: Qt.application
        function onStateChanged() {
            if (root.visible) {
                root.checkExpiry();
            }
        }
    }

    // Keep a snapshot so a track change cannot replace a selection being copied.
    function openSong(track, lyrics, offset, credit, creditUrl, creditHtml, validUntilMs, apiLyrics) {
        const isApi = Boolean(apiLyrics);
        const deadline = Number(validUntilMs) || 0;
        const now = Date.now();

        root.apiLyrics = isApi;
        root.validUntilMs = deadline;

        if (isApi && (deadline <= 0 || now >= deadline)) {
            expireSnapshot();
            show();
            raise();
            requestActivate();
            return;
        }

        root.isExpired = false;
        root.trackInfo = track || "";
        root.attribution = credit || "";
        root.attributionUrl = creditUrl || "";
        root.attributionHtml = (creditHtml !== undefined && creditHtml !== null) ? creditHtml : "";
        lyricsText.text = lyrics || "";

        if (isApi) {
            checkExpiry();
        } else {
            expiryTimer.stop();
        }

        show();
        raise();
        requestActivate();
        Qt.callLater(function() {
            lyricsText.forceActiveFocus();
            if (offset >= 0 && offset < lyricsText.length) {
                const end = lyricsText.text.indexOf("\n", offset);
                lyricsText.select(offset, end >= 0 ? end : lyricsText.length);
            } else {
                lyricsText.cursorPosition = 0;
            }
        });
    }

    Shortcut {
        sequences: [StandardKey.Close]
        onActivated: root.close()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: root.close()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            text: root.trackInfo
            textFormat: Text.PlainText
            font.bold: true
            wrapMode: Text.Wrap
            visible: text.length > 0
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Controls.TextArea {
                id: lyricsText
                objectName: "lyricsText"
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                wrapMode: TextEdit.Wrap
                Accessible.name: i18n("Song lyrics")
                placeholderText: root.placeholderText
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.attributionHtml.length > 0 || root.attribution.length > 0

            AttributionLabel {
                Layout.fillWidth: true
                attributionHtml: root.attributionHtml
                fallbackText: root.attribution
                fallbackUrl: root.attributionUrl
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Controls.Button {
                id: copySelectionButton
                objectName: "copySelectionButton"
                text: i18n("Copy selection")
                icon.name: "edit-copy"
                visible: !root.apiLyrics
                enabled: !root.apiLyrics && lyricsText.selectedText.length > 0
                onClicked: lyricsText.copy()
            }
            Controls.Button {
                id: copyAllButton
                objectName: "copyAllButton"
                text: i18n("Copy all lyrics")
                visible: !root.apiLyrics
                enabled: !root.apiLyrics && lyricsText.length > 0
                onClicked: {
                    const start = lyricsText.selectionStart;
                    const end = lyricsText.selectionEnd;
                    lyricsText.selectAll();
                    lyricsText.copy();
                    lyricsText.select(start, end);
                }
            }
            Item { Layout.fillWidth: true }
            Controls.Button {
                text: i18n("Close")
                onClicked: root.close()
            }
        }
    }
}
