// SPDX-License-Identifier: MIT

#include "lrcapplet.h"

#include <KConfigGroup>
#include <KLocalizedString>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace
{
constexpr int kMinOffsetMs = -2000;
constexpr int kMaxOffsetMs = 2000;
constexpr int kPreviewLineCount = 12;
const QString s_trackPreferencesGroup = QStringLiteral("Songs");

QString preferenceKey(const TrackMetadata &track)
{
    if (track.title.trimmed().isEmpty()) {
        return {};
    }

    QJsonArray identity;
    const QUrl url(track.url);
    if (!track.spotifyTrackId.isEmpty()) {
        identity = {QStringLiteral("spotify"), track.spotifyTrackId};
    } else if (url.isLocalFile() || track.url.startsWith(QLatin1Char('/'))) {
        identity = {QStringLiteral("file"), QDir::cleanPath(url.isLocalFile() ? url.toLocalFile() : track.url)};
    } else {
        identity = {QStringLiteral("metadata"), track.title.simplified().toCaseFolded(),
                    track.artist.simplified().toCaseFolded(), track.album.simplified().toCaseFolded(),
                    track.durationMs / 1000};
    }
    const QByteArray serialized = QJsonDocument(identity).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(serialized, QCryptographicHash::Sha256).toHex());
}
}

TrackMetadata LrcApplet::currentTrackMetadata() const
{
    TrackMetadata meta;
    meta.title = m_trackTitle;
    meta.artist = m_trackArtist;
    meta.album = m_trackAlbum;
    meta.durationMs = m_trackLengthUs > 0 ? m_trackLengthUs / 1000 : 0;
    meta.spotifyTrackId = m_spotifyTrackId;
    meta.mprisTrackId = m_mprisTrackId;
    meta.url = m_trackUrl;
    meta.isWebSession = m_webPageSession;
    return meta;
}

bool LrcApplet::updateSongContext()
{
    const QString key = preferenceKey(currentTrackMetadata());
    if (key == m_songKey) {
        return false;
    }
    m_songKey = key;
    cancelLrcImport();
    readSongPreferences();
    return true;
}

void LrcApplet::readSongPreferences()
{
    m_songOffsetMs.reset();
    m_savedProviderId.clear();
    m_savedCandidateId.clear();
    if (!m_songKey.isEmpty()) {
        const KConfigGroup prefs = config().group(s_trackPreferencesGroup).group(m_songKey);
        if (prefs.hasKey("OffsetMs")) {
            m_songOffsetMs = qBound(kMinOffsetMs, prefs.readEntry("OffsetMs", 0), kMaxOffsetMs);
        }
        const QString providerId = prefs.readEntry("ProviderId", QString());
        const QString candidateId = prefs.readEntry("CandidateId", QString());
        if (!providerId.isEmpty() && !candidateId.isEmpty()) {
            m_savedProviderId = providerId;
            m_savedCandidateId = candidateId;
        }
    }
    Q_EMIT songPreferencesChanged();
}

void LrcApplet::rememberSongLyrics(const QString &songKey, int offsetMs)
{
    if (songKey.isEmpty() || songKey != m_songKey) {
        return;
    }

    KConfigGroup prefs = config().group(s_trackPreferencesGroup).group(m_songKey);
    prefs.writeEntry("OffsetMs", qBound(kMinOffsetMs, offsetMs, kMaxOffsetMs));
    if (m_manualCandidateSelected && !m_currentCandidateId.isEmpty()) {
        prefs.writeEntry("ProviderId", m_activeProviderId);
        prefs.writeEntry("CandidateId", m_currentCandidateId);
    }
    prefs.sync();
    readSongPreferences();
    updateWord();
}

void LrcApplet::forgetSavedMatch()
{
    if (m_savedCandidateId.isEmpty()) {
        return;
    }
    KConfigGroup prefs = config().group(s_trackPreferencesGroup).group(m_songKey);
    prefs.deleteEntry("ProviderId");
    prefs.deleteEntry("CandidateId");
    prefs.sync();
    readSongPreferences();
}

