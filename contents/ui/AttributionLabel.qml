/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

/**
 * Reusable attribution label displaying rich text with individual clickable
 * links for lyric providers, makers, and uploaders.
 *
 * Used across the panel compact representation, LyricPopup, and SongLyricsWindow.
 */
PlasmaComponents3.Label {
    id: root

    property string attributionHtml: ""
    property string fallbackText: ""
    property string fallbackUrl: ""
    function escapeHtml(text) {
        if (!text) return "";
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#39;");
    }

    function isSafeUrl(url) {
        return Boolean(url && /^https?:\/\//i.test(url.trim()));
    }

    textFormat: Text.StyledText
    text: {
        if (attributionHtml.length > 0) {
            return attributionHtml;
        }
        const trimmedUrl = fallbackUrl ? fallbackUrl.trim() : "";
        if (isSafeUrl(trimmedUrl)) {
            const label = fallbackText.length > 0 ? fallbackText : trimmedUrl;
            return "<a href=\"" + escapeHtml(trimmedUrl) + "\">" + escapeHtml(label) + "</a>";
        }
        return escapeHtml(fallbackText);
    }

    font: Kirigami.Theme.smallFont
    color: Kirigami.Theme.textColor
    opacity: 0.75
    elide: Text.ElideNone
    topPadding: 0
    bottomPadding: 0

    HoverHandler {
        id: linkHover
        cursorShape: root.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    onLinkActivated: function (link) {
        if (isSafeUrl(link)) {
            Qt.openUrlExternally(link.trim());
        }
    }
}
