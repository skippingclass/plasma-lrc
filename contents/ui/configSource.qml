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

    property alias cfg_useSpicyLyrics: useSpicyCheckBox.checked
    property alias cfg_spicyKey: keyField.text

    Kirigami.FormLayout {

        QQC2.CheckBox {
            id: useSpicyCheckBox

            Kirigami.FormData.label: i18n("Spicy Lyrics:")
            text: i18n("Ask the Spicy Lyrics API for Spotify tracks")
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            opacity: 0.75
            visible: useSpicyCheckBox.checked
            text: i18n("It answers with the best sync it has, including community-made word timings. Tracks it does not know fall back to lrc_tty. Without a key the widget keeps using lrc_tty only.")
        }

        QQC2.TextField {
            id: keyField

            Kirigami.FormData.label: i18n("API key:")
            echoMode: TextInput.PasswordEchoOnEdit
            placeholderText: i18n("sl_sk_… or SPICY_LYRICS_SECRET_KEY")
            enabled: useSpicyCheckBox.checked
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            opacity: 0.75
            visible: useSpicyCheckBox.checked
            text: i18n("Get an application and a key at developers.spicylyrics.org. Each user needs their own key; the environment variable is used when the field is left empty.")
        }
    }
}