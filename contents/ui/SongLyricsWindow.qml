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

    property var plasmoidApplet: null
    property bool liveMode: false
    readonly property int activeLyricIndex: liveMode && plasmoidApplet && !plasmoidApplet.pauseHidden
        ? plasmoidApplet.lineIndex : -1
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

    function openLiveSong() {
        if (root.apiLyrics) {
            discardApiSnapshot();
        }
        root.liveMode = true;
        show();
        raise();
        requestActivate();
        Qt.callLater(root.followCurrentLine);
    }

    function openCurrentSnapshot() {
        const applet = root.plasmoidApplet;
        openSong(applet.trackInfo, applet.fullLyrics, applet.currentLyricOffset(),
            applet.attribution, applet.attributionUrl, applet.attributionHtml,
            applet.lyricsValidUntilMs, applet.fromSpicyLyrics);
    }

    function followCurrentLine() {
        if (root.visible && root.liveMode && followPlayback.checked
            && root.activeLyricIndex >= 0 && root.activeLyricIndex < liveLyrics.count) {
            liveLyrics.positionViewAtIndex(root.activeLyricIndex, ListView.Center);
        }
    }

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function markWord(text, start, end) {
        if (end <= start) {
            return escapeHtml(text);
        }
        return escapeHtml(text.slice(0, start)) + "<b><u>" + escapeHtml(text.slice(start, end))
            + "</u></b>" + escapeHtml(text.slice(end));
    }

    function liveLineText(fallback) {
        const applet = root.plasmoidApplet;
        const main = markWord(applet.text, applet.wordStart, applet.wordEnd);
        const backing = markWord(applet.backingText, applet.backingWordStart, applet.backingWordEnd);
        return (main && backing ? main + "<br/>" + backing : main || backing) || escapeHtml(fallback);
    }

    onActiveLyricIndexChanged: Qt.callLater(root.followCurrentLine)

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
        root.liveMode = false;
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
            text: root.liveMode && root.plasmoidApplet ? root.plasmoidApplet.trackInfo : root.trackInfo
            textFormat: Text.PlainText
            font.bold: true
            wrapMode: Text.Wrap
            visible: text.length > 0
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.plasmoidApplet !== null

            Controls.Button {
                text: root.liveMode ? i18n("Read text") : i18n("Live karaoke")
                onClicked: root.liveMode ? root.openCurrentSnapshot() : root.openLiveSong()
            }

            Controls.CheckBox {
                id: followPlayback
                text: i18n("Follow playback")
                visible: root.liveMode
                checked: true
                onToggled: root.followCurrentLine()
            }
        }

        ListView {
            id: liveLyrics
            objectName: "liveLyricsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.liveMode && count > 0
            clip: true
            model: root.visible && root.liveMode && root.plasmoidApplet ? root.plasmoidApplet.lyricLines : []
            onCountChanged: Qt.callLater(root.followCurrentLine)
            Controls.ScrollBar.vertical: Controls.ScrollBar {}

            delegate: Controls.ItemDelegate {
                id: lineDelegate
                required property int index
                required property string modelData
                width: liveLyrics.width
                highlighted: index === root.activeLyricIndex
                hoverEnabled: root.plasmoidApplet !== null && root.plasmoidApplet.canSeekLyrics
                Accessible.name: modelData
                onClicked: {
                    if (root.plasmoidApplet && root.plasmoidApplet.canSeekLyrics) {
                        root.plasmoidApplet.seekToLyricLine(index);
                    }
                }

                contentItem: Controls.Label {
                    text: lineDelegate.highlighted ? root.liveLineText(lineDelegate.modelData) : lineDelegate.modelData
                    textFormat: lineDelegate.highlighted ? Text.StyledText : Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.liveMode && liveLyrics.count === 0
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: i18n("No synchronized lyrics for the current track.")
            wrapMode: Text.Wrap
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.liveMode
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
            visible: creditLabel.attributionHtml.length > 0 || creditLabel.fallbackText.length > 0

            AttributionLabel {
                id: creditLabel
                Layout.fillWidth: true
                attributionHtml: root.liveMode && root.plasmoidApplet ? root.plasmoidApplet.attributionHtml : root.attributionHtml
                fallbackText: root.liveMode && root.plasmoidApplet ? root.plasmoidApplet.attribution : root.attribution
                fallbackUrl: root.liveMode && root.plasmoidApplet ? root.plasmoidApplet.attributionUrl : root.attributionUrl
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.liveMode && root.plasmoidApplet && root.plasmoidApplet.songKey.length > 0

            Controls.Label { text: i18n("Song offset (ms)") }
            Controls.SpinBox {
                id: songOffset
                from: -2000
                to: 2000
                stepSize: 50
                editable: true
                value: root.plasmoidApplet ? root.plasmoidApplet.songLyricOffset : 0
                Accessible.name: i18n("Song offset in milliseconds")
            }
            Controls.Button {
                text: i18n("Save for this song")
                icon.name: "document-save"
                onClicked: root.plasmoidApplet.rememberSongLyrics(root.plasmoidApplet.songKey, songOffset.value)
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Controls.Button {
                id: copySelectionButton
                objectName: "copySelectionButton"
                text: i18n("Copy selection")
                icon.name: "edit-copy"
                visible: !root.liveMode && !root.apiLyrics
                enabled: visible && lyricsText.selectedText.length > 0
                onClicked: lyricsText.copy()
            }
            Controls.Button {
                id: copyAllButton
                objectName: "copyAllButton"
                text: i18n("Copy all lyrics")
                visible: !root.liveMode && !root.apiLyrics
                enabled: visible && lyricsText.length > 0
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
