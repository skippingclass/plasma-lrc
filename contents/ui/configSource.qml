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
            text: i18n("It answers with the best sync it has, including community-made word timings. Tracks it does not know fall back to LRCLIB. Without a key the widget keeps using LRCLIB only.")
        }

        QQC2.TextField {
            id: keyField

            Kirigami.FormData.label: i18n("API key:")
            echoMode: TextInput.PasswordEchoOnEdit
            placeholderText: i18n("Client key: sl_pk_…")
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
            text: i18n("Get your client key through the approved Plasma LRC app template in the Spicy Lyrics catalog. The template link will be published after approval. Do not create a standalone developer application. SPICY_LYRICS_SECRET_KEY, if set, overrides this field.")
        }
    }
}
