/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"

namespace LyricSelection
{
enum class Voice {
    Any,
    Main,
    Background,
};

/**
 * Select the active or held word in @p line at @p positionMs.
 *
 * Non-monotonic or overlapping word timings are supported.
 * For Voice::Any, an active main word is prioritized over active background words.
 * Among active words of the same voice, the word with the latest start time is selected,
 * with deterministic tie-breaking.
 *
 * When no active matching word is found and @p holdMs > 0, gap hold selects the most
 * recently ended word within the hold window (never future words, and never indefinite/zero
 * duration intervals). For Voice::Any, main words are prioritized over background words in hold.
 *
 * If @p line.isBackground is true, all words in the line are treated as background voice.
 *
 * @param line LyricLine containing words to search.
 * @param positionMs Current playback position in milliseconds.
 * @param voice Voice filter: Any, Main, or Background.
 * @param holdMs Maximum milliseconds to hold the previous word during gaps.
 * @return Index into line.words, or -1 if no word is active or held.
 */
inline int wordAt(const LyricLine &line, qint64 positionMs, Voice voice = Voice::Any, qint64 holdMs = 0)
{
    if (line.words.isEmpty()) {
        return -1;
    }

    auto isWordBg = [&line](const LyricWord &w) -> bool {
        return line.isBackground || w.isBackground;
    };

    auto matchesVoice = [&](const LyricWord &w, Voice v) -> bool {
        const bool bg = isWordBg(w);
        switch (v) {
        case Voice::Main:
            return !bg;
        case Voice::Background:
            return bg;
        case Voice::Any:
            return true;
        }
        return false;
    };

    int bestActiveMain = -1;
    qint64 bestActiveMainStart = -1;
    int bestActiveBg = -1;
    qint64 bestActiveBgStart = -1;

    for (int i = 0; i < line.words.size(); ++i) {
        const LyricWord &w = line.words.at(i);
        if (positionMs >= w.startMs && positionMs < w.endMs) {
            const bool bg = isWordBg(w);
            if (!bg && matchesVoice(w, voice)) {
                if (bestActiveMain == -1 || w.startMs > bestActiveMainStart || (w.startMs == bestActiveMainStart && i > bestActiveMain)) {
                    bestActiveMain = i;
                    bestActiveMainStart = w.startMs;
                }
            } else if (bg && matchesVoice(w, voice)) {
                if (bestActiveBg == -1 || w.startMs > bestActiveBgStart || (w.startMs == bestActiveBgStart && i > bestActiveBg)) {
                    bestActiveBg = i;
                    bestActiveBgStart = w.startMs;
                }
            }
        }
    }

    if (bestActiveMain != -1) {
        return bestActiveMain;
    }
    if (bestActiveBg != -1) {
        return bestActiveBg;
    }

    if (holdMs > 0) {
        int bestHoldMain = -1;
        qint64 bestHoldMainEnd = -1;
        qint64 bestHoldMainStart = -1;
        int bestHoldBg = -1;
        qint64 bestHoldBgEnd = -1;
        qint64 bestHoldBgStart = -1;

        for (int i = 0; i < line.words.size(); ++i) {
            const LyricWord &w = line.words.at(i);
            if (w.endMs > w.startMs && w.endMs <= positionMs && (positionMs - w.endMs) <= holdMs) {
                const bool bg = isWordBg(w);
                if (!bg && matchesVoice(w, voice)) {
                    if (bestHoldMain == -1
                        || w.endMs > bestHoldMainEnd
                        || (w.endMs == bestHoldMainEnd && w.startMs > bestHoldMainStart)
                        || (w.endMs == bestHoldMainEnd && w.startMs == bestHoldMainStart && i > bestHoldMain)) {
                        bestHoldMain = i;
                        bestHoldMainEnd = w.endMs;
                        bestHoldMainStart = w.startMs;
                    }
                } else if (bg && matchesVoice(w, voice)) {
                    if (bestHoldBg == -1
                        || w.endMs > bestHoldBgEnd
                        || (w.endMs == bestHoldBgEnd && w.startMs > bestHoldBgStart)
                        || (w.endMs == bestHoldBgEnd && w.startMs == bestHoldBgStart && i > bestHoldBg)) {
                        bestHoldBg = i;
                        bestHoldBgEnd = w.endMs;
                        bestHoldBgStart = w.startMs;
                    }
                }
            }
        }

        if (bestHoldMain != -1) {
            return bestHoldMain;
        }
        if (bestHoldBg != -1) {
            return bestHoldBg;
        }
    }

    return -1;
}
}
