/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>
#include <QStringView>
#include <QtCore/qnumeric.h>
#include <optional>

namespace TimedLyric
{
struct Range {
    qint64 startMs;
    qint64 endMs;
};

inline std::optional<Range> parseRange(QStringView startText, QStringView durationText, qint64 offsetMs = 0)
{
    bool startOk = false;
    bool durationOk = false;
    const qint64 start = startText.toLongLong(&startOk);
    const qint64 duration = durationText.toLongLong(&durationOk);
    qint64 startMs = 0;
    if (!startOk || !durationOk || start < 0 || duration < 0 || qAddOverflow(start, offsetMs, &startMs)) {
        return std::nullopt;
    }
    startMs = qMax(0LL, startMs);
    qint64 endMs = 0;
    if (qAddOverflow(startMs, duration, &endMs)) {
        return std::nullopt;
    }
    return Range{startMs, endMs};
}

inline QString formatSeconds(qint64 ms)
{
    return QStringLiteral("%1.%2s").arg(ms / 1000).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
}
