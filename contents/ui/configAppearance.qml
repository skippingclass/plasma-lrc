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

    property alias cfg_showIcon: showIconCheckBox.checked
    property alias cfg_showTrackInfo: showTrackInfoCheckBox.checked
    property alias cfg_placeholderText: placeholderField.text
    property alias cfg_maxCharacters: maxCharactersField.value

    Kirigami.FormLayout {
        anchors.fill: parent

        QQC2.CheckBox {
            id: showIconCheckBox

            Kirigami.FormData.label: i18n("Icon:")
            text: i18n("Show a note next to the lyrics")
        }

        QQC2.CheckBox {
            id: showTrackInfoCheckBox

            Kirigami.FormData.label: i18n("Tooltip:")
            text: i18n("Show the current track in the tooltip")
        }

        QQC2.TextField {
            id: placeholderField

            Kirigami.FormData.label: i18n("No lyrics text:")
            Layout.fillWidth: true
        }

        QQC2.SpinBox {
            id: maxCharactersField

            Kirigami.FormData.label: i18n("Maximum length:")
            from: 0
            to: 500
            stepSize: 5
            textFromValue: function (value) {
                return value === 0 ? i18n("no limit") : value + " " + i18n("characters");
            }
            valueFromText: function (text) {
                const parsed = parseInt(text);
                return isNaN(parsed) ? 0 : parsed;
            }
        }
    }
}
