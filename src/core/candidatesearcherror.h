// SPDX-License-Identifier: MIT
#pragma once

#include "lyriclimits.h"
#include <QDateTime>
#include <QNetworkReply>
#include <limits>

namespace CandidateSearchError {
inline int retryAfterSeconds(QNetworkReply *reply)
{
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 429) return 0;
    const QByteArray header = reply->rawHeader("Retry-After").trimmed();
    bool ok = false;
    qint64 seconds = header.toLongLong(&ok);
    if (!ok) {
        const auto date = QDateTime::fromString(QString::fromLatin1(header), Qt::RFC2822Date);
        seconds = date.isValid() ? QDateTime::currentDateTimeUtc().secsTo(date) : 60;
    }
    return int(qBound(qint64(1), seconds, qint64(std::numeric_limits<int>::max() / 1000)));
}
inline MissingReason reason(QNetworkReply *reply)
{
    return LyricsLimits::isSizeLimitExceeded(reply) ? MissingReason::SizeLimitExceeded : MissingReason::Unreachable;
}
}
