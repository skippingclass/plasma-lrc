/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "customprovider.h"
#include "lrcparser.h"
#include "ttmlparser.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace
{
constexpr int kTimeoutMs = 8000;
} // namespace

CustomProvider::CustomProvider(QObject *parent)
    : LyricsProvider(parent)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &CustomProvider::onTimeout);
}

CustomProvider::~CustomProvider()
{
    cancel();
}

bool CustomProvider::canHandle(const TrackMetadata &meta) const
{
    if (!m_enabled || m_urlTemplate.trimmed().isEmpty()) {
        return false;
    }
    const QString tmpl = m_urlTemplate.trimmed();
    if (!tmpl.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !tmpl.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        return false;
    }
    return !meta.title.trimmed().isEmpty();
}

void CustomProvider::requestLyrics(const TrackMetadata &meta, quint64 generation)
{
    cancel();

    m_meta = meta;
    m_currentGeneration = generation;

    const QString urlStr = buildUrl(meta);
    const QUrl url(urlStr);
    if (!url.isValid()) {
        Q_EMIT lyricsFailed(generation, MissingReason::NotFound, id());
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) Plasma-LRC/1.7"));

    if (!m_headers.trimmed().isEmpty()) {
        const QStringList lines = m_headers.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const int colonIdx = line.indexOf(QLatin1Char(':'));
            if (colonIdx > 0) {
                const QByteArray name = line.left(colonIdx).trimmed().toUtf8();
                const QByteArray val = line.mid(colonIdx + 1).trimmed().toUtf8();
                if (!name.isEmpty() && !val.isEmpty()) {
                    req.setRawHeader(name, val);
                }
            }
        }
    }

    m_currentReply = m_network.get(req);
    connect(m_currentReply, &QNetworkReply::finished, this, &CustomProvider::onReplyFinished);
    m_watchdog.start(kTimeoutMs);
}

void CustomProvider::cancel()
{
    m_watchdog.stop();
    if (m_currentReply) {
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
}

void CustomProvider::onTimeout()
{
    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
}

void CustomProvider::onReplyFinished()
{
    m_watchdog.stop();
    QNetworkReply *reply = m_currentReply;
    m_currentReply = nullptr;

    if (!reply) {
        return;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unreachable, id());
        return;
    }

    const QByteArray rawData = reply->readAll();
    QString text = QString::fromUtf8(rawData);

    // If it's JSON, inspect common lyrics fields
    const QJsonDocument doc = QJsonDocument::fromJson(rawData);
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.contains(QStringLiteral("lyrics")) && root.value(QStringLiteral("lyrics")).isString()) {
            text = root.value(QStringLiteral("lyrics")).toString();
        } else if (root.contains(QStringLiteral("syncedLyrics")) && root.value(QStringLiteral("syncedLyrics")).isString()) {
            text = root.value(QStringLiteral("syncedLyrics")).toString();
        } else if (root.contains(QStringLiteral("ttmlLyrics")) && root.value(QStringLiteral("ttmlLyrics")).isString()) {
            text = root.value(QStringLiteral("ttmlLyrics")).toString();
        } else if (root.contains(QStringLiteral("data")) && root.value(QStringLiteral("data")).isObject()) {
            const QJsonObject dataObj = root.value(QStringLiteral("data")).toObject();
            if (dataObj.contains(QStringLiteral("lyrics")) && dataObj.value(QStringLiteral("lyrics")).isString()) {
                text = dataObj.value(QStringLiteral("lyrics")).toString();
            }
        }
    }

    if (text.trimmed().isEmpty()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::NotFound, id());
        return;
    }

    Lyrics lyrics;
    bool ok = false;

    if (text.contains(QLatin1String("<tt"), Qt::CaseInsensitive)) {
        ok = TtmlParser::parseTtml(text, &lyrics, m_meta.durationMs);
    } else {
        ok = LrcParser::parseLrc(text, &lyrics, m_meta.durationMs);
    }

    if (!ok || !lyrics.isUsable()) {
        const quint64 gen = m_currentGeneration;
        cancel();
        Q_EMIT lyricsFailed(gen, MissingReason::Unsynced, id());
        return;
    }

    lyrics.attribution = QStringLiteral("Custom URL");
    lyrics.attributionData.provider = QStringLiteral("Custom URL");

    const quint64 gen = m_currentGeneration;
    cancel();
    Q_EMIT lyricsLoaded(gen, lyrics, id());
}

QString CustomProvider::buildUrl(const TrackMetadata &meta) const
{
    QString res = m_urlTemplate.trimmed();
    res.replace(QStringLiteral("{title}"), QString::fromUtf8(QUrl::toPercentEncoding(meta.title.trimmed())));
    res.replace(QStringLiteral("{artist}"), QString::fromUtf8(QUrl::toPercentEncoding(meta.artist.trimmed())));
    res.replace(QStringLiteral("{album}"), QString::fromUtf8(QUrl::toPercentEncoding(meta.album.trimmed())));
    res.replace(QStringLiteral("{duration}"), QString::number(meta.durationMs > 0 ? (meta.durationMs + 500) / 1000 : 0));
    res.replace(QStringLiteral("{durationMs}"), QString::number(meta.durationMs));
    res.replace(QStringLiteral("{spotifyId}"), meta.spotifyTrackId);
    return res;
}
