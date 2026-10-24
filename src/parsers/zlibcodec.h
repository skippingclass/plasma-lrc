/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QByteArray>

namespace ZlibCodec {

/**
 * Decompress zlib, gzip or raw deflate data.
 * Returns empty QByteArray on failure.
 */
QByteArray decompress(const QByteArray &data);

} // namespace ZlibCodec
