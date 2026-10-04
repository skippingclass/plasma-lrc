/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

// SimpleKCM is what a Plasma 6 config page is supposed to be rooted in: the
// dialog sets `title` on it, and it brings its own scrolling and padding.
KCM.SimpleKCM {
    id: root

    property alias cfg_binaryPath: binaryPathField.text
    property alias cfg_player: playerField.text
    property alias cfg_ignoredPlayers: ignoredField.text
    property alias cfg_pollInterval: pollIntervalField.value
    property alias cfg_showTimestamp: showTimestampCheckBox.checked
    property alias cfg_pauseWhenIdle: pauseWhenIdleCheckBox.checked

    Kirigami.FormLayout {

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

        QQC2.TextField {
            id: ignoredField

            Kirigami.FormData.label: i18n("Never ask:")
            placeholderText: i18n("TelegramDesktop, chromium.instance1234")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: ignoredField.text.length > 0
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Applications that implement MPRIS for things that are not tracks — a chat client reporting a voice message, for example — are recognised by their metadata and skipped on their own. This field is for the rest.")
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
