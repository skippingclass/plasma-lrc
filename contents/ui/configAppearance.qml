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

    property alias cfg_showIcon: showIconCheckBox.checked
    property alias cfg_showTrackInfo: showTrackInfoCheckBox.checked
    property alias cfg_placeholderText: placeholderField.text
    property alias cfg_maxCharacters: maxCharactersField.value
    property alias cfg_compactPanel: compactModeCheckBox.checked
    property alias cfg_wordStyle: wordStyleBox.currentIndex
    property alias cfg_fadeTransition: fadeTransitionCheckBox.checked
    property alias cfg_textAlignment: textAlignmentBox.currentIndex

    Kirigami.FormLayout {

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

        QQC2.CheckBox {
            id: compactModeCheckBox

            Kirigami.FormData.label: i18n("Panel:")
            text: i18n("Compact mode")
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
            wrapMode: Text.Wrap
            opacity: 0.75
            visible: compactModeCheckBox.checked
            text: i18n("Leave the panel to the lyrics alone. The credit stays in the popup, where it is spelled out with a link to the contributor.")
        }

        QQC2.ComboBox {
            id: wordStyleBox

            Kirigami.FormData.label: i18n("Sung word:")
            model: [i18n("Underlined"), i18n("Bold"), i18n("Highlight color")]
        }

        QQC2.ComboBox {
            id: textAlignmentBox

            Kirigami.FormData.label: i18n("Alignment:")
            model: [i18n("Center"), i18n("Left")]
        }

        QQC2.CheckBox {
            id: fadeTransitionCheckBox

            Kirigami.FormData.label: i18n("Transitions:")
            text: i18n("Smooth fade animation between lines")
        }
    }
}
