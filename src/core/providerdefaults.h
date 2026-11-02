/* SPDX-License-Identifier: MIT */
#pragma once

#include <QStringList>

namespace ProviderDefaults
{
inline QStringList order()
{
    // Local overrides are instant; network sources follow the preferred order.
    return {QStringLiteral("local"), QStringLiteral("lrcred"), QStringLiteral("lrclib"),
            QStringLiteral("netease"), QStringLiteral("kugou"), QStringLiteral("unison"),
            QStringLiteral("spicy"), QStringLiteral("amll"), QStringLiteral("qqmusic"),
            QStringLiteral("custom")};
}
}
