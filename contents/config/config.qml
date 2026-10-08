/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick

import org.kde.plasma.configuration as PlasmaConfiguration

PlasmaConfiguration.ConfigModel {
    id: root

    PlasmaConfiguration.ConfigCategory {
        name: i18n("Lyrics")
        icon: "media-optical-music"
        source: "configLyrics.qml"
    }

    PlasmaConfiguration.ConfigCategory {
        name: i18n("Source")
        icon: "emblem-symbolic-link"
        source: "configSource.qml"
    }

    PlasmaConfiguration.ConfigCategory {
        name: i18n("Appearance")
        icon: "preferences-desktop-font"
        source: "configAppearance.qml"
    }

    PlasmaConfiguration.ConfigCategory {
        name: i18n("Blacklist")
        icon: "list-remove"
        source: "configBlacklist.qml"
    }
}