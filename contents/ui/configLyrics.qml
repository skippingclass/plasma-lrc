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

    property alias cfg_player: playerField.text
    property alias cfg_preferredPlayer: preferredField.text
    property alias cfg_ignoredPlayers: ignoredField.text
    property alias cfg_lyricOffset: offsetField.value
    property int cfg_pollInterval: 2000
    property alias cfg_showTimestamp: showTimestampCheckBox.checked
    property alias cfg_pauseWhenIdle: pauseWhenIdleCheckBox.checked
    property alias cfg_pauseHideDelay: pauseHideDelayField.value
    property alias cfg_clickAction: clickActionBox.currentIndex
    property alias cfg_middleClickAction: middleClickActionBox.currentIndex

    Kirigami.FormLayout {

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Player")
        }

        QQC2.TextField {
            id: playerField

            Kirigami.FormData.label: i18n("Lock to player:")
            placeholderText: i18n("auto-detect (leave empty)")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: playerField.text.length > 0
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("With a name here the widget follows that player and nothing else. Leave empty to let the widget auto-detect active players.")
        }

        QQC2.TextField {
            id: preferredField

            Kirigami.FormData.label: i18n("Favourite player:")
            placeholderText: i18n("e.g. spotify")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: preferredField.text.length > 0
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Used while it is playing. Part of the name is enough (e.g. \"chrome\" covers every chromium instance).")
        }

        QQC2.TextField {
            id: ignoredField

            Kirigami.FormData.label: i18n("Never ask:")
            placeholderText: i18n("e.g. TelegramDesktop, chromium.instance1234")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            visible: ignoredField.text.length > 0
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Applications to skip, separated by commas (for media sessions that are not music tracks).")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Timing & Pauses")
        }

        QQC2.SpinBox {
            id: offsetField

            Kirigami.FormData.label: i18n("Lyric offset:")
            from: -2000
            to: 2000
            stepSize: 50
            textFromValue: function (value) {
                return value > 0 ? "+" + value + " ms" : value + " ms";
            }
            valueFromText: function (text) {
                const parsed = parseInt(text);
                return isNaN(parsed) ? 0 : parsed;
            }
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            opacity: 0.75
            visible: offsetField.value !== 0
            text: i18n("Positive delays the lyrics (shifts later if they are rushing ahead); negative shows them earlier.")
        }

        QQC2.SpinBox {
            id: pauseHideDelayField

            Kirigami.FormData.label: i18n("Hide on pause:")
            from: 0
            to: 60
            stepSize: 1
            textFromValue: function (value) {
                return value === 0 ? i18n("never hide") : value + " s";
            }
            valueFromText: function (text) {
                const parsed = parseInt(text);
                return isNaN(parsed) ? 0 : parsed;
            }
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

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Mouse Actions")
        }

        QQC2.ComboBox {
            id: clickActionBox

            Kirigami.FormData.label: i18n("Left click:")
            model: [i18n("Open lyrics popup"), i18n("Play / Pause")]
        }

        QQC2.ComboBox {
            id: middleClickActionBox

            Kirigami.FormData.label: i18n("Middle click:")
            model: [i18n("None"), i18n("Play / Pause"), i18n("Next track")]
        }
    }
}
