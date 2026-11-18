/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "localprovider.h"
#include "lrcparser.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

LocalProvider::LocalProvider(QObject *parent)
    : LyricsProvider(parent)
{
}

bool LocalProvider::canHandle(const TrackMetadata &meta) const
{
    if (!m_enabled) {
        return false;
    }
    return !meta.url.isEmpty() || !meta.title.isEmpty();
}

void LocalProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    MissingReason failure = MissingReason::NotFound;
    for (const QString &path : localFilePaths(meta)) {
        MissingReason reason;
        Lyrics lyrics;
        if (readLyricsFile(path, meta.durationMs, lyrics, reason)) {
            Q_EMIT lyricsLoaded(generation, lyrics, id());
            return;
        }
        if (failure == MissingReason::NotFound || reason == MissingReason::SizeLimitExceeded) {
            failure = reason;
        }
    }
    Q_EMIT lyricsFailed(generation, failure, id());
}

bool LocalProvider::readLyricsFile(const QString &path, qint64 durationMs, Lyrics &lyrics,
                                   MissingReason &reason, QByteArray *source)
{
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
        reason = MissingReason::NotFound;
        return false;
    }
    if (!LyricsLimits::checkInputSize(file.size())) {
        reason = MissingReason::SizeLimitExceeded;
        return false;
    }
    const QByteArray data = file.read(LyricsLimits::kMaxInputSizeBytes + 1);
    if (!LyricsLimits::checkInputSize(data.size())) {
        reason = MissingReason::SizeLimitExceeded;
        return false;
    }
    if (file.error() != QFile::NoError) {
        reason = MissingReason::NotFound;
        return false;
    }

    const QString content = QString::fromUtf8(data);
    bool limitExceeded = false;
    const bool ok = info.suffix().compare(QLatin1String("ttml"), Qt::CaseInsensitive) == 0
        ? TtmlParser::parseTtml(content, &lyrics, durationMs, &limitExceeded)
        : LrcParser::parseLrc(content, &lyrics, durationMs, &limitExceeded);
    if (!ok || !lyrics.isUsable()) {
        reason = limitExceeded ? MissingReason::SizeLimitExceeded : MissingReason::Unsynced;
        return false;
    }

    lyrics.attribution = QStringLiteral("Local File (%1)").arg(info.fileName());
    lyrics.attributionUrl.clear();
    lyrics.attributionData = {QStringLiteral("Local File"), {}, {}};
    if (source) {
        *source = data;
    }
    return true;
}

void LocalProvider::cancel()
{
}

QString LocalProvider::importedFilePath(const QString &trackKey)
{
    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataDirectory.isEmpty() || trackKey.isEmpty()) {
        return {};
    }
    return dataDirectory + QStringLiteral("/plasma-lrc/imports/") + trackKey + QStringLiteral(".lrc");
}

QStringList LocalProvider::localFilePaths(const TrackMetadata &meta) const
{
    QStringList paths;
    // 1. Next to track audio file if url is a local file
    if (!meta.url.isEmpty()) {
        const QUrl parsedUrl(meta.url);
        QString localAudioPath;
        if (parsedUrl.isLocalFile()) {
            localAudioPath = parsedUrl.toLocalFile();
        } else if (meta.url.startsWith(QLatin1String("/"))) {
            localAudioPath = meta.url;
        }

        if (!localAudioPath.isEmpty()) {
            const QFileInfo audioInfo(localAudioPath);
            const QString baseWithoutExt = audioInfo.path() + QLatin1Char('/') + audioInfo.completeBaseName();

            // Check .ttml and .lrc beside audio
            paths.append(baseWithoutExt + QStringLiteral(".ttml"));
            paths.append(baseWithoutExt + QStringLiteral(".lrc"));
        }
    }

    // 2. Directories to search
    QStringList searchDirs;
    QString customDirectory = m_customDirectory.trimmed();
    if (customDirectory.startsWith(QLatin1String("~/"))) {
        customDirectory.replace(0, 1, QDir::homePath());
    }
    if (!customDirectory.isEmpty()) {
        searchDirs.append(customDirectory);
    }
    searchDirs.append(QDir::homePath() + QStringLiteral("/.lyrics"));
    searchDirs.append(QDir::homePath() + QStringLiteral("/Music/lyrics"));

    const QString cleanTitle = meta.title.trimmed();
    const QString cleanArtist = meta.artist.trimmed();

    QStringList candidates;
    if (!cleanArtist.isEmpty() && !cleanTitle.isEmpty()) {
        candidates.append(QStringLiteral("%1 - %2.ttml").arg(cleanArtist, cleanTitle));
        candidates.append(QStringLiteral("%1 - %2.lrc").arg(cleanArtist, cleanTitle));
        candidates.append(QStringLiteral("%1-%2.ttml").arg(cleanArtist, cleanTitle));
        candidates.append(QStringLiteral("%1-%2.lrc").arg(cleanArtist, cleanTitle));
    }
    if (!cleanTitle.isEmpty()) {
        candidates.append(QStringLiteral("%1.ttml").arg(cleanTitle));
        candidates.append(QStringLiteral("%1.lrc").arg(cleanTitle));
    }

    for (const QString &dirPath : searchDirs) {
        const QDir dir(dirPath);
        for (const QString &candidate : candidates) {
            paths.append(dir.filePath(candidate));
        }
    }

    paths.removeDuplicates();
    return paths;
}
