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
    property alias cfg_customHighlightColor: customHighlightColorField.text

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

        RowLayout {
            Kirigami.FormData.label: i18n("Highlight color:")
            spacing: Kirigami.Units.smallSpacing

            Rectangle {
                id: colorPreview
                width: Kirigami.Units.gridUnit * 1.5
                height: Kirigami.Units.gridUnit * 1.5
                radius: Kirigami.Units.smallSpacing / 2
                border.width: 1
                border.color: Kirigami.Theme.separatorColor
                color: {
                    const c = customHighlightColorField.text.trim();
                    return c.length > 0 ? c : Kirigami.Theme.highlightColor;
                }
            }

            QQC2.TextField {
                id: customHighlightColorField
                placeholderText: i18n("System accent (empty) or #RRGGBB")
                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
            }

            QQC2.Button {
                text: i18n("Reset")
                icon.name: "edit-clear"
                enabled: customHighlightColorField.text.length > 0
                onClicked: {
                    customHighlightColorField.text = "";
                }
            }
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Color presets:")
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: [
                    { name: "Spotify", color: "#1ed760" },
                    { name: "Cyan", color: "#00d4ff" },
                    { name: "Purple", color: "#b342f5" },
                    { name: "Yellow", color: "#ffd600" },
                    { name: "Coral", color: "#ff4d4d" },
                    { name: "White", color: "#ffffff" }
                ]
                delegate: QQC2.Button {
                    text: modelData.name
                    onClicked: {
                        customHighlightColorField.text = modelData.color;
                    }
                }
            }
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
