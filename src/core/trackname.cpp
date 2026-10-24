/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "trackname.h"
#include "lyrictypes.h"

#include <QRegularExpression>

namespace
{
constexpr auto kCaseInsensitive = QRegularExpression::CaseInsensitiveOption;

// "(Official Video)", "[4K]", "(Lyrics)", "(MV)", "(Премьера)", "(текст песни)".
// Notice: "live", "remix", "acoustic", "slowed", "sped up" are preserved for strict version matching.
const QRegularExpression s_bracketedNoise(
    QStringLiteral(R"((\s*[[(]\s*(?:official\s*)?(?:music\s*)?(?:video|audio|lyrics?(?:\s*video)?|visuali[sz]er|mv|m/v|hd|hq|4k[^\])]*|1080p[^\])]*|clip(?:\s*officiel)?|videoclip|премьера[^\])]*|клип|текст(?:\s*песни)?|official)\s*[\]\)]))"),
    kCaseInsensitive);

// Everything after a pipe is the chapter list or " | Lyrics video".
const QRegularExpression s_chapterTail(QStringLiteral(R"(\s*[|｜].*$)"));

// Leading track numbers: allow whitespace between digits and separator (. or -),
// but require whitespace AFTER the separator.
// This matches "01 - ", "01. ", "01.- ", "01. - ", "12. ", while preserving
// "99-Problems" (no whitespace after dash) and "21 Guns" (no separator).
const QRegularExpression s_leadingTrackNum(QStringLiteral(R"(^\s*\d{1,3}(?:\.\s*[-–—−]?\s+|\s*[-–—−]\s+))"));

// Audio extensions: .mp3, .flac, .m4a, etc.
const QRegularExpression s_audioExt(QStringLiteral(R"(\.(?:mp3|flac|m4a|wav|ogg|opus|aac|wma)$)"), kCaseInsensitive);

// A dangling "4K" after the brackets have gone.
const QRegularExpression s_qualityTail(QStringLiteral(R"(\s+(?:4k|hd|hq|mv|m/v)\s*$)"), kCaseInsensitive);

// "(feat. Someone)" and "feat. Someone" without brackets.
const QRegularExpression s_featureBracket(QStringLiteral(R"(\s*[[(]\s*(?:feat|ft)\.?\s[^)\]]*[)\]])"), kCaseInsensitive);
const QRegularExpression s_featureTail(QStringLiteral(R"(\s+(?:feat|ft)\.?\s.*$)"), kCaseInsensitive);

const QRegularExpression s_topicSuffix(QStringLiteral(R"(\s*-\s*Topic$)"), kCaseInsensitive);
const QRegularExpression s_vevoSuffix(QStringLiteral(R"(\s*VEVO$)"), kCaseInsensitive);
const QRegularExpression s_officialSuffix(QStringLiteral(R"(\s+(?:official|официальный)\s*$)"), kCaseInsensitive);
// "Artist, Someone Else (feat. Third)" and "Artist feat. Someone".
const QRegularExpression s_artistFeatureBracket(QStringLiteral(R"(\s*[,;]?\s*\((?:feat|ft)\.?\s[^)]*\))"), kCaseInsensitive);
const QRegularExpression s_artistFeatureTail(QStringLiteral(R"(\s*[,;]?\s+(?:feat|ft)\.?\s.*$)"), kCaseInsensitive);

// Slash is a delimiter ONLY when surrounded by whitespace, preserving all embedded-slash stage names (e.g. AC/DC)
const QRegularExpression s_artistSplit(QStringLiteral(R"([;,]|(?:\s+/\s+)|(?:\s+and\s+)|\s*&\s*|\s+(?:feat|ft)\.?\s+)"), kCaseInsensitive);

const QRegularExpression s_artistDashTitle(QStringLiteral(R"(^(.+?)\s+[-–—−]\s+(.+)$)"));

QString tidy(QString value)
{
    return value.simplified();
}

} // namespace

QString cleanTrackTitle(QString title)
{
    title = tidy(title);
    title.remove(s_audioExt);
    title.remove(s_leadingTrackNum);
    title.remove(s_bracketedNoise);
    title.remove(s_chapterTail);
    title.remove(s_featureBracket);
    title.remove(s_featureTail);
    title = tidy(title);

    // "[Official Video] -" or "–" leaves a dangling dash behind.
    auto isDash = [](QChar c) {
        return c == QLatin1Char('-') || c.unicode() == 0x2013 || c.unicode() == 0x2014 || c.unicode() == 0x2212;
    };
    while (!title.isEmpty() && isDash(title.back())) {
        title.chop(1);
        title = tidy(title);
    }

    title.remove(s_qualityTail);
    return tidy(title);
}

QString cleanTrackArtist(QString artist)
{
    artist = tidy(artist);
    artist.remove(s_topicSuffix);
    artist.remove(s_vevoSuffix);
    artist.remove(s_artistFeatureBracket);
    artist.remove(s_artistFeatureTail);
    artist = tidy(artist);
    artist.remove(s_officialSuffix);
    return tidy(artist);
}

QString extractPrimaryArtist(const QString &artist)
{
    const QString cleaned = cleanTrackArtist(artist);
    if (cleaned.isEmpty()) {
        return QString();
    }
    const QStringList parts = cleaned.split(s_artistSplit, Qt::SkipEmptyParts);
    return tidy(parts.isEmpty() ? cleaned : parts.first());
}

void splitTrackArtistAndTitle(QString *artist, QString *title, bool fromWebPage)
{
    if (!artist || !title) {
        return;
    }

    const QRegularExpressionMatch match = s_artistDashTitle.match(*title);
    if (!match.hasMatch()) {
        return;
    }

    const QString left = tidy(match.captured(1));
    const QString right = tidy(match.captured(2));
    if (left.isEmpty() || right.isEmpty()) {
        return;
    }

    const QString current = tidy(*artist);
    const bool currentContainsLeft = !current.isEmpty() && current.contains(left, Qt::CaseInsensitive);
    const bool leftContainsCurrent = !current.isEmpty() && left.contains(current, Qt::CaseInsensitive);
    if (current.isEmpty() || fromWebPage || currentContainsLeft || leftContainsCurrent) {
        *artist = left;
        *title = right;
    }
}

void normalizeTrackMetadata(TrackMetadata *meta)
{
    if (!meta || meta->searchNormalized) {
        return;
    }

    // 1. Split Artist - Song for web metadata or when artist matches/is empty
    splitTrackArtistAndTitle(&meta->artist, &meta->title, meta->isWebSession);

    // 2. Clean ordinary decorations while preserving meaningful version tags
    meta->artist = cleanTrackArtist(meta->artist);
    meta->title = cleanTrackTitle(meta->title);

    meta->searchNormalized = true;
}