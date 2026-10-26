/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QString>
#include <QStringList>

namespace LyricFilter
{
inline bool isCreditOrNotice(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return false;
    }

    static const QStringList s_notices = {
        QStringLiteral("纯音乐"),
        QStringLiteral("请欣赏"),
        QStringLiteral("请您欣赏"),
        QStringLiteral("没有填词"),
        QStringLiteral("暂无歌词"),
        QStringLiteral("无歌词"),
    };
    for (const QString &notice : s_notices) {
        if (t.contains(notice)) {
            return true;
        }
    }

    const QString lower = t.toLower();
    if (lower == QLatin1String("instrumental")
        || lower == QLatin1String("[instrumental]")
        || lower == QLatin1String("(instrumental)")) {
        return true;
    }

    int colonIdx = -1;
    const int idxAscii = t.indexOf(QLatin1Char(':'));
    const int idxFull = t.indexOf(QChar(0xFF1A));
    if (idxAscii != -1 && idxFull != -1) {
        colonIdx = qMin(idxAscii, idxFull);
    } else if (idxAscii != -1) {
        colonIdx = idxAscii;
    } else {
        colonIdx = idxFull;
    }

    if (colonIdx != -1) {
        const QString prefix = t.left(colonIdx).trimmed().toLower();
        static const QStringList s_creditPrefixes = {
            QStringLiteral("作词"), QStringLiteral("作曲"), QStringLiteral("编曲"),
            QStringLiteral("制作人"), QStringLiteral("制作"), QStringLiteral("监制"),
            QStringLiteral("企划"), QStringLiteral("统筹"), QStringLiteral("出品"),
            QStringLiteral("发行"), QStringLiteral("混音"), QStringLiteral("母带"),
            QStringLiteral("录音"), QStringLiteral("和声"), QStringLiteral("吉他"),
            QStringLiteral("贝斯"), QStringLiteral("鼓"), QStringLiteral("键盘"),
            QStringLiteral("弦乐"), QStringLiteral("小提琴"), QStringLiteral("大提琴"),
            QStringLiteral("钢琴"), QStringLiteral("缩混"), QStringLiteral("人声"),
            QStringLiteral("版权"), QStringLiteral("op"), QStringLiteral("sp"),
            QStringLiteral("vocals"), QStringLiteral("producer"), QStringLiteral("mastering"),
            QStringLiteral("mixer"), QStringLiteral("mixing"), QStringLiteral("engineer"),
            QStringLiteral("personnel"), QStringLiteral("composer"), QStringLiteral("lyricist"),
            QStringLiteral("arranger"), QStringLiteral("written by"), QStringLiteral("produced by"),
            QStringLiteral("recorded by")
        };
        for (const QString &kw : s_creditPrefixes) {
            if (prefix.contains(kw)) {
                return true;
            }
        }
    }

    static const QStringList s_startingKeywords = {
        QStringLiteral("作词"), QStringLiteral("作曲"), QStringLiteral("编曲"), QStringLiteral("制作人")
    };
    for (const QString &kw : s_startingKeywords) {
        if (t.startsWith(kw)) {
            return true;
        }
    }

    return false;
}
} // namespace LyricFilter
