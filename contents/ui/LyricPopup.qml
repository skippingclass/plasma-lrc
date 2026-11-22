/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

/**
 * Panel popup showing media player controls (album art, track metadata,
 * seek bar, transport controls), the active lyric line with word-level
 * highlighting, playback/lyric status, attribution, and quick actions.
 *
 * The lyric line is passed in already prepared for rich text, because it comes
 * from a third party and has to be escaped somewhere that can see all of it.
 */
Item {
    id: root

    /// Track as MPRIS reports it.
    property string trackInfo: ""
    property string trackTitle: ""
    property string trackArtist: ""
    property string trackAlbum: ""
    property string artUrl: ""
    property string playerName: ""
    property bool hasPlayer: false
    property bool showTrackInfo: true
    property bool blurBackground: true
    property bool roundedCorners: true

    /// Playback state and MPRIS capabilities.
    property bool playing: false
    property double trackDurationMs: 0
    property double playbackPositionMs: 0
    property var positionProvider: null
    property bool canGoNext: false
    property bool canGoPrevious: false
    property bool canPlayPause: false
    property bool canSeek: false
    property bool hasShuffle: false
    property bool shuffle: false
    property bool hasLoopStatus: false
    property string loopStatus: ""

    /// The line, with the current word already marked up.
    property string line: ""
    /// Shown instead of the line when there is nothing to sing along with.
    property string placeholder: ""
    /// Short status summary for the footer when attribution is hidden.
    property string statusText: ""
    property bool hasLyrics: true

    /// Who the lyrics came from, when it is worth naming.
    property string attribution: ""
    property string attributionUrl: ""
    property string attributionHtml: ""
    property bool showAttribution: false

    property bool isBlacklisted: false
    property bool isLyricsDiscarded: false

    signal togglePlayPause()
    signal nextTrack()
    signal previousTrack()
    signal seekToPosition(double positionMs)
    signal syncPosition()
    signal toggleShuffle()
    signal cycleLoopStatus()
    signal raisePlayer()

    signal toggleBlacklist()
    signal toggleDiscardLyrics()
    signal retryLyrics()
    signal openSongLyrics()

    readonly property bool hasTrack: root.trackTitle.length > 0 || root.trackInfo.length > 0
    readonly property string displayTitle: root.trackTitle.length > 0 ? root.trackTitle : root.trackInfo
    readonly property int contentWidth: root.hasTrack
        ? Kirigami.Units.gridUnit * 28
        : Kirigami.Units.gridUnit * 16
    readonly property int cardRadius: Kirigami.Units.smallSpacing * 2
    readonly property int coverRadius: root.roundedCorners ? Kirigami.Units.smallSpacing * 2 : 0

    readonly property color surfaceColor: Qt.rgba(
        Kirigami.Theme.textColor.r,
        Kirigami.Theme.textColor.g,
        Kirigami.Theme.textColor.b,
        0.045
    )
    readonly property color borderColor: Qt.rgba(
        Kirigami.Theme.textColor.r,
        Kirigami.Theme.textColor.g,
        Kirigami.Theme.textColor.b,
        0.10
    )
    readonly property color pillColor: Qt.rgba(
        Kirigami.Theme.textColor.r,
        Kirigami.Theme.textColor.g,
        Kirigami.Theme.textColor.b,
        0.075
    )

    property double displayedPositionMs: 0
    property double pendingSeekMs: 0

    // Enforce min/preferred/max to match content so Plasma's AppletPopup never
    // clips the media controls to a stale popupHeight/popupWidth saved from an
    // older version, while still hugging dynamic lyric height changes.
    implicitWidth: contentWidth
    implicitHeight: mainColumn.implicitHeight
    Layout.minimumWidth: contentWidth
    Layout.preferredWidth: contentWidth
    Layout.maximumWidth: contentWidth
    Layout.minimumHeight: mainColumn.implicitHeight
    Layout.preferredHeight: mainColumn.implicitHeight
    Layout.maximumHeight: mainColumn.implicitHeight

    function escapeHtml(text) {
        if (!text) return "";
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#39;");
    }

    function formatDuration(ms) {
        const totalSeconds = Math.max(0, Math.floor((ms || 0) / 1000));
        const hours = Math.floor(totalSeconds / 3600);
        const minutes = Math.floor((totalSeconds % 3600) / 60);
        const seconds = totalSeconds % 60;
        const secStr = seconds < 10 ? ("0" + seconds) : String(seconds);
        if (hours > 0) {
            const minStr = minutes < 10 ? ("0" + minutes) : String(minutes);
            return hours + ":" + minStr + ":" + secStr;
        }
        return minutes + ":" + secStr;
    }

    function refreshDisplayedPosition() {
        const pos = root.positionProvider ? root.positionProvider() : root.playbackPositionMs;
        const clamped = root.trackDurationMs > 0
            ? Math.max(0, Math.min(pos, root.trackDurationMs))
            : Math.max(0, pos);
        root.displayedPositionMs = clamped;
        if (!seekSlider.pressed) {
            seekSlider.value = clamped;
        }
    }

    onPlaybackPositionMsChanged: refreshDisplayedPosition()
    onTrackDurationMsChanged: refreshDisplayedPosition()
    onVisibleChanged: {
        if (visible) {
            root.syncPosition();
            refreshDisplayedPosition();
        }
    }

    Component.onCompleted: refreshDisplayedPosition()

    Timer {
        id: positionTimer
        interval: 250
        repeat: true
        running: root.visible && root.hasTrack && root.playing && root.trackDurationMs > 0 && !seekSlider.pressed
        onTriggered: root.refreshDisplayedPosition()
    }

    ColumnLayout {
        id: mainColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Kirigami.Units.smallSpacing * 2

        // Hero media player card
        Rectangle {
            id: playerCard
            Layout.fillWidth: true
            implicitHeight: heroContent.implicitHeight + Kirigami.Units.smallSpacing * 5
            Layout.minimumHeight: implicitHeight
            Layout.preferredHeight: implicitHeight
            radius: root.cardRadius
            color: root.surfaceColor
            clip: true
            visible: root.hasTrack

            // Ultra-soft color wash extracted from the album art (downsampled to a
            // coarse color grid and heavily blurred so only smooth ambient colors
            // bleed across the card, never a recognizable stretched cover image)
            Image {
                id: backdropArt
                anchors.fill: parent
                source: root.blurBackground ? root.artUrl : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                smooth: true
                sourceSize: Qt.size(12, 12)
                visible: false
            }

            MultiEffect {
                anchors.fill: parent
                anchors.margins: -40
                scale: 1.35
                rotation: -14
                source: backdropArt
                blurEnabled: true
                blur: 1.0
                blurMax: 64
                blurMultiplier: 2.0
                saturation: 0.75
                brightness: 0.04
                opacity: (root.blurBackground && backdropArt.status === Image.Ready) ? 0.34 : 0.0
                visible: root.blurBackground && opacity > 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: 280
                        easing.type: Easing.OutCubic
                    }
                }
            }

            // Gentle scrim so typography and controls remain crisp on vivid or bright covers
            Rectangle {
                anchors.fill: parent
                visible: root.blurBackground && backdropArt.status === Image.Ready
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0.0
                        color: Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.24)
                    }
                    GradientStop {
                        position: 1.0
                        color: Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.52)
                    }
                }
            }

            // Crisp hairline frame on top of the blurred backdrop
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: "transparent"
                border.width: 1
                border.color: root.borderColor
                z: 10
            }

            RowLayout {
                id: heroContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Kirigami.Units.smallSpacing * 2.5
                spacing: Kirigami.Units.smallSpacing * 3

                // Large square album cover on the left (click to raise player)
                Item {
                    id: coverContainer
                    readonly property int artSize: Math.round(Kirigami.Units.gridUnit * 8.2)
                    Layout.preferredWidth: artSize
                    Layout.preferredHeight: artSize
                    Layout.minimumWidth: artSize
                    Layout.minimumHeight: artSize
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: coverFallback
                        anchors.fill: parent
                        radius: root.coverRadius
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.08)

                        Kirigami.Icon {
                            anchors.centerIn: parent
                            width: Kirigami.Units.iconSizes.large
                            height: Kirigami.Units.iconSizes.large
                            source: "media-optical-audio"
                            opacity: 0.55
                            visible: coverImage.status !== Image.Ready
                        }
                    }

                    Image {
                        id: coverImage
                        anchors.fill: parent
                        source: root.artUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        mipmap: true
                        smooth: true
                        sourceSize: Qt.size(300, 300)
                        visible: !root.roundedCorners && status === Image.Ready
                    }

                    Rectangle {
                        id: coverMask
                        anchors.fill: parent
                        radius: root.coverRadius
                        visible: false
                        layer.enabled: true
                    }

                    MultiEffect {
                        anchors.fill: parent
                        source: coverImage
                        maskEnabled: true
                        maskSource: coverMask
                        maskThresholdMin: 0.5
                        maskSpreadAtMin: 1.0
                        opacity: (root.roundedCorners && coverImage.status === Image.Ready) ? 1.0 : 0.0
                        visible: root.roundedCorners && opacity > 0

                        Behavior on opacity {
                            NumberAnimation {
                                duration: 180
                                easing.type: Easing.OutCubic
                            }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: root.coverRadius
                        color: coverMouse.containsMouse && root.hasPlayer
                            ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.08)
                            : "transparent"
                        border.width: 1
                        border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.14)
                    }

                    MouseArea {
                        id: coverMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: root.hasPlayer
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.raisePlayer()

                        PlasmaComponents3.ToolTip.text: root.playerName.length > 0
                            ? i18n("Open %1", root.playerName)
                            : i18n("Open player")
                        PlasmaComponents3.ToolTip.visible: containsMouse && root.hasPlayer
                    }
                }

                // Right side: metadata at top, seek bar + transport controls at bottom
                ColumnLayout {
                    id: rightControlsColumn
                    Layout.fillWidth: true
                    Layout.minimumHeight: coverContainer.artSize
                    Layout.alignment: Qt.AlignVCenter
                    spacing: Kirigami.Units.smallSpacing

                    // Track title, artist, album, and top-right status/player pill
                    ColumnLayout {
                        id: trackMetaColumn
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing * 1.5

                            PlasmaComponents3.Label {
                                id: titleLabel
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                text: root.displayTitle
                                textFormat: Text.PlainText
                                font.bold: true
                                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.18
                                color: Kirigami.Theme.textColor
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                visible: root.showTrackInfo && text.length > 0
                            }

                            Item {
                                Layout.fillWidth: true
                                visible: !titleLabel.visible
                            }

                            // Playback status & player name pill, aligned cleanly in the top-right
                            Rectangle {
                                id: playerPill
                                visible: root.hasPlayer || root.playerName.length > 0
                                readonly property int maxPillWidth: Kirigami.Units.gridUnit * 8
                                implicitWidth: Math.min(playerPillRow.implicitWidth + Kirigami.Units.smallSpacing * 3, maxPillWidth)
                                implicitHeight: Math.max(20, playerPillLabel.implicitHeight + 6)
                                Layout.preferredWidth: implicitWidth
                                Layout.preferredHeight: implicitHeight
                                Layout.maximumWidth: maxPillWidth
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                radius: height / 2
                                color: pillMouse.containsMouse && root.hasPlayer
                                    ? Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.14)
                                    : root.pillColor

                                RowLayout {
                                    id: playerPillRow
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: Kirigami.Units.smallSpacing * 1.5
                                    anchors.rightMargin: Kirigami.Units.smallSpacing * 1.5
                                    spacing: Kirigami.Units.smallSpacing

                                    Rectangle {
                                        Layout.preferredWidth: 6
                                        Layout.preferredHeight: 6
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 3
                                        color: root.playing ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                                    }

                                    PlasmaComponents3.Label {
                                        id: playerPillLabel
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: root.playerName.length > 0
                                            ? i18n("%1 · %2", root.playing ? i18n("Playing") : i18n("Paused"), root.playerName)
                                            : (root.playing ? i18n("Playing") : i18n("Paused"))
                                        textFormat: Text.PlainText
                                        font: Kirigami.Theme.smallFont
                                        color: Kirigami.Theme.textColor
                                        opacity: 0.82
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                        verticalAlignment: Text.AlignVCenter
                                        topPadding: 0
                                        bottomPadding: 0
                                    }
                                }

                                MouseArea {
                                    id: pillMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: root.hasPlayer
                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: root.raisePlayer()
                                }
                            }
                        }

                        PlasmaComponents3.Label {
                            id: artistLabel
                            Layout.fillWidth: true
                            text: root.trackArtist
                            textFormat: Text.PlainText
                            color: Kirigami.Theme.textColor
                            opacity: 0.82
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            visible: root.showTrackInfo && root.trackArtist.length > 0
                        }

                        PlasmaComponents3.Label {
                            id: albumLabel
                            Layout.fillWidth: true
                            text: root.trackAlbum
                            textFormat: Text.PlainText
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.textColor
                            opacity: 0.55
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            visible: root.showTrackInfo && root.trackAlbum.length > 0 && root.trackAlbum !== root.trackTitle
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    // Seek bar + elapsed / total timestamps
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        visible: root.trackDurationMs > 0

                        PlasmaComponents3.Slider {
                            id: seekSlider
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, root.trackDurationMs)
                            live: true
                            enabled: root.canSeek

                            onMoved: {
                                if (pressed) {
                                    root.pendingSeekMs = value;
                                } else {
                                    root.displayedPositionMs = value;
                                    root.seekToPosition(Math.round(value));
                                }
                            }

                            onPressedChanged: {
                                if (pressed) {
                                    root.pendingSeekMs = value;
                                } else {
                                    root.displayedPositionMs = value;
                                    root.seekToPosition(Math.round(value));
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 2
                            Layout.rightMargin: 2
                            Layout.topMargin: -Math.round(Kirigami.Units.smallSpacing * 0.5)

                            PlasmaComponents3.Label {
                                text: root.formatDuration(seekSlider.pressed ? root.pendingSeekMs : root.displayedPositionMs)
                                textFormat: Text.PlainText
                                font.family: Kirigami.Theme.smallFont.family
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                font.features: ({ "tnum": 1 })
                                color: Kirigami.Theme.textColor
                                opacity: 0.65
                            }

                            Item {
                                Layout.fillWidth: true
                            }

                            PlasmaComponents3.Label {
                                text: root.formatDuration(root.trackDurationMs)
                                textFormat: Text.PlainText
                                font.family: Kirigami.Theme.smallFont.family
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                font.features: ({ "tnum": 1 })
                                color: Kirigami.Theme.textColor
                                opacity: 0.65
                            }
                        }
                    }

                    // Media transport controls
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Item {
                            Layout.fillWidth: true
                        }

                        PlasmaComponents3.ToolButton {
                            id: shuffleBtn
                            visible: root.hasShuffle
                            icon.name: "media-playlist-shuffle"
                            text: i18n("Shuffle")
                            display: PlasmaComponents3.AbstractButton.IconOnly
                            checkable: true
                            checked: root.shuffle
                            opacity: root.shuffle ? 1.0 : 0.65
                            PlasmaComponents3.ToolTip.text: root.shuffle ? i18n("Shuffle: On") : i18n("Shuffle: Off")
                            PlasmaComponents3.ToolTip.visible: hovered
                            onClicked: root.toggleShuffle()
                        }

                        PlasmaComponents3.ToolButton {
                            id: prevBtn
                            icon.name: "media-skip-backward"
                            icon.width: Kirigami.Units.iconSizes.smallMedium
                            icon.height: Kirigami.Units.iconSizes.smallMedium
                            text: i18n("Previous")
                            display: PlasmaComponents3.AbstractButton.IconOnly
                            enabled: root.canGoPrevious
                            PlasmaComponents3.ToolTip.text: i18n("Previous track")
                            PlasmaComponents3.ToolTip.visible: hovered
                            onClicked: root.previousTrack()
                        }

                        PlasmaComponents3.AbstractButton {
                            id: playPauseBtn
                            readonly property int btnSize: Math.round(Kirigami.Units.gridUnit * 2.15)
                            implicitWidth: btnSize
                            implicitHeight: btnSize
                            enabled: root.canPlayPause
                            text: root.playing ? i18n("Pause") : i18n("Play")
                            scale: pressed ? 0.92 : 1.0

                            Behavior on scale {
                                NumberAnimation {
                                    duration: 90
                                    easing.type: Easing.OutQuad
                                }
                            }

                            background: Rectangle {
                                radius: width / 2
                                color: {
                                    if (!playPauseBtn.enabled) {
                                        return Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12);
                                    }
                                    if (playPauseBtn.down) {
                                        return Qt.darker(Kirigami.Theme.highlightColor, 1.12);
                                    }
                                    if (playPauseBtn.hovered) {
                                        return Qt.lighter(Kirigami.Theme.highlightColor, 1.08);
                                    }
                                    return Kirigami.Theme.highlightColor;
                                }

                                Behavior on color {
                                    ColorAnimation {
                                        duration: 120
                                    }
                                }
                            }

                            contentItem: Item {
                                Kirigami.Icon {
                                    anchors.centerIn: parent
                                    width: Kirigami.Units.iconSizes.smallMedium
                                    height: Kirigami.Units.iconSizes.smallMedium
                                    source: root.playing ? "media-playback-pause" : "media-playback-start"
                                    color: playPauseBtn.enabled ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                                    isMask: true
                                }
                            }

                            PlasmaComponents3.ToolTip.text: root.playing ? i18n("Pause") : i18n("Play")
                            PlasmaComponents3.ToolTip.visible: hovered
                            onClicked: root.togglePlayPause()
                        }

                        PlasmaComponents3.ToolButton {
                            id: nextBtn
                            icon.name: "media-skip-forward"
                            icon.width: Kirigami.Units.iconSizes.smallMedium
                            icon.height: Kirigami.Units.iconSizes.smallMedium
                            text: i18n("Next")
                            display: PlasmaComponents3.AbstractButton.IconOnly
                            enabled: root.canGoNext
                            PlasmaComponents3.ToolTip.text: i18n("Next track")
                            PlasmaComponents3.ToolTip.visible: hovered
                            onClicked: root.nextTrack()
                        }

                        PlasmaComponents3.ToolButton {
                            id: loopBtn
                            visible: root.hasLoopStatus
                            icon.name: root.loopStatus === "Track" ? "media-playlist-repeat-song" : "media-playlist-repeat"
                            text: i18n("Repeat")
                            display: PlasmaComponents3.AbstractButton.IconOnly
                            checkable: true
                            checked: root.loopStatus === "Track" || root.loopStatus === "Playlist"
                            opacity: checked ? 1.0 : 0.65
                            PlasmaComponents3.ToolTip.text: {
                                if (root.loopStatus === "Track") {
                                    return i18n("Repeat: Track");
                                }
                                if (root.loopStatus === "Playlist") {
                                    return i18n("Repeat: Playlist");
                                }
                                return i18n("Repeat: Off");
                            }
                            PlasmaComponents3.ToolTip.visible: hovered
                            onClicked: root.cycleLoopStatus()
                        }

                        Item {
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        // Current lyric line / status card (when track is active)
        Rectangle {
            id: lyricCard
            Layout.fillWidth: true
            implicitHeight: lyricRow.implicitHeight + Kirigami.Units.smallSpacing * 3.5
            Layout.minimumHeight: implicitHeight
            Layout.preferredHeight: implicitHeight
            radius: root.cardRadius
            color: root.surfaceColor
            border.width: 1
            border.color: root.borderColor
            visible: root.hasTrack

            RowLayout {
                id: lyricRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Kirigami.Units.smallSpacing * 2
                anchors.rightMargin: Kirigami.Units.smallSpacing * 2.5
                spacing: Kirigami.Units.smallSpacing * 2

                Rectangle {
                    Layout.preferredWidth: 3
                    Layout.fillHeight: true
                    Layout.minimumHeight: Kirigami.Units.gridUnit
                    radius: 1.5
                    color: root.hasLyrics
                        ? Kirigami.Theme.highlightColor
                        : Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.22)
                }

                PlasmaComponents3.Label {
                    id: lyricTextLabel
                    Layout.fillWidth: true
                    text: {
                        if (root.hasLyrics) {
                            return root.line.length > 0 ? root.line : "♪";
                        }
                        return root.escapeHtml(root.placeholder);
                    }
                    textFormat: Text.RichText
                    color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                    font.pointSize: root.hasLyrics
                        ? Kirigami.Theme.defaultFont.pointSize * 1.04
                        : Kirigami.Theme.defaultFont.pointSize
                    wrapMode: Text.Wrap
                    lineHeight: 1.25
                }
            }
        }

        // Idle empty state when no active player is present
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing * 2
            visible: !root.hasTrack

            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                Layout.alignment: Qt.AlignVCenter
                source: "media-optical-audio"
                opacity: 0.55
            }

            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: root.escapeHtml(root.placeholder)
                textFormat: Text.RichText
                color: Kirigami.Theme.disabledTextColor
                wrapMode: Text.Wrap
            }
        }

        // Footer: attribution / lyrics status + quick action buttons
        RowLayout {
            Layout.fillWidth: true
            visible: root.hasTrack
            spacing: Kirigami.Units.smallSpacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 0

                AttributionLabel {
                    Layout.fillWidth: true
                    attributionHtml: root.attributionHtml
                    fallbackText: root.attribution
                    fallbackUrl: root.attributionUrl
                    wrapMode: Text.Wrap
                    visible: root.showAttribution && root.hasLyrics
                }

                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: root.statusText
                    textFormat: Text.PlainText
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    visible: !(root.showAttribution && root.hasLyrics) && text.length > 0
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: 2

                PlasmaComponents3.ToolButton {
                    id: openLyricsBtn
                    icon.name: "text-x-generic"
                    text: i18n("Lyrics")
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    visible: root.hasLyrics
                    PlasmaComponents3.ToolTip.text: i18n("Open song lyrics…")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: root.openSongLyrics()
                }

                PlasmaComponents3.ToolButton {
                    id: retryBtn
                    icon.name: "view-refresh"
                    text: i18n("Retry")
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    visible: root.hasTrack
                    PlasmaComponents3.ToolTip.text: i18n("Try searching again")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: root.retryLyrics()
                }

                PlasmaComponents3.ToolButton {
                    id: discardBtn
                    icon.name: root.isLyricsDiscarded ? "view-visible" : "view-hidden"
                    text: root.isLyricsDiscarded ? i18n("Restore") : i18n("Discard")
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    visible: (root.hasLyrics || root.isLyricsDiscarded) && root.hasTrack
                    PlasmaComponents3.ToolTip.text: root.isLyricsDiscarded
                        ? i18n("Restore lyrics for this track")
                        : i18n("Discard lyrics for this track (this time)")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: root.toggleDiscardLyrics()
                }

                PlasmaComponents3.ToolButton {
                    id: blacklistBtn
                    icon.name: root.isBlacklisted ? "edit-undo" : "list-remove"
                    text: root.isBlacklisted ? i18n("Unblacklist") : i18n("Blacklist")
                    display: PlasmaComponents3.AbstractButton.IconOnly
                    visible: root.hasTrack
                    PlasmaComponents3.ToolTip.text: root.isBlacklisted
                        ? i18n("Unblacklist lyrics for this track")
                        : i18n("Blacklist lyrics for this track")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: root.toggleBlacklist()
                }
            }
        }
    }
}
