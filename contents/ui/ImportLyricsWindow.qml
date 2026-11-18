// SPDX-License-Identifier: MIT

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami

Window {
    id: root

    width: Kirigami.Units.gridUnit * 28
    height: Kirigami.Units.gridUnit * 23
    minimumWidth: Kirigami.Units.gridUnit * 23
    minimumHeight: Kirigami.Units.gridUnit * 16
    title: i18n("Import local lyrics — Plasma LRC")
    color: Kirigami.Theme.backgroundColor

    property var plasmoidApplet: null
    property string songKey: ""
    property string trackInfo: ""
    property var previewResult: ({})
    property string errorText: ""
    readonly property bool trackChanged: plasmoidApplet !== null && songKey !== plasmoidApplet.songKey

    function openForTrack() {
        root.plasmoidApplet.cancelLrcImport();
        root.songKey = root.plasmoidApplet.songKey;
        root.trackInfo = root.plasmoidApplet.trackInfo;
        root.previewResult = {};
        root.errorText = "";
        offset.value = root.plasmoidApplet.songLyricOffset;
        show();
        raise();
        requestActivate();
        fileDialog.open();
    }

    function previewText() {
        const lines = root.previewResult.lines || [];
        return lines.map(function(line) {
            const ms = Math.max(0, line.startMs + offset.value);
            const minutes = Math.floor(ms / 60000);
            const seconds = Math.floor(ms / 1000) % 60;
            const millis = ms % 1000;
            return minutes + ":" + String(seconds).padStart(2, "0") + "."
                + String(millis).padStart(3, "0") + "  " + line.text;
        }).join("\n");
    }

    onClosing: {
        fileDialog.close();
        if (root.plasmoidApplet) {
            root.plasmoidApplet.cancelLrcImport();
        }
        root.previewResult = {};
    }

    FileDialog {
        id: fileDialog
        title: i18n("Choose an LRC file")
        fileMode: FileDialog.OpenFile
        nameFilters: [i18n("LRC lyrics (*.lrc *.LRC)")]
        onAccepted: {
            root.previewResult = root.plasmoidApplet.previewLrcImport(selectedFile, root.songKey);
            root.errorText = root.previewResult.error || "";
        }
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
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            visible: root.trackChanged || root.errorText.length > 0
            text: root.trackChanged ? i18n("The track changed. Reopen Import lyrics for the new track.") : root.errorText
        }

        RowLayout {
            Layout.fillWidth: true

            Controls.Button {
                text: i18n("Choose file…")
                icon.name: "document-open"
                enabled: !root.trackChanged
                onClicked: fileDialog.open()
            }

            Controls.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: root.previewResult.lineCount
                    ? i18np("%1 synchronized line", "%1 synchronized lines", root.previewResult.lineCount)
                    : ""
            }
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Controls.TextArea {
                objectName: "importPreviewText"
                text: root.previewText()
                textFormat: TextEdit.PlainText
                readOnly: true
                wrapMode: TextEdit.Wrap
                placeholderText: i18n("Choose a file to preview its synchronized lines.")
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Controls.Label { text: i18n("Song offset (ms)") }
            Controls.SpinBox {
                id: offset
                from: -2000
                to: 2000
                stepSize: 50
                editable: true
                Accessible.name: i18n("Song offset in milliseconds")
            }
            Item { Layout.fillWidth: true }
            Controls.Button {
                text: i18n("Cancel")
                onClicked: root.close()
            }
            Controls.Button {
                text: i18n("Import")
                icon.name: "document-import"
                highlighted: true
                enabled: !root.trackChanged && !!root.previewResult.lineCount
                onClicked: {
                    root.errorText = root.plasmoidApplet.saveLrcImport(root.songKey, offset.value);
                    if (root.errorText.length === 0) {
                        root.close();
                    }
                }
            }
        }
    }
}
