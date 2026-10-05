/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "trackname.h"

#include <QRegularExpression>

namespace
{
constexpr auto kCaseInsensitive = QRegularExpression::CaseInsensitiveOption;

// "(Official Video)", "[4K]", "(Lyrics)", "(MV)", "(Премьера)", "(текст песни)".
const QRegularExpression s_bracketedNoise(
    QStringLiteral(R"((\s*[[(]\s*(?:official\s*)?(?:music\s*)?(?:video|audio|lyrics?(?:\s*video)?|visuali[sz]er|mv|m/v|hd|hq|4k|live|clip(?:\s*officiel)?|videoclip|премьера[^\])]*|клип|текст(?:\s*песни)?|official)\s*[\]\)]))"),
    kCaseInsensitive);

// Everything after a pipe is the chapter list or " | Lyrics video".
const QRegularExpression s_chapterTail(QStringLiteral(R"(\s*[|｜].*$)"));

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

const QRegularExpression s_artistDashTitle(QStringLiteral(R"(^(.+?)\s+[-–—]\s+(.+)$)"));

QString tidy(QString value)
{
    return value.simplified();
}
}

QString cleanTrackTitle(QString title)
{
    title = tidy(title);
    title.remove(s_bracketedNoise);
    title.remove(s_chapterTail);
    title.remove(s_featureBracket);
    title.remove(s_featureTail);
    title = tidy(title);

    // "[Official Video] -" leaves a dangling dash behind.
    while (title.endsWith(QLatin1Char('-'))) {
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
    if (current.isEmpty() || fromWebPage || left.contains(current, Qt::CaseInsensitive)) {
        *artist = left;
        *title = right;
    }
}