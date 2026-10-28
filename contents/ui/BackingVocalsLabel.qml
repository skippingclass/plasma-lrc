/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

PlasmaComponents3.Label {
    id: root

    property string rawText: ""
    property int wordStart: 0
    property int wordEnd: 0
    property int wordStyle: 1
    property color highlightColor: Kirigami.Theme.highlightColor
    property int maxCharacters: 0
    property bool isInline: false
    property real availableHeight: 0

    font: {
        const sf = Kirigami.Theme.smallFont;
        if (availableHeight > 0 && availableHeight < 14) {
            return Qt.font({
                family: sf.family,
                pixelSize: Math.max(8, Math.floor(availableHeight - 1)),
                weight: sf.weight
            });
        }
        return sf;
    }

    function escapeHtml(text) {
        if (!text) return "";
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#39;");
    }

    function formatActiveWord(word) {
        const escaped = escapeHtml(word);
        if (root.wordStyle === 3) {
            return escaped;
        }
        if (root.wordStyle === 1) {
            return "<b>" + escaped + "</b>";
        }
        if (root.wordStyle === 2) {
            return "<font color=\"" + root.highlightColor + "\">" + escaped + "</font>";
        }
        return "<u>" + escaped + "</u>";
    }

    function formatBackingText(text, wStart, wEnd, style, limit, inlineMode) {
        if (!text || text.length === 0) return "";
        const charLimit = (limit > 0 && text.length > limit) ? limit : text.length;
        const suffix = (limit > 0 && text.length > limit) ? "…" : "";
        const visibleText = text.substring(0, charLimit);

        let formatted = "";
        if (style === 3 || wEnd <= wStart || wStart >= charLimit) {
            formatted = escapeHtml(visibleText + suffix);
        } else {
            const startIdx = Math.max(0, Math.min(wStart, charLimit));
            const endIdx = Math.max(startIdx, Math.min(wEnd, charLimit));
            const activeSlice = visibleText.substring(startIdx, endIdx);
            formatted = escapeHtml(visibleText.substring(0, startIdx))
                + formatActiveWord(activeSlice)
                + escapeHtml(visibleText.substring(endIdx) + suffix);
        }

        if (inlineMode) {
            const trimmed = visibleText.trim();
            if (trimmed.startsWith("(") || trimmed.startsWith("[") || trimmed.startsWith("（") || trimmed.startsWith("【")) {
                return formatted;
            }
            return "(" + formatted + ")";
        }
        return formatted;
    }

    readonly property string formattedText: formatBackingText(rawText, wordStart, wordEnd, wordStyle, maxCharacters, isInline)

    text: formattedText
    textFormat: Text.StyledText
    wrapMode: Text.NoWrap
    maximumLineCount: 1
    elide: Text.ElideRight
    color: Kirigami.Theme.disabledTextColor
    verticalAlignment: Text.AlignVCenter
}
