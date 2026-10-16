/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lrcparser.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QVector>

class QNetworkReply;

/**
 * Native LRCLIB client with multi-tier search, candidate duration scoring,
 * and local caching ported from /home/pirkov/self/server/src/lyrics.js.
 */
class LrclibClient : public QObject
{
    Q_OBJECT

public:
    explicit LrclibClient(QObject *parent = nullptr);
    ~LrclibClient() override;

    void request(const QString &title, const QString &artist, qint64 durationMs = 0, bool fromWebPage = false);
    void cancel();

    static int scoreSearchMatch(const QJsonObject &cand, qint64 targetDurationMs);
    static bool parseCandidate(const QJsonObject &cand, Lyrics *lyrics, qint64 trackDurationMs = 0);
    static QString cacheDirectory();

Q_SIGNALS:
    void loaded(const QString &requestKey, const Lyrics &lyrics);
    void missing(const QString &requestKey);

private:
    struct SearchAttempt {
        QUrl url;
        bool isSearch = false;
    };

    void startNextAttempt();
    void handleReply(QNetworkReply *reply, const SearchAttempt &attempt);
    void finishWithCandidate(const QJsonObject &cand);
    void finishWithMissing();
    static QString makeCacheKey(const QString &title, const QString &artist, qint64 durationMs);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_activeReply;
    QHash<QString, Lyrics> m_memoryCache;

    QString m_currentKey;
    qint64 m_currentDurationMs = 0;
    QVector<SearchAttempt> m_attempts;
    int m_attemptIndex = 0;

    QJsonObject m_bestCandidate;
    int m_bestScore = -1;
};
