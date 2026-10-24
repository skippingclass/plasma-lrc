/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "localprovider.h"
#include "lrcparser.h"
#include "ttmlparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    m_currentGeneration = generation;

    const QString filePath = findLocalFile(meta);
    if (filePath.isEmpty()) {
        Q_EMIT lyricsFailed(generation, MissingReason::NotFound, id());
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT lyricsFailed(generation, MissingReason::NotFound, id());
        return;
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    Lyrics lyrics;
    bool ok = false;

    if (filePath.endsWith(QLatin1String(".ttml"), Qt::CaseInsensitive)
        || content.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
        ok = TtmlParser::parseTtml(content, &lyrics, meta.durationMs);
    } else {
        ok = LrcParser::parseLrc(content, &lyrics, meta.durationMs);
    }

    if (!ok || !lyrics.isUsable()) {
        Q_EMIT lyricsFailed(generation, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attribution = QStringLiteral("Local File (%1)").arg(QFileInfo(filePath).fileName());
    lyrics.attributionData.provider = QStringLiteral("Local File");

    Q_EMIT lyricsLoaded(generation, lyrics, id());
}

void LocalProvider::cancel()
{
}

QString LocalProvider::findLocalFile(const TrackMetadata &meta) const
{
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
            const QString besideTtml = baseWithoutExt + QStringLiteral(".ttml");
            if (QFileInfo::exists(besideTtml)) {
                return besideTtml;
            }
            const QString besideLrc = baseWithoutExt + QStringLiteral(".lrc");
            if (QFileInfo::exists(besideLrc)) {
                return besideLrc;
            }
        }
    }

    // 2. Directories to search
    QStringList searchDirs;
    if (!m_customDirectory.trimmed().isEmpty()) {
        searchDirs.append(m_customDirectory.trimmed());
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
        if (!dir.exists()) {
            continue;
        }
        for (const QString &candidate : candidates) {
            const QString full = dir.filePath(candidate);
            if (QFileInfo::exists(full)) {
                return full;
            }
        }
    }

    return QString();
}
