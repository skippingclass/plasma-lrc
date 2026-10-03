/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kirigami as Kirigami

QQC2.Pane {
    id: root

    property alias cfg_binaryPath: binaryPathField.text
    property alias cfg_player: playerField.text
    property alias cfg_pollInterval: pollIntervalField.value
    property alias cfg_showTimestamp: showTimestampCheckBox.checked
    property alias cfg_pauseWhenIdle: pauseWhenIdleCheckBox.checked

    Kirigami.FormLayout {
        anchors.fill: parent

        QQC2.TextField {
            id: binaryPathField

            Kirigami.FormData.label: i18n("lrc_tty executable:")
            placeholderText: i18n("lrc_tty")
            Layout.fillWidth: true
        }

        QQC2.SpinBox {
            id: pollIntervalField

            Kirigami.FormData.label: i18n("Update every:")
            from: 200
            to: 10000
            stepSize: 100
            textFromValue: function (value) {
                return value + " ms";
            }
            valueFromText: function (text) {
                return parseInt(text);
            }
        }

        QQC2.TextField {
            id: playerField

            Kirigami.FormData.label: i18n("MPRIS player:")
            placeholderText: i18n("auto-detect")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: playerField.text.length > 0
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Run 'lrc_tty --list-players' to see which players are available. Leave this empty to try all of them.")
        }

        QQC2.CheckBox {
            id: showTimestampCheckBox

            Kirigami.FormData.label: i18n("Timestamps:")
            text: i18n("Prefix the lyrics with a [mm:ss] timestamp")
        }

        QQC2.CheckBox {
            id: pauseWhenIdleCheckBox

            Kirigami.FormData.label: i18n("Idle:")
            text: i18n("Stop querying while nothing is playing")
        }
    }
}
