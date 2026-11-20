/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "customprovider.h"
#include "lrcparser.h"
#include "ttmlparser.h"
#include "lyriclimits.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
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
    const bool hasCustomHeaders = !m_headers.trimmed().isEmpty();
    if (hasCustomHeaders || !url.userInfo().isEmpty()) {
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    }

    if (hasCustomHeaders) {
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
    LyricsLimits::attachSizeLimitGuard(m_currentReply);
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
    const quint64 generation = m_currentGeneration;

    if (LyricsLimits::isSizeLimitExceeded(reply)) {
        Q_EMIT lyricsFailed(generation, MissingReason::SizeLimitExceeded, id());
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT lyricsFailed(generation, MissingReason::Unreachable, id());
        return;
    }

    const QByteArray rawData = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(rawData);
    QStringList texts;
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        texts = {root.value(QStringLiteral("lyrics")).toString(),
                 root.value(QStringLiteral("syncedLyrics")).toString(),
                 root.value(QStringLiteral("ttmlLyrics")).toString(),
                 root.value(QStringLiteral("data")).toObject().value(QStringLiteral("lyrics")).toString()};
    } else if (doc.isNull()) {
        texts.append(QString::fromUtf8(rawData));
    }

    MissingReason reason = MissingReason::NotFound;
    for (const QString &text : texts) {
        if (text.trimmed().isEmpty()) {
            continue;
        }
        Lyrics lyrics;
        bool limitExceeded = false;
        const bool ok = text.contains(QLatin1String("<tt"), Qt::CaseInsensitive)
            ? TtmlParser::parseTtml(text, &lyrics, m_meta.durationMs, &limitExceeded)
            : LrcParser::parseLrc(text, &lyrics, m_meta.durationMs, &limitExceeded);
        if (limitExceeded) {
            reason = MissingReason::SizeLimitExceeded;
            break;
        }
        if (ok && lyrics.isUsable()) {
            lyrics.attribution = QStringLiteral("Custom URL");
            lyrics.attributionUrl.clear();
            lyrics.attributionData = {QStringLiteral("Custom URL"), {}, {}};
            Q_EMIT lyricsLoaded(generation, lyrics, id());
            return;
        }
        reason = MissingReason::Unsynced;
    }
    Q_EMIT lyricsFailed(generation, reason, id());
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
