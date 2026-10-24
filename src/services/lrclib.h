/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lrcparser.h"
#include "lyrictypes.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QVector>

class QNetworkReply;

/**
 * Native LRCLIB client with multi-tier search, candidate identity validation,
 * duration scoring, monotonic generation tracking, and local caching.
 */
class LrclibClient : public QObject
{
    Q_OBJECT

public:
    explicit LrclibClient(QObject *parent = nullptr,
                          const QUrl &apiBase = QUrl(QStringLiteral("https://lrclib.net/api/")));
    ~LrclibClient() override;

    QString request(const TrackMetadata &meta);
    QString request(const QString &title, const QString &artist, qint64 durationMs = 0, bool fromWebPage = false);
    void cancel();
    void invalidate(const TrackMetadata &meta);
    void invalidate(const QString &title, const QString &artist, qint64 durationMs = 0, bool fromWebPage = false);
    QString currentKey() const { return m_currentKey; }

    static int scoreSearchMatch(const QJsonObject &cand, qint64 targetDurationMs, const QString &expectedTitle = QString(), const QString &expectedArtist = QString());
    static bool parseCandidate(const QJsonObject &cand, Lyrics *lyrics, qint64 trackDurationMs = 0, const QString &expectedTitle = QString(), const QString &expectedArtist = QString());
    static bool isDurationCompatible(double candDurationSec, qint64 targetDurationMs);
    static double maxAllowedDurationDiffSec(qint64 targetDurationMs);
    static bool validateCandidateIdentity(const QJsonObject &cand, const QString &expectedTitle, const QString &expectedArtist);

    static QString makeCacheKey(const QString &title, const QString &artist, qint64 durationMs);
    static QString cacheDirectory();
    static QByteArray userAgent();

Q_SIGNALS:
    void loaded(const QString &requestKey, const Lyrics &lyrics);
    void missing(const QString &requestKey);

private:
    struct SearchAttempt {
        QUrl url;
        bool isSearch = false;
    };

    void startNextAttempt();
    void executeAttempt(const SearchAttempt &attempt);
    void handleReply(QNetworkReply *reply, const SearchAttempt &attempt);
    void finishWithCandidate(const QJsonObject &cand);
    void finishWithMissing(bool writeNegativeCache = true);
    void cleanupAttemptState();

    QNetworkAccessManager m_network;
    const QUrl m_apiBase;
    QPointer<QNetworkReply> m_activeReply;
    struct CachedLyrics {
        Lyrics lyrics;
        qint64 expiresAtMs;
    };
    QHash<QString, CachedLyrics> m_memoryCache;

    QString m_currentKey;
    qint64 m_currentDurationMs = 0;
    QString m_expectedTitle;
    QString m_expectedArtist;
    QVector<SearchAttempt> m_attempts;
    int m_attemptIndex = 0;
    bool m_hadRateLimitOrNetworkError = false;
    quint64 m_generation = 0;

    QJsonObject m_bestCandidate;
    int m_bestScore = -1;
};
