/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.plasma.plasmoid
import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

// SimpleKCM is what a Plasma 6 config page is supposed to be rooted in: the
// dialog sets `title` on it, and it brings its own scrolling and padding.
KCM.SimpleKCM {
    id: root

    property alias cfg_providerOrder: rawOrderField.text
    property alias cfg_disabledProviders: rawDisabledField.text
    property alias cfg_useSpicyLyrics: useSpicyHiddenBox.checked
    property alias cfg_spicyKey: keyField.text
    property alias cfg_localLyricsFolder: localFolderField.text
    property alias cfg_customLyricsUrl: customUrlField.text
    property alias cfg_customLyricsHeaders: customHeadersField.text

    readonly property var targetApplet: {
        if (typeof Plasmoid !== "undefined" && Plasmoid) {
            if (Plasmoid.applet) {
                return Plasmoid.applet;
            }
            return Plasmoid;
        }
        return null;
    }

    QQC2.TextField {
        id: rawOrderField
        visible: false
        onTextChanged: {
            if (!root.syncing) {
                root.syncFromConfig();
            }
        }
    }

    QQC2.TextField {
        id: rawDisabledField
        visible: false
        onTextChanged: {
            if (!root.syncing) {
                root.syncFromConfig();
            }
        }
    }

    QQC2.CheckBox {
        id: useSpicyHiddenBox
        visible: false
        onCheckedChanged: {
            if (!root.syncing) {
                root.syncFromConfig();
            }
        }
    }

    property bool syncing: false
    property bool initialized: false

    ListModel {
        id: providersModel
    }

    function getProviderDefaultName(id) {
        switch (id) {
        case "local": return i18n("Local Files");
        case "unison": return "Unison (boidu.dev)";
        case "lrcred": return "lrc.red";
        case "netease": return "NetEase (网易云音乐)";
        case "kugou": return "KuGou (酷狗音乐)";
        case "qqmusic": return "QQ Music (QQ音乐)";
        case "amll": return "AMLL TTML DB";
        case "spicy": return "Spicy Lyrics";
        case "lrclib": return "LRCLIB";
        case "custom": return i18n("Custom URL");
        default: return id;
        }
    }

    function getProviderDefaultWordSync(id) {
        return id === "local" || id === "unison" || id === "lrcred" || id === "netease" || id === "kugou" || id === "qqmusic" || id === "amll" || id === "spicy" || id === "custom";
    }

    function getProviderDefaultLineSync(id) {
        return true;
    }

    function getProviderDefaultRequiresKey(id) {
        return id === "spicy";
    }

    function getProviderDefaultRequiresSpotify(id) {
        return id === "amll" || id === "spicy";
    }

    function getProviderDesc(id) {
        switch (id) {
        case "local":
            return i18n("Loads .ttml or .lrc files next to the local audio track, or from ~/.lyrics.");
        case "unison":
            return i18n("Community synced lyrics database (boidu.dev) with word-level TTML timing. Free.");
        case "lrcred":
            return i18n("Free Apple Music TTML database with syllable sync. No token required.");
        case "netease":
            return i18n("Massive Asian & international music catalog (网易云音乐) with YRC syllable sync.");
        case "kugou":
            return i18n("KuGou (酷狗音乐) database with KRC word-level timing.");
        case "qqmusic":
            return i18n("Tencent QQ Music (QQ音乐) with QRC syllable sync.");
        case "amll":
            return i18n("Community Apple Music TTML repository for Spotify tracks via CDN. Free.");
        case "spicy":
            return i18n("Community lyrics database with word sync for Spotify tracks. Requires API key.");
        case "lrclib":
            return i18n("Free open-source community lyrics database (line-by-line sync). Reliable fallback.");
        case "custom":
            return i18n("Self-hosted or custom lyrics HTTP endpoint using {title}, {artist}, {duration} template.");
        default:
            return "";
        }
    }

    function syncFromConfig() {
        if (root.syncing) {
            return;
        }
        root.syncing = true;

        let orderStr = rawOrderField.text.trim();
        let disabledStr = rawDisabledField.text.trim();

        if (!root.initialized && orderStr.length === 0 && root.targetApplet && root.targetApplet.providerOrder) {
            const appletOrder = root.targetApplet.providerOrder;
            if (Array.isArray(appletOrder) && appletOrder.length > 0) {
                orderStr = appletOrder.join(",");
                rawOrderField.text = orderStr;
            }
        }

        if (orderStr.length === 0) {
            orderStr = "local,unison,lrcred,netease,kugou,qqmusic,amll,spicy,lrclib,custom";
            rawOrderField.text = orderStr;
        }

        root.initialized = true;

        let disabledList = disabledStr.length > 0
            ? disabledStr.split(",").map(function(s) { return s.trim(); })
            : [];

        if (!useSpicyHiddenBox.checked && disabledList.indexOf("spicy") === -1) {
            disabledList.push("spicy");
        }

        let orderList = orderStr.split(",").map(function(s) { return s.trim(); }).filter(function(s) { return s.length > 0; });
        const allKnown = ["local", "unison", "lrcred", "netease", "kugou", "qqmusic", "amll", "spicy", "lrclib", "custom"];
        for (let i = 0; i < allKnown.length; ++i) {
            if (orderList.indexOf(allKnown[i]) === -1) {
                orderList.push(allKnown[i]);
            }
        }

        let appletProvidersMap = {};
        if (root.targetApplet && typeof root.targetApplet.providers === "function") {
            const list = root.targetApplet.providers();
            for (let i = 0; i < list.length; ++i) {
                appletProvidersMap[list[i].id] = list[i];
            }
        }

        providersModel.clear();
        for (let i = 0; i < orderList.length; ++i) {
            const id = orderList[i];
            const appletInfo = appletProvidersMap[id];

            const name = appletInfo ? appletInfo.name : root.getProviderDefaultName(id);
            let isEnabled = disabledList.indexOf(id) === -1;
            if (id === "spicy" && !useSpicyHiddenBox.checked) {
                isEnabled = false;
            }
            const wordSync = appletInfo ? appletInfo.wordSync : root.getProviderDefaultWordSync(id);
            const lineSync = appletInfo ? appletInfo.lineSync : root.getProviderDefaultLineSync(id);
            const reqKey = appletInfo ? appletInfo.requiresKey : root.getProviderDefaultRequiresKey(id);
            const reqSpotify = appletInfo ? appletInfo.requiresSpotifyId : root.getProviderDefaultRequiresSpotify(id);
            const desc = root.getProviderDesc(id);

            providersModel.append({
                "providerId": id,
                "providerName": name,
                "providerEnabled": isEnabled,
                "hasWordSync": wordSync,
                "hasLineSync": lineSync,
                "needsKey": reqKey,
                "needsSpotifyId": reqSpotify,
                "providerDesc": desc
            });
        }

        root.syncing = false;
    }

    function syncToConfig() {
        if (root.syncing) {
            return;
        }
        root.syncing = true;

        let order = [];
        let disabled = [];
        let spicyEnabled = true;

        for (let i = 0; i < providersModel.count; ++i) {
            const item = providersModel.get(i);
            order.push(item.providerId);
            if (!item.providerEnabled) {
                disabled.push(item.providerId);
                if (item.providerId === "spicy") {
                    spicyEnabled = false;
                }
            } else if (item.providerId === "spicy") {
                spicyEnabled = true;
            }
        }

        rawOrderField.text = order.join(",");
        rawDisabledField.text = disabled.join(",");
        useSpicyHiddenBox.checked = spicyEnabled;

        root.syncing = false;
    }

    function moveUp(idx) {
        if (idx <= 0 || idx >= providersModel.count) {
            return;
        }
        providersModel.move(idx, idx - 1, 1);
        root.syncToConfig();
        if (root.targetApplet && typeof root.targetApplet.moveProviderUp === "function") {
            root.targetApplet.moveProviderUp(idx);
        }
    }

    function moveDown(idx) {
        if (idx < 0 || idx >= providersModel.count - 1) {
            return;
        }
        providersModel.move(idx, idx + 1, 1);
        root.syncToConfig();
        if (root.targetApplet && typeof root.targetApplet.moveProviderDown === "function") {
            root.targetApplet.moveProviderDown(idx);
        }
    }

    function toggleProvider(idx, isChecked) {
        if (idx < 0 || idx >= providersModel.count) {
            return;
        }
        providersModel.setProperty(idx, "providerEnabled", isChecked);
        const id = providersModel.get(idx).providerId;
        if (id === "spicy") {
            useSpicyHiddenBox.checked = isChecked;
        }
        root.syncToConfig();
        if (root.targetApplet && typeof root.targetApplet.setProviderEnabled === "function") {
            root.targetApplet.setProviderEnabled(id, isChecked);
        }
    }

    function isSpicyEnabled() {
        for (let i = 0; i < providersModel.count; ++i) {
            if (providersModel.get(i).providerId === "spicy") {
                return providersModel.get(i).providerEnabled;
            }
        }
        return useSpicyHiddenBox.checked;
    }

    Component.onCompleted: {
        root.syncFromConfig();
    }

    Connections {
        target: root.targetApplet
        ignoreUnknownSignals: true
        function onProviderOrderChanged() {
            root.syncFromConfig();
        }
    }

    Kirigami.FormLayout {
        id: formLayout

        Kirigami.Heading {
            level: 3
            text: i18n("Lyrics Providers Priority")
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Providers are queried in order from top to bottom. If a provider returns no lyrics or fails, the widget automatically falls back to the next active provider.")
        }

        Repeater {
            model: providersModel

            delegate: Rectangle {
                id: itemDelegate
                required property string providerId
                required property string providerName
                required property bool providerEnabled
                required property bool hasWordSync
                required property bool hasLineSync
                required property bool needsKey
                required property bool needsSpotifyId
                required property string providerDesc
                required property int index

                Layout.columnSpan: 2
                Layout.fillWidth: true
                implicitHeight: contentRow.implicitHeight + Kirigami.Units.smallSpacing * 2
                radius: Kirigami.Units.smallSpacing
                color: Kirigami.Theme.alternateBackgroundColor
                border.width: 1
                border.color: itemDelegate.providerEnabled ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                opacity: itemDelegate.providerEnabled ? 1.0 : 0.65

                RowLayout {
                    id: contentRow
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    // Priority reordering buttons
                    ColumnLayout {
                        spacing: 0

                        QQC2.ToolButton {
                            icon.name: "arrow-up"
                            enabled: itemDelegate.index > 0
                            implicitWidth: Kirigami.Units.iconSizes.small * 1.5
                            implicitHeight: Kirigami.Units.iconSizes.small * 1.5
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: i18n("Move up in priority")
                            onClicked: root.moveUp(itemDelegate.index)
                        }

                        QQC2.ToolButton {
                            icon.name: "arrow-down"
                            enabled: itemDelegate.index < providersModel.count - 1
                            implicitWidth: Kirigami.Units.iconSizes.small * 1.5
                            implicitHeight: Kirigami.Units.iconSizes.small * 1.5
                            QQC2.ToolTip.visible: hovered
                            QQC2.ToolTip.text: i18n("Move down in priority")
                            onClicked: root.moveDown(itemDelegate.index)
                        }
                    }

                    // Enable/disable checkbox
                    QQC2.CheckBox {
                        id: enableCheckBox
                        checked: itemDelegate.providerEnabled
                        onToggled: root.toggleProvider(itemDelegate.index, checked)
                    }

                    // Provider info and badges
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.Label {
                                text: itemDelegate.providerName
                                font.bold: true
                            }

                            QQC2.Label {
                                text: "#" + (itemDelegate.index + 1)
                                opacity: 0.5
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                            }

                            Item { Layout.fillWidth: true }

                            // Badges
                            Rectangle {
                                visible: itemDelegate.hasWordSync
                                radius: Kirigami.Units.smallSpacing / 2
                                implicitWidth: wordSyncText.implicitWidth + Kirigami.Units.smallSpacing * 1.5
                                implicitHeight: wordSyncText.implicitHeight + 4
                                color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.2)
                                border.width: 1
                                border.color: Kirigami.Theme.highlightColor

                                QQC2.Label {
                                    id: wordSyncText
                                    anchors.centerIn: parent
                                    text: i18n("Word sync")
                                    font.bold: true
                                    font.pointSize: Math.max(7, Kirigami.Theme.smallFont.pointSize - 1)
                                    color: Kirigami.Theme.highlightColor
                                }
                            }

                            Rectangle {
                                visible: itemDelegate.needsKey
                                radius: Kirigami.Units.smallSpacing / 2
                                implicitWidth: keyText.implicitWidth + Kirigami.Units.smallSpacing * 1.5
                                implicitHeight: keyText.implicitHeight + 4
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.1)
                                border.width: 1
                                border.color: Kirigami.Theme.disabledTextColor

                                QQC2.Label {
                                    id: keyText
                                    anchors.centerIn: parent
                                    text: i18n("API key")
                                    font.bold: true
                                    font.pointSize: Math.max(7, Kirigami.Theme.smallFont.pointSize - 1)
                                }
                            }
                        }

                        QQC2.Label {
                            text: itemDelegate.providerDesc
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            opacity: 0.75
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        Kirigami.Heading {
            level: 3
            text: i18n("Spicy Lyrics Settings")
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        QQC2.TextField {
            id: keyField
            Kirigami.FormData.label: i18n("API key:")
            echoMode: TextInput.PasswordEchoOnEdit
            placeholderText: i18n("Client key: sl_pk_…")
            enabled: root.isSpicyEnabled()
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Get your client key through the approved Plasma LRC app template in the Spicy Lyrics catalog. The template link will be published after approval. Do not create a standalone developer application. SPICY_LYRICS_SECRET_KEY, if set, overrides this field.")
        }

        Kirigami.Separator {
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        Kirigami.Heading {
            level: 3
            text: i18n("Local Files Settings")
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        QQC2.TextField {
            id: localFolderField
            Kirigami.FormData.label: i18n("Lyrics folder:")
            placeholderText: i18n("~/.lyrics or ~/Music/lyrics")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Optional extra folder to look for {Artist} - {Title}.lrc or .ttml files. Files located next to the playing audio file are always searched first.")
        }

        Kirigami.Separator {
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        Kirigami.Heading {
            level: 3
            text: i18n("Custom URL Provider Settings")
            Layout.columnSpan: 2
            Layout.fillWidth: true
        }

        QQC2.TextField {
            id: customUrlField
            Kirigami.FormData.label: i18n("URL template:")
            placeholderText: i18n("https://example.com/lyrics?artist={artist}&title={title}")
            Layout.fillWidth: true
        }

        QQC2.TextField {
            id: customHeadersField
            Kirigami.FormData.label: i18n("Headers:")
            placeholderText: i18n("Authorization: Bearer my-token")
            Layout.fillWidth: true
        }

        QQC2.Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.Wrap
            opacity: 0.75
            text: i18n("Supported placeholders: {title}, {artist}, {album}, {duration} (seconds), {durationMs}, {spotifyId}.")
        }
    }
}
