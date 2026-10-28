/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root

    readonly property bool vertical: Plasmoid.formFactor === PlasmaCore.Types.Vertical
    readonly property string lyricText: Plasmoid.text
    readonly property bool showBackingVocals: {
        if (Plasmoid.showBackingVocals !== undefined) {
            return Plasmoid.showBackingVocals;
        }
        if (Plasmoid.configuration && Plasmoid.configuration.showBackingVocals !== undefined) {
            return Plasmoid.configuration.showBackingVocals;
        }
        return false;
    }
    readonly property string backingText: (showBackingVocals && Plasmoid.backingText) ? Plasmoid.backingText : ""
    readonly property bool hasPrimaryLyrics: !Plasmoid.pauseHidden && Plasmoid.active && lyricText.length > 0
    readonly property bool hasBackingLyrics: !Plasmoid.pauseHidden && Plasmoid.active && showBackingVocals && backingText.length > 0
    readonly property bool hasLyrics: hasPrimaryLyrics || hasBackingLyrics
    readonly property int maxCharacters: Plasmoid.maxCharacters

    property string displayedLyric: ""
    property string displayedBacking: ""
    property int displayedLineIndex: -1
    property int wordStart: 0
    property int wordEnd: 0
    property int sungEnd: 0
    property int backingWordStart: 0
    property int backingWordEnd: 0
    property int backingSungEnd: 0

    function syncDisplayedState() {
        root.displayedLyric = root.lyricText;
        root.displayedBacking = root.backingText;
        root.displayedLineIndex = Plasmoid.lineIndex;
        root.wordStart = (Plasmoid.wordStart !== undefined ? Plasmoid.wordStart : 0);
        root.wordEnd = (Plasmoid.wordEnd !== undefined ? Plasmoid.wordEnd : 0);
        root.sungEnd = (Plasmoid.sungEnd !== undefined ? Plasmoid.sungEnd : root.wordEnd);
        const bwStart = (Plasmoid.backingWordStart !== undefined ? Plasmoid.backingWordStart : 0);
        const bwEnd = (Plasmoid.backingWordEnd !== undefined ? Plasmoid.backingWordEnd : 0);
        const strippedWord = root.stripOuterBrackets(root.backingText, bwStart, bwEnd);
        root.backingWordStart = strippedWord.start;
        root.backingWordEnd = strippedWord.end;
        const rawBgSung = (Plasmoid.backingSungEnd !== undefined ? Plasmoid.backingSungEnd : bwEnd);
        root.backingSungEnd = root.stripOuterBrackets(root.backingText, 0, rawBgSung).end;
    }

    function clearDisplayedState() {
        root.displayedLyric = "";
        root.displayedBacking = "";
        root.displayedLineIndex = -1;
        root.wordStart = 0;
        root.wordEnd = 0;
        root.sungEnd = 0;
        root.backingWordStart = 0;
        root.backingWordEnd = 0;
        root.backingSungEnd = 0;
    }

    Component.onCompleted: {
        root.syncDisplayedState();
    }

    Connections {
        target: Plasmoid

        function onLineIndexChanged() {
            if (!root.compactRepresentationItem) {
                root.syncDisplayedState();
            }
        }

        function onWordChanged() {
            if (root.displayedLineIndex >= 0 && Plasmoid.lineIndex !== root.displayedLineIndex) {
                return;
            }
            root.wordStart = (Plasmoid.wordStart !== undefined ? Plasmoid.wordStart : 0);
            root.wordEnd = (Plasmoid.wordEnd !== undefined ? Plasmoid.wordEnd : 0);
            if (Plasmoid.sungEnd !== undefined) {
                root.sungEnd = Plasmoid.sungEnd;
            } else {
                root.sungEnd = root.wordEnd;
            }
        }

        function onBackingChanged() {
            if (root.displayedLineIndex >= 0 && Plasmoid.lineIndex !== root.displayedLineIndex) {
                return;
            }
            const bwStart = (Plasmoid.backingWordStart !== undefined ? Plasmoid.backingWordStart : 0);
            const bwEnd = (Plasmoid.backingWordEnd !== undefined ? Plasmoid.backingWordEnd : 0);
            const strippedWord = root.stripOuterBrackets(root.displayedBacking, bwStart, bwEnd);
            root.backingWordStart = strippedWord.start;
            root.backingWordEnd = strippedWord.end;
            if (Plasmoid.backingSungEnd !== undefined) {
                const stripped = root.stripOuterBrackets(root.displayedBacking, 0, Plasmoid.backingSungEnd);
                root.backingSungEnd = stripped.end;
            } else {
                root.backingSungEnd = strippedWord.end;
            }
        }
    }

    Connections {
        target: root

        function onLyricTextChanged() {
            if (!root.compactRepresentationItem) {
                root.syncDisplayedState();
            }
        }

        function onBackingTextChanged() {
            if (root.compactRepresentationItem && Plasmoid.fadeTransition && root.displayedLineIndex >= 0 && Plasmoid.lineIndex !== root.displayedLineIndex) {
                return;
            }
            root.displayedBacking = root.backingText;
            const bwStart = (Plasmoid.backingWordStart !== undefined ? Plasmoid.backingWordStart : 0);
            const bwEnd = (Plasmoid.backingWordEnd !== undefined ? Plasmoid.backingWordEnd : 0);
            const strippedWord = root.stripOuterBrackets(root.backingText, bwStart, bwEnd);
            root.backingWordStart = strippedWord.start;
            root.backingWordEnd = strippedWord.end;
            if (Plasmoid.backingSungEnd !== undefined) {
                const stripped = root.stripOuterBrackets(root.backingText, 0, Plasmoid.backingSungEnd);
                root.backingSungEnd = stripped.end;
            } else {
                root.backingSungEnd = strippedWord.end;
            }
        }

        function onHasLyricsChanged() {
            if (!root.hasLyrics) {
                root.displayedLineIndex = -1;
                root.wordStart = 0;
                root.wordEnd = 0;
                root.sungEnd = 0;
                root.backingWordStart = 0;
                root.backingWordEnd = 0;
                root.backingSungEnd = 0;
                if (!root.compactRepresentationItem) {
                    root.displayedLyric = "";
                    root.displayedBacking = "";
                }
            } else {
                if (!root.compactRepresentationItem) {
                    root.syncDisplayedState();
                } else {
                    root.displayedLineIndex = Plasmoid.lineIndex;
                    root.wordStart = (Plasmoid.wordStart !== undefined ? Plasmoid.wordStart : 0);
                    root.wordEnd = (Plasmoid.wordEnd !== undefined ? Plasmoid.wordEnd : 0);
                    root.sungEnd = (Plasmoid.sungEnd !== undefined ? Plasmoid.sungEnd : root.wordEnd);
                    const bwStart = (Plasmoid.backingWordStart !== undefined ? Plasmoid.backingWordStart : 0);
                    const bwEnd = (Plasmoid.backingWordEnd !== undefined ? Plasmoid.backingWordEnd : 0);
                    const strippedWord = root.stripOuterBrackets(root.backingText, bwStart, bwEnd);
                    root.backingWordStart = strippedWord.start;
                    root.backingWordEnd = strippedWord.end;
                    const rawBgSung = (Plasmoid.backingSungEnd !== undefined ? Plasmoid.backingSungEnd : bwEnd);
                    root.backingSungEnd = root.stripOuterBrackets(root.backingText, 0, rawBgSung).end;
                }
            }
        }
    }

    Layout.minimumWidth: root.vertical ? 0 : Math.max(200, compactRepresentationItem ? compactRepresentationItem.Layout.minimumWidth : 200)
    Layout.preferredWidth: root.vertical ? -1 : Math.max(200, compactRepresentationItem ? compactRepresentationItem.Layout.preferredWidth : 200)
    implicitWidth: root.vertical ? (compactRepresentationItem ? compactRepresentationItem.implicitWidth : 0) : Math.max(200, compactRepresentationItem ? compactRepresentationItem.implicitWidth : 200)

    // Lyrics come from a third party and end up in rich text, so they have to
    // be escaped before anything is wrapped around them.
    function escapeHtml(text) {
        if (!text) return "";
        return text.replace(/&/g, "&amp;")
                   .replace(/</g, "&lt;")
                   .replace(/>/g, "&gt;")
                   .replace(/"/g, "&quot;")
                   .replace(/'/g, "&#39;");
    }

    // What has already been sung is dimmed, so the panel popup reads the way a
    // karaoke line does. A colour is avoided on purpose: any accent colour would
    // clash with the panel background.
    readonly property color dimmedColor: {
        const base = Kirigami.Theme.textColor;
        return Qt.rgba(base.r, base.g, base.b, 0.45);
    }

    // The word being sung is marked either with an underline (0), bold (1), highlight color (2), or none (3).
    readonly property color highlightColor: {
        const custom = Plasmoid.customHighlightColor ? Plasmoid.customHighlightColor.trim() : "";
        if (/^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(custom)) {
            return custom;
        }
        return Kirigami.Theme.highlightColor;
    }

    readonly property string textColorHex: {
        const tc = Kirigami.Theme.textColor;
        const r = Math.round(tc.r * 255).toString(16).padStart(2, '0');
        const g = Math.round(tc.g * 255).toString(16).padStart(2, '0');
        const b = Math.round(tc.b * 255).toString(16).padStart(2, '0');
        return "#" + r + g + b;
    }

    readonly property string grayColorHex: {
        const dt = Kirigami.Theme.disabledTextColor;
        const r = Math.round(dt.r * 255).toString(16).padStart(2, '0');
        const g = Math.round(dt.g * 255).toString(16).padStart(2, '0');
        const b = Math.round(dt.b * 255).toString(16).padStart(2, '0');
        return "#" + r + g + b;
    }

    readonly property string faintGrayColorHex: {
        const dt = Kirigami.Theme.disabledTextColor;
        const bg = Kirigami.Theme.backgroundColor;
        const r = Math.round((dt.r * 0.55 + bg.r * 0.45) * 255).toString(16).padStart(2, '0');
        const g = Math.round((dt.g * 0.55 + bg.g * 0.45) * 255).toString(16).padStart(2, '0');
        const b = Math.round((dt.b * 0.55 + bg.b * 0.45) * 255).toString(16).padStart(2, '0');
        return "#" + r + g + b;
    }

    readonly property string highlightColorHex: {
        const hc = root.highlightColor;
        if (typeof hc === "string" && hc.startsWith("#")) {
            return hc;
        }
        const r = Math.round(hc.r * 255).toString(16).padStart(2, '0');
        const g = Math.round(hc.g * 255).toString(16).padStart(2, '0');
        const b = Math.round(hc.b * 255).toString(16).padStart(2, '0');
        return "#" + r + g + b;
    }

    function stripOuterBrackets(str, bStart, bEnd) {
        if (!str || str.length === 0) {
            return { text: "", start: 0, end: 0 };
        }
        let s = 0;
        let e = str.length;
        while (s < e && str.charAt(s) === ' ') {
            s++;
        }
        const openCh = s < e ? str.charAt(s) : '';
        if (openCh === '(' || openCh === '[' || openCh === '（' || openCh === '【') {
            s++;
            while (s < e && str.charAt(s) === ' ') {
                s++;
            }
        }
        let trailing = e;
        while (trailing > s && str.charAt(trailing - 1) === ' ') {
            trailing--;
        }
        const closeCh = trailing > s ? str.charAt(trailing - 1) : '';
        if (closeCh === ')' || closeCh === ']' || closeCh === '）' || closeCh === '】') {
            trailing--;
            while (trailing > s && str.charAt(trailing - 1) === ' ') {
                trailing--;
            }
        }
        const text = str.substring(s, trailing);
        const adjStart = Math.max(0, Math.min((bStart !== undefined ? bStart : 0) - s, text.length));
        const adjEnd = Math.max(adjStart, Math.min((bEnd !== undefined ? bEnd : 0) - s, text.length));
        return { text: text, start: adjStart, end: adjEnd };
    }

    function formatActiveWord(word) {
        const escaped = escapeHtml(word);
        if (Plasmoid.wordStyle === 4) {
            return "<font color=\"" + textColorHex + "\">" + escaped + "</font>";
        }
        if (Plasmoid.wordStyle === 3) {
            return escaped;
        }
        if (Plasmoid.wordStyle === 1) {
            return "<b>" + escaped + "</b>";
        }
        if (Plasmoid.wordStyle === 2) {
            return "<font color=\"" + highlightColor + "\">" + escaped + "</font>";
        }
        return "<u>" + escaped + "</u>";
    }

    function plainLine(lineText) {
        if (!hasLyrics) {
            return displayText;
        }
        const text = lineText !== undefined ? lineText : lyricText;
        if (maxCharacters > 0 && text.length > maxCharacters) {
            return text.substring(0, maxCharacters) + "…";
        }
        return text;
    }

    readonly property string truncatedPlainLine: plainLine(lyricText)

    function formatCompactLine(lineText) {
        const text = lineText !== undefined ? lineText : displayedLyric;
        const limit = (maxCharacters > 0 && text.length > maxCharacters) ? maxCharacters : text.length;
        const suffix = (maxCharacters > 0 && text.length > maxCharacters) ? "…" : "";
        const visibleText = text.substring(0, limit);

        if (Plasmoid.wordStyle === 4) {
            if (!Plasmoid.wordSynced) {
                return "<font color=\"" + textColorHex + "\">" + escapeHtml(visibleText + suffix) + "</font>";
            }
            const sungBoundary = Math.max(0, Math.min(root.sungEnd, limit));
            if (sungBoundary <= 0) {
                return "<font color=\"" + grayColorHex + "\">" + escapeHtml(visibleText + suffix) + "</font>";
            }
            if (sungBoundary >= limit) {
                return "<font color=\"" + textColorHex + "\">" + escapeHtml(visibleText + suffix) + "</font>";
            }
            const sungPart = "<font color=\"" + textColorHex + "\">" + escapeHtml(visibleText.substring(0, sungBoundary)) + "</font>";
            const unSungPart = "<font color=\"" + grayColorHex + "\">" + escapeHtml(visibleText.substring(sungBoundary) + suffix) + "</font>";
            return sungPart + unSungPart;
        }

        const useRootOffsets = (text === root.displayedLyric);
        const rawStart = useRootOffsets ? root.wordStart : Plasmoid.wordStart;
        const rawEnd = useRootOffsets ? root.wordEnd : Plasmoid.wordEnd;
        if (Plasmoid.wordStyle === 3 || !Plasmoid.wordSynced || rawEnd <= rawStart || rawStart >= limit) {
            return escapeHtml(visibleText + suffix);
        }
        const wStart = Math.max(0, Math.min(rawStart, limit));
        const wEnd = Math.max(wStart, Math.min(rawEnd, limit));
        const activeSlice = visibleText.substring(wStart, wEnd);
        return escapeHtml(visibleText.substring(0, wStart))
            + formatActiveWord(activeSlice)
            + escapeHtml(visibleText.substring(wEnd) + suffix);
    }

    // The line with the word being sung marked, for the panel.
    readonly property string compactLine: formatCompactLine(lyricText)

    function formatBackingWord(word) {
        const escaped = escapeHtml(word);
        if (Plasmoid.wordStyle === 4 || Plasmoid.wordStyle === 3) {
            return "<font color=\"" + textColorHex + "\">" + escaped + "</font>";
        }
        if (Plasmoid.wordStyle === 1) {
            return "<font color=\"" + textColorHex + "\"><b>" + escaped + "</b></font>";
        }
        if (Plasmoid.wordStyle === 2) {
            return "<font color=\"" + highlightColorHex + "\">" + escaped + "</font>";
        }
        return "<font color=\"" + textColorHex + "\"><u>" + escaped + "</u></font>";
    }

    function formatBackingLine(text) {
        const str = text !== undefined ? text : root.displayedBacking;
        if (!str || str.length === 0) return "";
        const useRootOffsets = (str === root.displayedBacking);
        const bStart = Plasmoid.backingWordStart !== undefined ? Plasmoid.backingWordStart : 0;
        const bEnd = Plasmoid.backingWordEnd !== undefined ? Plasmoid.backingWordEnd : 0;

        const stripped = stripOuterBrackets(str, bStart, bEnd);
        const cleanText = stripped.text;
        const adjStart = useRootOffsets ? root.backingWordStart : stripped.start;
        const adjEnd = useRootOffsets ? root.backingWordEnd : stripped.end;

        const limit = (maxCharacters > 0 && cleanText.length > maxCharacters) ? maxCharacters : cleanText.length;
        const suffix = (maxCharacters > 0 && cleanText.length > maxCharacters) ? "…" : "";
        const visibleText = cleanText.substring(0, limit);

        if (Plasmoid.wordStyle === 4) {
            const bgSungBoundary = Math.max(0, Math.min(root.backingSungEnd, limit));
            if (bgSungBoundary <= 0) {
                return "<font color=\"" + faintGrayColorHex + "\">" + escapeHtml(visibleText + suffix) + "</font>";
            }
            if (bgSungBoundary >= limit) {
                return "<font color=\"" + textColorHex + "\">" + escapeHtml(visibleText + suffix) + "</font>";
            }
            const sungPart = "<font color=\"" + textColorHex + "\">" + escapeHtml(visibleText.substring(0, bgSungBoundary)) + "</font>";
            const unSungPart = "<font color=\"" + faintGrayColorHex + "\">" + escapeHtml(visibleText.substring(bgSungBoundary) + suffix) + "</font>";
            return sungPart + unSungPart;
        }

        if (Plasmoid.wordStyle === 3 || adjEnd <= adjStart || adjStart >= limit) {
            return escapeHtml(visibleText + suffix);
        }
        const wStart = Math.max(0, Math.min(adjStart, limit));
        const wEnd = Math.max(wStart, Math.min(adjEnd, limit));
        const activeSlice = visibleText.substring(wStart, wEnd);
        return escapeHtml(visibleText.substring(0, wStart))
            + formatBackingWord(activeSlice)
            + escapeHtml(visibleText.substring(wEnd) + suffix);
    }

    // The line with the word being sung marked, for the popup: what has already
    // been sung is dimmed.
    readonly property string fullLine: {
        if (!Plasmoid.wordSynced) {
            return escapeHtml(lyricText);
        }
        if (Plasmoid.wordStyle === 4) {
            const sungBoundary = Math.max(0, Math.min(root.sungEnd, lyricText.length));
            const sung = "<font color=\"" + textColorHex + "\">" + escapeHtml(lyricText.substring(0, sungBoundary)) + "</font>";
            const rest = "<font color=\"" + grayColorHex + "\">" + escapeHtml(lyricText.substring(sungBoundary)) + "</font>";
            return sung + rest;
        }
        if (Plasmoid.wordStyle === 3 || Plasmoid.word.length === 0) {
            return escapeHtml(lyricText);
        }
        const sung = "<font color=\"" + grayColorHex + "\">" + escapeHtml(lyricText.substring(0, Plasmoid.wordStart)) + "</font>";
        const current = formatActiveWord(Plasmoid.word);
        const rest = escapeHtml(lyricText.substring(Plasmoid.wordEnd));
        return sung + current + rest;
    }

    readonly property string fullBackingLine: {
        if (!backingText || backingText.length === 0) return "";
        return formatBackingLine(backingText);
    }

    readonly property string displayText: {
        if (Plasmoid.pauseHidden) {
            return "";
        }
        if (hasPrimaryLyrics) {
            return lyricText;
        }
        if (hasBackingLyrics) {
            return backingText;
        }
        if (Plasmoid.isIntro) {
            return Plasmoid.showNoteDuringPauses ? (Plasmoid.placeholderText.length > 0 ? Plasmoid.placeholderText : "♪") : "";
        }
        if (Plasmoid.isBreak) {
            return Plasmoid.showNoteDuringPauses ? (Plasmoid.placeholderText.length > 0 ? Plasmoid.placeholderText : "♪") : "";
        }
        return Plasmoid.placeholderText;
    }
    readonly property string hintText: displayText

    readonly property bool hasCredit: (Plasmoid.attributionHtml.length > 0 || Plasmoid.attribution.length > 0)
    readonly property bool hasPanelCredit: hasCredit && Plasmoid.fromSpicyLyrics
    readonly property bool isLayoutBelow: !vertical && hasPanelCredit && Plasmoid.attributionLayout === "Below"
    readonly property real availPanelHeight: (root.compactRepresentationItem && root.compactRepresentationItem.height > 0) ? root.compactRepresentationItem.height : 30
    readonly property bool fitsTwoRows: availPanelHeight >= 20
    readonly property bool hasBackingTwoRows: false

    FontMetrics {
        id: defaultFontMetrics
        font: Kirigami.Theme.defaultFont
    }

    FontMetrics {
        id: smallFontMetrics
        font: Kirigami.Theme.smallFont
    }

    readonly property real creditRowHeight: Math.min(Math.ceil(smallFontMetrics.height), Math.floor(availPanelHeight * 0.45))

    readonly property font lyricBelowFont: {
        if (!root.isLayoutBelow || !root.hasLyrics || !root.hasPanelCredit) {
            return Kirigami.Theme.defaultFont;
        }
        if (defaultFontMetrics.height + creditRowHeight <= availPanelHeight) {
            return Kirigami.Theme.defaultFont;
        }
        const sf = Kirigami.Theme.smallFont;
        const maxLyricHeight = Math.floor(availPanelHeight - creditRowHeight);
        if (smallFontMetrics.height > maxLyricHeight && maxLyricHeight >= 8) {
            return Qt.font({
                family: sf.family,
                pixelSize: maxLyricHeight,
                weight: sf.weight
            });
        }
        return sf;
    }

    readonly property real backingRowHeight: hasBackingTwoRows ? Math.max(9, Math.floor(availPanelHeight * 0.42)) : 0
    readonly property real primaryTwoRowHeight: hasBackingTwoRows ? Math.floor(availPanelHeight - backingRowHeight) : availPanelHeight

    readonly property font primaryTwoRowFont: {
        const sf = Kirigami.Theme.smallFont;
        const target = Math.max(8, Math.min(sf.pixelSize > 0 ? sf.pixelSize : 11, Math.floor(primaryTwoRowHeight - 1)));
        return Qt.font({
            family: sf.family,
            pixelSize: target,
            weight: sf.weight
        });
    }

    readonly property font backingTwoRowFont: {
        const sf = Kirigami.Theme.smallFont;
        const target = Math.max(8, Math.min(sf.pixelSize > 0 ? sf.pixelSize : 10, Math.floor(backingRowHeight - 1)));
        return Qt.font({
            family: sf.family,
            pixelSize: target,
            weight: sf.weight
        });
    }

    function formatInlineBacking(text) {
        const bg = text !== undefined ? text : root.displayedBacking;
        if (!bg || bg.length === 0) return "";
        const formatted = formatBackingLine(bg);
        if (!formatted || formatted.length === 0) return "";
        const baseColor = Plasmoid.wordStyle === 4 ? faintGrayColorHex : grayColorHex;
        return "&nbsp;&nbsp;<font size=\"-1\" color=\"" + baseColor + "\">" + formatted + "</font>";
    }

    FontMetrics {
        id: lyricFontMetrics
        font: (root.hasBackingTwoRows) ? root.primaryTwoRowFont : ((root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont)
    }

    readonly property real lyricRowHeight: {
        if (root.hasBackingTwoRows) {
            return root.primaryTwoRowHeight;
        }
        if (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) {
            return Math.max(Math.ceil(lyricFontMetrics.height), availPanelHeight - creditRowHeight);
        }
        return Math.ceil(lyricFontMetrics.height);
    }

    Plasmoid.backgroundHints: Plasmoid.location === PlasmaCore.Types.Desktop //
        ? PlasmaCore.Types.DefaultBackground
        : PlasmaCore.Types.NoBackground
    preferredRepresentation: compactRepresentation

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Copy current lyric line")
            icon.name: "edit-copy"
            visible: !Plasmoid.fromSpicyLyrics
            enabled: !Plasmoid.fromSpicyLyrics && Plasmoid.text.trim().length > 0
            onTriggered: Plasmoid.copyCurrentLine()
        },
        PlasmaCore.Action {
            text: i18n("Open song lyrics…")
            icon.name: "text-x-generic"
            enabled: Plasmoid.fullLyrics.length > 0
            onTriggered: lyricsWindow.openSong(Plasmoid.trackInfo, Plasmoid.fullLyrics,
                Plasmoid.currentLyricOffset(), Plasmoid.attribution, Plasmoid.attributionUrl, Plasmoid.attributionHtml,
                Plasmoid.lyricsValidUntilMs, Plasmoid.fromSpicyLyrics)
        },
        PlasmaCore.Action {
            text: Plasmoid.isLyricsDiscarded
                ? i18n("Restore lyrics for this track")
                : i18n("Discard lyrics for this track")
            icon.name: Plasmoid.isLyricsDiscarded ? "view-visible" : "view-hidden"
            enabled: (Plasmoid.hasLyrics || Plasmoid.isLyricsDiscarded) && Plasmoid.trackInfo.length > 0
            onTriggered: Plasmoid.toggleDiscardCurrentLyrics()
        },
        PlasmaCore.Action {
            text: Plasmoid.isCurrentTrackBlacklisted
                ? i18n("Unblacklist current track lyrics")
                : i18n("Blacklist current track lyrics")
            icon.name: Plasmoid.isCurrentTrackBlacklisted ? "edit-undo" : "list-remove"
            enabled: Plasmoid.trackInfo.length > 0
            onTriggered: Plasmoid.toggleBlacklistCurrentTrack()
        },
        PlasmaCore.Action {
            text: i18n("Diagnostics…")
            icon.name: "dialog-information"
            onTriggered: diagnosticsWindow.openDiagnostics(Plasmoid)
        }
    ]

    DiagnosticsWindow {
        id: diagnosticsWindow
        plasmoidApplet: Plasmoid
    }

    SongLyricsWindow {
        id: lyricsWindow
    }

    // Which source the line actually came from.
    readonly property string sourceName: {
        if (Plasmoid.provider.length > 0) {
            return Plasmoid.provider;
        }
        if (Plasmoid.attribution.length > 0) {
            return Plasmoid.attribution;
        }
        return Plasmoid.fromSpicyLyrics ? i18n("Unknown") : i18n("LRCLIB");
    }
    readonly property string attributionHint: {
        if (Plasmoid.isLyricsDiscarded) {
            return i18n("Lyrics discarded for this track");
        }
        if (Plasmoid.isCurrentTrackBlacklisted) {
            return i18n("Lyrics blacklisted for this track");
        }
        if (Plasmoid.hasLyrics) {
            return i18n("via %1", sourceName);
        }
        if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured) {
            if (Plasmoid.apiHasNoTimings) {
                return i18n("No synchronized lyrics available");
            }
            if (Plasmoid.apiUnreachable) {
                return Plasmoid.apiError;
            }
        }
        return i18n("No lyrics found for this track");
    }

    toolTipMainText: {
        if (Plasmoid.trackInfo.length > 0) {
            return Plasmoid.trackInfo;
        }
        return i18n("Plasma LRC");
    }
    toolTipSubText: {
        if (Plasmoid.trackInfo.length === 0) {
            return i18n("No player active");
        }
        if (Plasmoid.pauseHidden) {
            const pausedLines = [i18n("Playback paused")];
            if (Plasmoid.player.length > 0) {
                pausedLines.push(i18n("player: %1", Plasmoid.player));
            }
            return pausedLines.join("\n");
        }

        const lines = [];
        if (Plasmoid.player.length > 0) {
            lines.push(i18n("player: %1", Plasmoid.player));
        }
        lines.push(attributionHint);
        return lines.filter(Boolean).join("\n");
    }

    compactRepresentation: MouseArea {
        id: compactArea

        hoverEnabled: true
        activeFocusOnTab: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton

        Accessible.role: Accessible.StaticText
        Accessible.name: root.displayText

        // The panel gives an applet the width it asks for and does not come back
        // to it, so an empty lyric once meant a widget a few pixels wide, with the
        // text wrapped to one letter per line. The floor keeps a usable width
        // (at least 200px on horizontal panels) whatever the content happens to be.
        readonly property int floorWidth: root.vertical ? 0 : 200

        readonly property bool showIconItem: Plasmoid.showIcon && !Plasmoid.pauseHidden
        readonly property real iconReservedWidth: showIconItem ? Kirigami.Units.iconSizes.small : 0
        readonly property bool creditItemVisible: root.hasLyrics && root.hasPanelCredit
        readonly property real creditFullWidth: creditItemVisible ? Math.ceil(creditLabel.implicitWidth) : 0

        readonly property real primaryDesiredWidth: (typeof lyricContainer !== "undefined" && lyricContainer) ? lyricContainer.primaryTargetWidth : 0
        readonly property real backingDesiredWidth: (root.hasBackingTwoRows && typeof lyricContainer !== "undefined" && lyricContainer) ? lyricContainer.backingTargetWidth : 0
        readonly property real lyricDesiredWidth: Math.max(primaryDesiredWidth, backingDesiredWidth)
        readonly property real usefulLyricWidth: root.vertical ? 0 : Math.min(floorWidth, lyricDesiredWidth)

        readonly property real spacing: compactLayout.columnSpacing

        // Below mode: noteIcon is in column 0 (spanning 2 rows), lyric and credit share column 1.
        readonly property real belowDesiredWidth: (showIconItem ? (iconReservedWidth + spacing) : 0)
            + Math.max(lyricDesiredWidth, creditFullWidth)
        readonly property real belowMinWidth: (showIconItem ? (iconReservedWidth + spacing) : 0)
            + Math.max(usefulLyricWidth, creditFullWidth)

        // Right mode: [icon?] [lyric] [credit?] in separate columns.
        readonly property int rightItemCount: (showIconItem ? 1 : 0) + 1 + (creditItemVisible ? 1 : 0)
        readonly property real rightGapsWidth: Math.max(0, rightItemCount - 1) * spacing
        readonly property real rightDesiredWidth: (showIconItem ? iconReservedWidth : 0)
            + lyricDesiredWidth
            + (creditItemVisible ? creditFullWidth : 0)
            + rightGapsWidth
        readonly property real rightMinWidth: (showIconItem ? iconReservedWidth : 0)
            + usefulLyricWidth
            + (creditItemVisible ? creditFullWidth : 0)
            + rightGapsWidth

        readonly property real desiredTotalWidth: {
            if (root.vertical) {
                return compactLayout.implicitWidth;
            }
            return Math.max(floorWidth, root.isLayoutBelow ? belowDesiredWidth : rightDesiredWidth);
        }

        readonly property real minTotalWidth: {
            if (root.vertical) {
                return 0;
            }
            return Math.max(floorWidth, root.isLayoutBelow ? belowMinWidth : rightMinWidth);
        }

        implicitWidth: desiredTotalWidth
        implicitHeight: compactLayout.implicitHeight

        Layout.preferredWidth: desiredTotalWidth
        Layout.minimumWidth: minTotalWidth
        Layout.maximumWidth: Math.max(desiredTotalWidth, minTotalWidth)
        Layout.preferredHeight: implicitHeight
        Layout.minimumHeight: implicitHeight

        onClicked: function (mouse) {
            if (creditLabel.visible) {
                const pt = creditLabel.mapFromItem(compactArea, mouse.x, mouse.y);
                if (creditLabel.hoveredLink.length > 0 || creditLabel.linkAt(pt.x, pt.y).length > 0) {
                    return;
                }
            }
            if (mouse.button === Qt.MiddleButton) {
                if (Plasmoid.middleClickAction === 1) {
                    Plasmoid.togglePlayPause();
                } else if (Plasmoid.middleClickAction === 2) {
                    Plasmoid.nextTrack();
                }
            } else if (mouse.button === Qt.LeftButton) {
                if (Plasmoid.clickAction === 1) {
                    Plasmoid.togglePlayPause();
                } else {
                    root.expanded = !root.expanded;
                }
            }
        }

        GridLayout {
            id: compactLayout

            anchors.fill: parent
            columns: root.vertical ? 1 : (root.isLayoutBelow ? (noteIcon.visible ? 2 : 1) : (noteIcon.visible ? (root.hasPanelCredit ? 3 : 2) : (root.hasPanelCredit ? 2 : 1)))
            rows: root.vertical ? (root.hasPanelCredit ? 3 : 2) : ((root.isLayoutBelow || root.hasBackingTwoRows) ? 2 : 1)
            columnSpacing: Kirigami.Units.smallSpacing * 2
            rowSpacing: 0

            Kirigami.Icon {
                id: noteIcon

                // Follows the icon theme, so that it does not look out of place
                // in the panel.
                source: Plasmoid.hasLyrics ? "audio-x-generic" : "media-playlist-repeat"
                Layout.row: 0
                Layout.column: 0
                Layout.rowSpan: (root.isLayoutBelow || root.hasBackingTwoRows) ? 2 : 1
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                Layout.minimumWidth: Kirigami.Units.iconSizes.small
                Layout.alignment: Qt.AlignCenter
                visible: Plasmoid.showIcon && !Plasmoid.pauseHidden
                opacity: root.hasLyrics ? 1.0 : (Plasmoid.hasLyrics ? 0.75 : 0.5)
            }

            TextMetrics {
                id: lineMetrics
                font: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont
                text: {
                    if (!root.hasLyrics) {
                        return root.plainLine(root.displayText);
                    }
                    const showBg = root.showBackingVocals && root.displayedBacking.length > 0;
                    const showPrim = root.displayedLyric.length > 0;
                    if (showBg) {
                        const prim = root.plainLine(root.displayedLyric);
                        const stripped = root.stripOuterBrackets(root.displayedBacking, 0, 0).text;
                        const bg = root.plainLine(stripped);
                        return showPrim ? (prim + "   " + bg) : bg;
                    }
                    if (showPrim) {
                        return root.plainLine(root.displayedLyric);
                    }
                    return root.plainLine(root.displayText);
                }
            }

            TextMetrics {
                id: backingLineMetrics
                font: Kirigami.Theme.defaultFont
                text: root.plainLine(root.backingText)
            }

            Item {
                id: lyricContainer

                readonly property real maxLyricWidth: root.maxCharacters > 0 //
                    ? Math.ceil(characterWidth.advanceWidth) * root.maxCharacters
                    : Infinity
                readonly property real primaryTargetWidth: root.maxCharacters > 0 //
                    ? Math.min(Math.ceil(lineMetrics.advanceWidth) + 4, maxLyricWidth)
                    : Math.ceil(lineMetrics.advanceWidth) + 4
                readonly property real backingTargetWidth: primaryTargetWidth
                readonly property real targetWidth: primaryTargetWidth
                readonly property bool lineFadeAnimRunning: lineFadeAnim.running

                implicitWidth: targetWidth
                implicitHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricRowHeight : lyricLabel.implicitHeight

                Layout.row: root.vertical ? 1 : 0
                Layout.column: root.vertical ? 0 : (noteIcon.visible ? 1 : 0)
                Layout.rowSpan: 1
                Layout.fillWidth: true
                Layout.fillHeight: root.isLayoutBelow
                Layout.minimumWidth: root.vertical ? 0 : compactArea.usefulLyricWidth
                Layout.preferredWidth: targetWidth
                Layout.preferredHeight: implicitHeight
                Layout.maximumWidth: root.maxCharacters > 0 //
                    ? Math.max(maxLyricWidth, compactArea.floorWidth)
                    : Math.max(implicitWidth, Layout.minimumWidth)
                Layout.alignment: Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) :
                    (root.isLayoutBelow ? (Qt.AlignHCenter | Qt.AlignVCenter) : Qt.AlignCenter)

                PlasmaComponents3.Label {
                    id: lyricLabel
                    anchors.fill: parent
                    text: {
                        if (!root.hasLyrics) {
                            return root.escapeHtml(root.displayText);
                        }
                        const showBg = root.showBackingVocals && root.displayedBacking.length > 0;
                        const showPrim = root.displayedLyric.length > 0;
                        if (showBg) {
                            const bgFmt = root.formatInlineBacking(root.displayedBacking);
                            if (showPrim) {
                                return root.formatCompactLine(root.displayedLyric) + bgFmt;
                            }
                            return bgFmt.trim();
                        }
                        if (showPrim) {
                            return root.formatCompactLine(root.displayedLyric);
                        }
                        return root.escapeHtml(root.displayText);
                    }
                    textFormat: Text.StyledText
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    font: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.lyricBelowFont : Kirigami.Theme.defaultFont
                    color: root.hasLyrics ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    horizontalAlignment: Plasmoid.textAlignment === 1 ? Text.AlignLeft : Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    opacity: 1.0
                }

                SequentialAnimation {
                    id: lineFadeAnim

                    NumberAnimation {
                        target: lyricLabel
                        property: "opacity"
                        to: 0.2
                        duration: 35
                        easing.type: Easing.InOutQuad
                    }
                    ScriptAction {
                        script: {
                            root.syncDisplayedState();
                        }
                    }
                    NumberAnimation {
                        target: lyricLabel
                        property: "opacity"
                        to: 1.0
                        duration: 55
                        easing.type: Easing.OutCubic
                    }
                }

                Connections {
                    target: Plasmoid

                    function onLineIndexChanged() {
                        if (Plasmoid.lineIndex < 0) {
                            lineFadeAnim.stop();
                            root.clearDisplayedState();
                            lyricLabel.opacity = 1.0;
                            return;
                        }

                        if (Plasmoid.lineIndex === root.displayedLineIndex) {
                            return;
                        }

                        const hadDisplayedContent = (root.displayedLyric.length > 0 || root.displayedBacking.length > 0);
                        const hasNewContent = (root.lyricText.length > 0 || root.backingText.length > 0);

                        if (!Plasmoid.fadeTransition || !root.hasLyrics || !hadDisplayedContent || !hasNewContent) {
                            lineFadeAnim.stop();
                            root.syncDisplayedState();
                            lyricLabel.opacity = 1.0;
                        } else {
                            lineFadeAnim.stop();
                            lineFadeAnim.restart();
                        }
                    }
                }

                Connections {
                    target: root

                    function onLyricTextChanged() {
                        if (!Plasmoid.active || !Plasmoid.fadeTransition) {
                            root.syncDisplayedState();
                        }
                    }

                    function onBackingTextChanged() {
                        if (Plasmoid.fadeTransition && root.displayedLineIndex >= 0 && Plasmoid.lineIndex !== root.displayedLineIndex) {
                            return;
                        }
                        root.displayedBacking = root.backingText;
                    }

                    function onHasLyricsChanged() {
                        lineFadeAnim.stop();
                        if (!root.hasLyrics) {
                            root.clearDisplayedState();
                            lyricLabel.opacity = 1.0;
                        } else {
                            root.syncDisplayedState();
                            lyricLabel.opacity = 1.0;
                        }
                    }
                }
            }

            Item {
                id: backingContainer

                implicitWidth: lyricContainer.targetWidth
                implicitHeight: 0

                Layout.row: 1
                Layout.column: root.vertical ? 0 : (noteIcon.visible ? 1 : 0)
                Layout.rowSpan: 1
                Layout.fillWidth: false
                Layout.fillHeight: false
                visible: false
            }

            AttributionLabel {
                id: creditLabel

                attributionHtml: Plasmoid.attributionHtml
                fallbackText: Plasmoid.attribution
                fallbackUrl: Plasmoid.attributionUrl
                visible: root.hasLyrics && root.hasPanelCredit
                font: Kirigami.Theme.smallFont
                opacity: compactArea.containsMouse ? 0.95 : 0.75
                wrapMode: root.vertical ? Text.Wrap : Text.NoWrap

                Layout.row: (root.isLayoutBelow || root.vertical) ? (root.vertical ? 2 : 1) : 0
                Layout.column: root.vertical ? 0 : (root.isLayoutBelow ? (noteIcon.visible ? 1 : 0) : (noteIcon.visible ? 2 : 1))
                Layout.rowSpan: 1
                Layout.alignment: root.vertical ? Qt.AlignHCenter :
                    (root.isLayoutBelow ?
                        (Plasmoid.textAlignment === 1 ? (Qt.AlignLeft | Qt.AlignVCenter) : (Qt.AlignHCenter | Qt.AlignVCenter)) :
                        Qt.AlignVCenter)
                Layout.preferredHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.creditRowHeight : implicitHeight
                Layout.minimumHeight: (root.isLayoutBelow && root.hasLyrics && root.hasPanelCredit) ? root.creditRowHeight : 0
                Layout.fillWidth: false
                Layout.fillHeight: false
                Layout.minimumWidth: root.vertical ? 0 : implicitWidth
                Layout.preferredWidth: implicitWidth
            }
        }
    }

    fullRepresentation: LyricPopup {
        id: fullView

        // The popup takes its size from the content. An explicit minimum or
        // maximum here makes the window open at that size instead, and Plasma
        // then remembers it in the panel configuration.

        trackInfo: Plasmoid.trackInfo
        playerName: Plasmoid.player
        showTrackInfo: Plasmoid.showTrackInfo

        line: {
            if (root.hasLyrics) {
                if (root.showBackingVocals && root.hasBackingLyrics) {
                    const bgFmt = "<font size=\"-1\" color=\"" + root.grayColorHex + "\">" + root.fullBackingLine + "</font>";
                    if (root.hasPrimaryLyrics) {
                        return root.fullLine + " " + bgFmt;
                    }
                    return bgFmt;
                }
                return root.fullLine;
            }
            if (Plasmoid.isIntro) {
                return Plasmoid.showNoteDuringPauses
                    ? ("<font color=\"" + root.grayColorHex + "\">" + i18n("♪ Intro ♪") + "</font>")
                    : ("<font color=\"" + root.grayColorHex + "\">" + i18n("Intro") + "</font>");
            }
            if (Plasmoid.isBreak) {
                return Plasmoid.showNoteDuringPauses
                    ? ("<font color=\"" + root.grayColorHex + "\">" + i18n("♪ Instrumental ♪") + "</font>")
                    : ("<font color=\"" + root.grayColorHex + "\">" + i18n("Instrumental") + "</font>");
            }
            return "";
        }
        placeholder: {
            if (Plasmoid.isLyricsDiscarded) {
                return i18n("Lyrics discarded for this track");
            }
            if (Plasmoid.isCurrentTrackBlacklisted) {
                return i18n("Lyrics are blacklisted for this track");
            }
            if (Plasmoid.pauseHidden) {
                return i18n("Playback paused");
            }
            if (Plasmoid.trackInfo.length > 0) {
                if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured && Plasmoid.apiUnreachable) {
                    return Plasmoid.apiError;
                }
                if (Plasmoid.hasSpotifyTrackId && Plasmoid.spicyConfigured && Plasmoid.apiHasNoTimings) {
                    return i18n("No synchronized lyrics available");
                }
                return i18n("No lyrics available for this track");
            }
            return i18n("No active player");
        }
        hasLyrics: Plasmoid.hasLyrics

        attribution: Plasmoid.attribution
        attributionUrl: Plasmoid.attributionUrl
        attributionHtml: Plasmoid.attributionHtml
        showAttribution: root.hasCredit
        isBlacklisted: Plasmoid.isCurrentTrackBlacklisted
        onToggleBlacklist: Plasmoid.toggleBlacklistCurrentTrack()
        isLyricsDiscarded: Plasmoid.isLyricsDiscarded
        onToggleDiscardLyrics: Plasmoid.toggleDiscardCurrentLyrics()
    }

    // Only used to find out how wide a single character is, so that the compact
    // representation can be limited to a configurable number of characters.
    TextMetrics {
        id: characterWidth

        font: Kirigami.Theme.defaultFont
        text: "0"
    }
}
