/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lyrictypes.h"
#include "trackname.h"

#include <cmath>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace TrackMatch
{

struct TrackVersion
{
    bool isLive = false;
    bool isAcoustic = false;
    bool isRemix = false;
    bool isInstrumental = false;
    bool isSlowed = false;
    bool isSpedUp = false;
    QString specificRemix;
};

inline QString normalizePunctuationAndWhitespace(QString s)
{
    s.replace(QLatin1Char('$'), QLatin1String("s"));
    s.remove(QLatin1Char('\''));
    s.remove(QChar(0x2019)); // ’
    s.remove(QChar(0x2018)); // ‘
    s.remove(QChar(0x02BC)); // ʼ
    s.remove(QLatin1Char('"'));
    s.remove(QChar(0x201C)); // “
    s.remove(QChar(0x201D)); // ”
    s.remove(QChar(0x00AB)); // «
    s.remove(QChar(0x00BB)); // »

    // Replace dashes, hyphens, colons, slashes, underscores with space
    static const QRegularExpression s_dashPunct(QStringLiteral(R"([_/\-:;~–—−\\])"));
    s.replace(s_dashPunct, QStringLiteral(" "));

    // Remove remaining punctuation like dots, commas, exclamation, question marks, brackets
    static const QRegularExpression s_removePunct(QStringLiteral(R"([.,!?@#%^&*()[\]{}（）【】《》])"));
    s.remove(s_removePunct);

    return s.simplified().toLower();
}

inline QString normalizeArtistName(const QString &artist)
{
    const QString cleaned = cleanTrackArtist(artist).trimmed();
    return normalizePunctuationAndWhitespace(cleaned);
}

inline QStringList extractArtistList(const QString &artistStr)
{
    QStringList result;
    const QString cleaned = cleanTrackArtist(artistStr);
    if (cleaned.isEmpty()) {
        return result;
    }

    // Delimiters: comma, semicolon, slash ONLY surrounded by whitespace, & (ampersand), "and", feat/ft, and Chinese ideographic comma U+3001
    // By requiring whitespace around slashes, all embedded-slash stage names (like AC/DC) are naturally preserved without hardcoded whitelists.
    static const QRegularExpression splitRe(
        QString(QStringLiteral(R"([;,]|(?:\s+/\s+)|(?:\s+and\s+)|\s*&\s*|\s+(?:feat|ft)\.?\s+|)")) + QChar(0x3001),
        QRegularExpression::CaseInsensitiveOption);

    const QStringList parts = cleaned.split(splitRe, Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString norm = normalizeArtistName(p);
        if (!norm.isEmpty() && !result.contains(norm)) {
            result.append(norm);
        }
    }
    return result;
}

inline bool areVersionsCompatible(const TrackVersion &v1, const TrackVersion &v2)
{
    if (v1.isLive != v2.isLive) {
        return false;
    }
    if (v1.isAcoustic != v2.isAcoustic) {
        return false;
    }
    if (v1.isInstrumental != v2.isInstrumental) {
        return false;
    }
    if (v1.isRemix != v2.isRemix) {
        return false;
    }
    if (v1.isSlowed != v2.isSlowed) {
        return false;
    }
    if (v1.isSpedUp != v2.isSpedUp) {
        return false;
    }
    if (v1.isRemix && v2.isRemix) {
        if (!v1.specificRemix.isEmpty() && !v2.specificRemix.isEmpty()) {
            if (v1.specificRemix != v2.specificRemix) {
                return false;
            }
        }
    }
    return true;
}

inline void inspectAndExtractVersion(const QString &text, TrackVersion &ver)
{
    const QString lower = text.toLower();

    static const QRegularExpression s_liveRe(QStringLiteral(R"((\b(live|concert|en\s+vivo|unplugged)\b|现场|現場))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression s_acousticRe(QStringLiteral(R"(\b(acoustic|piano\s+version|strings?\s+version|orchestral)\b)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression s_remixRe(QStringLiteral(R"(\b(remix|club\s+mix|extended\s+mix|dance\s+mix|vip\s+mix|dub\s+mix|dub|vip)\b)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression s_instrumentalRe(QStringLiteral(R"((\b(instrumental|karaoke)\b|伴奏))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression s_slowedRe(QStringLiteral(R"(\b(slowed(?:\s*\+\s*reverb)?|slowed\s+down|chopped\s+and\s+screwed)\b)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression s_spedUpRe(QStringLiteral(R"(\b(sped\s+up|speed\s+up|nightcore)\b)"), QRegularExpression::CaseInsensitiveOption);

    if (s_liveRe.match(lower).hasMatch()) {
        ver.isLive = true;
    }
    if (s_acousticRe.match(lower).hasMatch()) {
        ver.isAcoustic = true;
    }
    if (s_instrumentalRe.match(lower).hasMatch()) {
        ver.isInstrumental = true;
    }
    if (s_slowedRe.match(lower).hasMatch()) {
        ver.isSlowed = true;
    }
    if (s_spedUpRe.match(lower).hasMatch()) {
        ver.isSpedUp = true;
    }

    const QRegularExpressionMatch remixMatch = s_remixRe.match(lower);
    if (remixMatch.hasMatch()) {
        ver.isRemix = true;
        // Extract descriptor if present (e.g. "steve aoki remix" -> "steve aoki")
        QString desc = lower;
        desc.remove(s_remixRe);
        desc = normalizePunctuationAndWhitespace(desc);
        if (!desc.isEmpty()) {
            ver.specificRemix = desc;
        }
    }
}

inline QString normalizeTitle(const QString &title, TrackVersion *outVersion = nullptr)
{
    QString t = title.trimmed();
    TrackVersion ver;

    // 1. Check brackets: (...) or [...] or （...） or 【...】
    static const QRegularExpression s_bracketRe(QStringLiteral(R"(\s*[([（【]([^)\]）】]+)[)\]）】])"));
    auto bIt = s_bracketRe.globalMatch(t);
    while (bIt.hasNext()) {
        const auto match = bIt.next();
        inspectAndExtractVersion(match.captured(1), ver);
    }
    // Remove bracketed version tags
    static const QRegularExpression s_bracketVersionRemoveRe(
        QStringLiteral(R"(\s*[([（【][^)\]）】]*(?:\b(?:live|concert|en\s+vivo|unplugged|acoustic|piano\s+version|strings?\s+version|orchestral|remix|club\s+mix|extended\s+mix|dance\s+mix|vip|dub|instrumental|karaoke|slowed(?:\s*\+\s*reverb)?|slowed\s+down|chopped\s+and\s+screwed|sped\s+up|speed\s+up|nightcore)\b|现场|現場|伴奏)[^)\]）】]*[)\]）】])"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(s_bracketVersionRemoveRe);

    // 2. Check dash tails: " - Live...", " - Acoustic...", " - ... Remix", " - Instrumental", " - Slowed..."
    static const QRegularExpression s_dashVersionRe(
        QStringLiteral(R"(\s+[-–—−]\s+(.*(?:live|concert|en\s+vivo|unplugged|现场|現場|acoustic|piano\s+version|strings?\s+version|orchestral|remix|club\s+mix|extended\s+mix|dance\s+mix|vip|dub|instrumental|karaoke|slowed|sped\s+up|speed\s+up|nightcore|伴奏).*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto dashMatch = s_dashVersionRe.match(t);
    if (dashMatch.hasMatch()) {
        inspectAndExtractVersion(dashMatch.captured(1), ver);
        t.remove(s_dashVersionRe);
    }

    // 3. Trailing word versions: " Song Live", " Song Acoustic", " Song Remix", " Song Slowed", etc.
    static const QRegularExpression s_trailingVersionRe(
        QStringLiteral(R"(\s+\b(live|acoustic|remix|instrumental|slowed|sped\s+up|nightcore)\b.*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto trailingMatch = s_trailingVersionRe.match(t);
    if (trailingMatch.hasMatch()) {
        inspectAndExtractVersion(trailingMatch.captured(0), ver);
        t.remove(s_trailingVersionRe);
    }

    // 4. Strip feature brackets: "(feat. ...)", "(with ...)"
    static const QRegularExpression s_featureWithRe(
        QStringLiteral(R"(\s*[([（【]\s*(?:feat|ft|with)\.?\s[^)\]）】]*[)\]）】])"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(s_featureWithRe);

    // 5. Strip official video/MV/audio brackets including Chinese 官方
    static const QRegularExpression s_officialBracketRe(
        QStringLiteral(R"(\s*[([（【][^)\]）】]*(?:official|官方|video|audio|mv|m/v|lyrics?|visuali[sz]er|hd|hq|4k|clip)[^)\]）】]*[)\]）】])"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(s_officialBracketRe);

    // 6. Strip remaster / edition neutral tails (e.g. " - 2011 Remaster", "(Deluxe Edition)", "(Original Mix)")
    static const QRegularExpression s_editionBracketRe(
        QStringLiteral(R"(\s*[([（【][^)\]）】]*(?:re-?master(?:ed)?|deluxe(?:\s+edition)?|anniversary(?:\s+edition)?|original\s+mix|album\s+version|single\s+version|bonus\s+track)[^)\]）】]*[)\]）】])"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(s_editionBracketRe);

    static const QRegularExpression s_editionDashRe(
        QStringLiteral(R"(\s+[-–—−]\s+(?:\d{4}\s+)?(?:re-?master(?:ed)?|deluxe(?:\s+edition)?|anniversary(?:\s+edition)?|original\s+mix|album\s+version|single\s+version|bonus\s+track)\b.*$)"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(s_editionDashRe);

    // 7. Use existing cleanTrackTitle to strip ordinary video/noise brackets and feature brackets
    t = cleanTrackTitle(t);

    if (outVersion) {
        *outVersion = ver;
    }

    return normalizePunctuationAndWhitespace(t);
}

inline bool matchTitles(const QString &candidateTitle, const QString &targetTitle)
{
    if (candidateTitle.trimmed().isEmpty() || targetTitle.trimmed().isEmpty()) {
        return false;
    }

    TrackVersion candVer;
    TrackVersion targetVer;

    const QString normCand = normalizeTitle(candidateTitle, &candVer);
    const QString normTarget = normalizeTitle(targetTitle, &targetVer);

    if (normCand.isEmpty() || normTarget.isEmpty()) {
        return false;
    }

    if (!areVersionsCompatible(targetVer, candVer)) {
        return false;
    }

    return normCand == normTarget;
}

inline bool matchArtists(const QStringList &candidateArtists, const QString &targetArtist)
{
    const QString normTarget = normalizeArtistName(targetArtist);
    if (normTarget.isEmpty()) {
        // Target artist not known -> pass
        return true;
    }

    if (candidateArtists.isEmpty()) {
        // Target artist known, candidate artist missing -> reject
        return false;
    }

    const QStringList targetList = extractArtistList(targetArtist);
    const QString targetPrimary = targetList.isEmpty() ? normTarget : targetList.first();

    QStringList candList;
    for (const QString &c : candidateArtists) {
        const QStringList parts = extractArtistList(c);
        for (const QString &p : parts) {
            if (!candList.contains(p)) {
                candList.append(p);
            }
        }
    }

    if (candList.isEmpty()) {
        return false;
    }

    const QString candPrimary = candList.first();

    // 1. Primary artists match
    if (targetPrimary == candPrimary) {
        return true;
    }

    // 2. Exact joined match
    if (candList.join(QLatin1Char(' ')) == normTarget) {
        return true;
    }

    // 3. Candidate artist list contains target primary artist
    if (candList.contains(targetPrimary)) {
        return true;
    }

    // 4. Target artist list contains candidate primary artist
    if (targetList.contains(candPrimary)) {
        return true;
    }

    return false;
}

inline bool matchArtists(const QString &candidateArtist, const QString &targetArtist)
{
    const QString normCand = normalizeArtistName(candidateArtist);
    if (normCand.isEmpty() && !targetArtist.trimmed().isEmpty()) {
        return false;
    }
    return matchArtists(QStringList{candidateArtist}, targetArtist);
}

inline bool matchDurations(qint64 targetDurationMs, qint64 candidateDurationMs, qint64 maxDiffMs = 4000)
{
    if (targetDurationMs <= 0 || candidateDurationMs <= 0) {
        return true;
    }
    return std::abs(targetDurationMs - candidateDurationMs) <= maxDiffMs;
}

inline bool matchCandidate(const TrackMetadata &target,
                           const QString &candidateTitle,
                           const QString &candidateArtist,
                           qint64 candidateDurationMs = 0,
                           qint64 maxDurationDiffMs = 4000)
{
    if (!matchTitles(candidateTitle, target.title)) {
        return false;
    }

    if (!target.artist.trimmed().isEmpty()) {
        if (!matchArtists(candidateArtist, target.artist)) {
            return false;
        }
    }

    if (!matchDurations(target.durationMs, candidateDurationMs, maxDurationDiffMs)) {
        return false;
    }

    return true;
}

inline bool matchCandidate(const TrackMetadata &target,
                           const QString &candidateTitle,
                           const QStringList &candidateArtists,
                           qint64 candidateDurationMs = 0,
                           qint64 maxDurationDiffMs = 4000)
{
    if (!matchTitles(candidateTitle, target.title)) {
        return false;
    }

    if (!target.artist.trimmed().isEmpty()) {
        if (!matchArtists(candidateArtists, target.artist)) {
            return false;
        }
    }

    if (!matchDurations(target.durationMs, candidateDurationMs, maxDurationDiffMs)) {
        return false;
    }

    return true;
}

inline bool validateExactLookupMetadata(const TrackMetadata &target,
                                        const QString &responseTitle,
                                        const QString &responseArtist,
                                        qint64 responseDurationMs = 0,
                                        qint64 maxDurationDiffMs = 4000)
{
    const QString respTitleTrimmed = responseTitle.trimmed();
    if (!respTitleTrimmed.isEmpty() && !target.title.trimmed().isEmpty()) {
        if (!matchTitles(respTitleTrimmed, target.title)) {
            return false;
        }
    }

    const QString respArtistTrimmed = responseArtist.trimmed();
    if (!respArtistTrimmed.isEmpty() && !target.artist.trimmed().isEmpty()) {
        if (!matchArtists(respArtistTrimmed, target.artist)) {
            return false;
        }
    }

    if (responseDurationMs > 0 && target.durationMs > 0) {
        if (!matchDurations(target.durationMs, responseDurationMs, maxDurationDiffMs)) {
            return false;
        }
    }

    return true;
}

} // namespace TrackMatch
