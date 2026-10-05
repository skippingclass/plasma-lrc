/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "playermeta.h"

#include <QDBusArgument>
#include <QDBusObjectPath>

namespace
{
// A browser hands out a list with one empty string in it rather than no list at
// all, and counting that as an artist gave points to whatever page was playing.
QStringList stringList(const QVariant &value)
{
    if (value.metaType().id() == QMetaType::QStringList) {
        return value.toStringList();
    }
    if (value.metaType().id() == QMetaType::QVariantList) {
        QStringList result;
        const QVariantList list = value.toList();
        result.reserve(list.size());
        for (const QVariant &item : list) {
            result.append(item.toString());
        }
        return result;
    }
    if (!value.isValid()) {
        return {};
    }
    return {value.toString()};
}

QVariantMap variantMap(const QVariant &value)
{
    if (value.metaType().id() == QMetaType::QVariantMap) {
        return value.toMap();
    }
    QVariantMap map;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument >> map;
    return map;
}
}

QString mprisTrackId(const QVariant &value)
{
    // There is no QMetaType enum value for it: Qt registers the name.
    if (value.metaType().name() == QLatin1String("QDBusObjectPath")) {
        return value.value<QDBusObjectPath>().path();
    }
    return value.toString();
}

QStringList trackArtists(const QVariant &value)
{
    QStringList artists = stringList(value);
    artists.removeAll(QString());
    return artists;
}

bool looksLikeWebPageSession(const QString &trackId)
{
    return trackId.startsWith(QLatin1String("/org/chromium/MediaPlayer2/"));
}

bool playerNameMatches(const QString &configured, const QString &playerName)
{
    const QString wanted = configured.trimmed();
    if (wanted.isEmpty() || playerName.isEmpty()) {
        return false;
    }
    return playerName.compare(wanted, Qt::CaseInsensitive) == 0 || playerName.contains(wanted, Qt::CaseInsensitive);
}

int musicScore(const QVariantMap &properties)
{
    const QVariantMap metadata = variantMap(properties.value(QStringLiteral("Metadata")));

    int score = 0;
    const QString status = properties.value(QStringLiteral("PlaybackStatus")).toString();
    // Playing counts for more than paused: a paused music player still describes
    // the track better than a "playing" voice message does.
    if (status == QLatin1String("Playing")) {
        score += 6;
    } else if (status == QLatin1String("Paused")) {
        score += 2;
    }

    if (metadata.value(QStringLiteral("mpris:length")).toLongLong() > 0) {
        score += 2;
    }
    const bool hasAlbum = !metadata.value(QStringLiteral("xesam:album")).toString().isEmpty();
    if (hasAlbum) {
        score += 2;
    }
    if (!metadata.value(QStringLiteral("mpris:artUrl")).toString().isEmpty()) {
        score += 1;
    }
    if (!metadata.value(QStringLiteral("xesam:trackNumber")).isNull()) {
        score += 1;
    }
    if (!trackArtists(metadata.value(QStringLiteral("xesam:artist"))).isEmpty()) {
        score += 1;
    }

    const QString trackId = mprisTrackId(metadata.value(QStringLiteral("mpris:trackid")));

    // The convention for "this is not a track" that Telegram Desktop and a few
    // others use. Their titles are not lyrics either, so they go to the back
    // rather than merely losing points.
    if (trackId.startsWith(QLatin1String("/org/desktop_app/"))) {
        score -= 10;
    }

    // A browser makes up an id in this namespace for a media session a web page
    // created for itself: a clip on a social network, an advertisement, something
    // that autoplayed. There is no track behind it and no lyrics to look up, and
    // what the page calls itself is what lands on the panel — "(2) Home / X".
    // YouTube and the rest report a real id, a URL or a file path, and are not
    // touched by this.
    if (looksLikeWebPageSession(trackId)) {
        score -= 6;
    }

    // Neither an artist nor an album is what a media session for a web page looks
    // like, while what a music player reports about a track has at least one of
    // them. Not decisive on its own: a local file without tags has neither, and it
    // is still worth looking lyrics up for.
    if (trackArtists(metadata.value(QStringLiteral("xesam:artist"))).isEmpty() && !hasAlbum) {
        score -= 3;
    }

    return score;
}