void LrcApplet::forgetSongLyrics()
{
    if (!hasSongPreferences()) {
        return;
    }
    const bool hadSavedMatch = !m_savedCandidateId.isEmpty();
    KConfigGroup prefs = config().group(s_trackPreferencesGroup).group(m_songKey);
    prefs.deleteGroup();
    prefs.sync();
    readSongPreferences();
    if (hadSavedMatch) {
        retryLyricsSearch();
    } else {
        updateWord();
    }
}

QStringList LrcApplet::lyricLines() const
{
    QStringList lines;
    if (hasLyrics()) {
        lines.reserve(m_lyrics.lines.size());
        for (const auto &line : m_lyrics.lines) {
            lines.append(line.text);
        }
    }
    return lines;
}

bool LrcApplet::hasImportedLyrics() const
{
    return QFileInfo(LocalProvider::importedFilePath(m_songKey)).isFile();
}

QVariantMap LrcApplet::previewLrcImport(const QUrl &file, const QString &songKey)
{
    cancelLrcImport();
    if (songKey.isEmpty() || songKey != m_songKey || isCurrentTrackBlacklisted()) {
        return {{QStringLiteral("error"), i18n("The track changed or is unavailable.")}};
    }
    if (!m_localProvider->isEnabled()) {
        return {{QStringLiteral("error"), i18n("Enable Local Files in Source settings to import lyrics.")}};
    }
    const QString path = file.toLocalFile();
    if (QFileInfo(path).suffix().compare(QLatin1String("lrc"), Qt::CaseInsensitive) != 0) {
        return {{QStringLiteral("error"), i18n("Choose a local .lrc file.")}};
    }

    Lyrics lyrics;
    MissingReason reason;
    if (!LocalProvider::readLyricsFile(path, currentTrackMetadata().durationMs, lyrics, reason, &m_importSource)) {
        QString error;
        switch (reason) {
        case MissingReason::SizeLimitExceeded:
            error = i18n("This file exceeds the lyrics size limits.");
            break;
        case MissingReason::Unsynced:
            error = i18n("This file has no usable synchronized lyrics.");
            break;
        default:
            error = i18n("The selected file could not be read.");
            break;
        }
        return {{QStringLiteral("error"), error}};
    }

    m_importSongKey = songKey;
    QVariantList preview;
    const qsizetype count = qMin<qsizetype>(kPreviewLineCount, lyrics.lines.size());
    for (qsizetype i = 0; i < count; ++i) {
        const LyricLine &line = lyrics.lines.at(i);
        preview.append(QVariantMap{{QStringLiteral("text"), line.text}, {QStringLiteral("startMs"), line.startMs}});
    }
    return {{QStringLiteral("lines"), preview}, {QStringLiteral("lineCount"), lyrics.lines.size()}};
}

QString LrcApplet::saveLrcImport(const QString &songKey, int offsetMs)
{
    if (songKey != m_songKey || songKey != m_importSongKey || m_importSource.isEmpty() || isCurrentTrackBlacklisted()) {
        return i18n("Choose and preview an .lrc file for the current track first.");
    }
    if (!m_localProvider->isEnabled()) {
        return i18n("Enable Local Files in Source settings to import lyrics.");
    }

    const QString path = LocalProvider::importedFilePath(m_songKey);
    QSaveFile file(path);
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly)
        || file.write(m_importSource) != m_importSource.size() || !file.commit()) {
        return i18n("Could not save the imported lyrics.");
    }

    resetManualMatch();
    forgetSavedMatch();
    rememberSongLyrics(songKey, offsetMs);
    cancelLrcImport();
    retryLyricsSearch();
    return {};
}

void LrcApplet::cancelLrcImport()
{
    m_importSongKey.clear();
    m_importSource.clear();
}

void LrcApplet::removeImportedLyrics()
{
    if (QFile::remove(LocalProvider::importedFilePath(m_songKey))) {
        Q_EMIT songPreferencesChanged();
        retryLyricsSearch();
    }
}
