/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QRegularExpression>
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

    // 1. Static Chinese / English notices and instrumental tags
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

    // 2. Standalone section tags in square brackets [Section] or full-width 【Section】
    // English sections: intro, outro, verse, chorus, pre-chorus, post-chorus, hook, bridge, interlude, instrumental
    // Russian sections: куплет, припев, бридж, интро, аутро, вступление, кода, предприпев
    // With optional number (Verse 1, Куплет 2) and optional artist attribution (e.g. [Chorus: Artist])
    if ((t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']')))
        || (t.startsWith(QChar(0x3010)) && t.endsWith(QChar(0x3011)))) {
        const QString inner = t.mid(1, t.size() - 2).trimmed();
        static const QRegularExpression s_sectionRe(
            QStringLiteral(R"(^(?:intro|outro|verse(?:\s*\d+)?|chorus(?:\s*\d+)?|pre-chorus(?:\s*\d+)?|post-chorus(?:\s*\d+)?|hook(?:\s*\d+)?|bridge(?:\s*\d+)?|interlude|instrumental|куплет(?:\s*\d+)?|припев(?:\s*\d+)?|бридж(?:\s*\d+)?|интро|аутро|вступление|кода|предприпев)(?:\s*[:：].*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (s_sectionRe.match(inner).hasMatch()) {
            return true;
        }
    }

    // 3. Explicit credit lines with colon (ASCII ':' or full-width '：' U+FF1A)
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
        QString prefix = t.left(colonIdx).trimmed();
        // Remove surrounding brackets if prefix was bracketed, e.g. [作词] or (Written by)
        if ((prefix.startsWith(QLatin1Char('[')) && prefix.endsWith(QLatin1Char(']')))
            || (prefix.startsWith(QLatin1Char('(')) && prefix.endsWith(QLatin1Char(')')))) {
            prefix = prefix.mid(1, prefix.size() - 2).trimmed();
        }

        // Split multiple roles separated by delimiters /, &, +, ,, 、, or ; (NOT by whitespace)
        static const QRegularExpression s_roleDelim(QStringLiteral(R"([/&+,、;]+)"));
        const QStringList rawParts = prefix.split(s_roleDelim, Qt::SkipEmptyParts);

        auto isKnownRole = [](const QString &roleStr) -> bool {
            const QString r = roleStr.trimmed().toLower();
            if (r.isEmpty()) return false;
            static const QStringList s_roles = {
                // Chinese
                QStringLiteral("作词"), QStringLiteral("作曲"), QStringLiteral("编曲"),
                QStringLiteral("制作人"), QStringLiteral("制作"), QStringLiteral("监制"),
                QStringLiteral("企划"), QStringLiteral("统筹"), QStringLiteral("出品"),
                QStringLiteral("发行"), QStringLiteral("混音"), QStringLiteral("母带"),
                QStringLiteral("录音"), QStringLiteral("和声"), QStringLiteral("吉他"),
                QStringLiteral("贝斯"), QStringLiteral("鼓"), QStringLiteral("键盘"),
                QStringLiteral("弦乐"), QStringLiteral("小提琴"), QStringLiteral("大提琴"),
                QStringLiteral("钢琴"), QStringLiteral("缩混"), QStringLiteral("人声"),
                QStringLiteral("演唱"), QStringLiteral("歌手"), QStringLiteral("原唱"),
                QStringLiteral("词"), QStringLiteral("曲"), QStringLiteral("版权"),
                QStringLiteral("op"), QStringLiteral("sp"),
                // English
                QStringLiteral("vocals"), QStringLiteral("producer"), QStringLiteral("mastering"),
                QStringLiteral("mixer"), QStringLiteral("mixing"), QStringLiteral("engineer"),
                QStringLiteral("personnel"), QStringLiteral("composer"), QStringLiteral("lyricist"),
                QStringLiteral("arranger"), QStringLiteral("lyrics"), QStringLiteral("words"),
                QStringLiteral("music"), QStringLiteral("artist"), QStringLiteral("performed by"),
                QStringLiteral("written by"), QStringLiteral("produced by"), QStringLiteral("recorded by"),
                QStringLiteral("lyrics by"), QStringLiteral("music by"), QStringLiteral("composed by"),
                QStringLiteral("arranged by"), QStringLiteral("mixed by"), QStringLiteral("mastered by"),
                QStringLiteral("executive producer"), QStringLiteral("mastering engineer"),
                QStringLiteral("mixing engineer"), QStringLiteral("recording engineer"),
                QStringLiteral("sound engineer"), QStringLiteral("lead vocals"),
                QStringLiteral("backing vocals"), QStringLiteral("background vocals"),
                // Russian
                QStringLiteral("слова"), QStringLiteral("музыка"), QStringLiteral("вокал"),
                QStringLiteral("продюсер"), QStringLiteral("сведение"), QStringLiteral("мастеринг"),
                QStringLiteral("исполнитель"), QStringLiteral("автор слов"), QStringLiteral("автор музыки"),
                QStringLiteral("аранжировка")
            };
            return s_roles.contains(r);
        };

        if (!rawParts.isEmpty()) {
            bool allMatch = true;
            for (const QString &part : rawParts) {
                if (!isKnownRole(part)) {
                    allMatch = false;
                    break;
                }
            }
            if (allMatch) {
                return true;
            }
        }
    }

    // 4. Format without colon: ONLY with a clearly separated known role designation and a non-empty value
    static const QStringList s_separatedPrefixes = {
        QStringLiteral("written by "),
        QStringLiteral("produced by "),
        QStringLiteral("recorded by "),
        QStringLiteral("lyrics by "),
        QStringLiteral("composed by "),
        QStringLiteral("arranged by "),
        QStringLiteral("mixed by "),
        QStringLiteral("mastered by "),
        QStringLiteral("music by "),
        QStringLiteral("автор слов "),
        QStringLiteral("автор музыки "),
        QStringLiteral("作词 "),
        QStringLiteral("作曲 "),
        QStringLiteral("编曲 "),
        QStringLiteral("制作人 "),
        QStringLiteral("制作 "),
        QStringLiteral("监制 "),
        QStringLiteral("演唱 "),
        QStringLiteral("歌手 "),
        QStringLiteral("出品 "),
        QStringLiteral("词 "),
        QStringLiteral("曲 ")
    };

    for (const QString &sep : s_separatedPrefixes) {
        if (t.startsWith(sep, Qt::CaseInsensitive)) {
            const QString value = t.mid(sep.size()).trimmed();
            if (!value.isEmpty()) {
                return true;
            }
        }
    }

    return false;
}
} // namespace LyricFilter
