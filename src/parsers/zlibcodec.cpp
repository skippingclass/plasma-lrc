/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "zlibcodec.h"

#include <cstring>
#include <zlib.h>

namespace ZlibCodec {

static QByteArray decompressWithWindowBits(const QByteArray &data, int windowBits)
{
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = static_cast<uInt>(data.size());

    if (inflateInit2(&strm, windowBits) != Z_OK) {
        return QByteArray();
    }

    QByteArray out;
    char buffer[16384];
    int ret = Z_OK;

    do {
        strm.next_out = reinterpret_cast<Bytef *>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret < 0 && ret != Z_BUF_ERROR) {
            inflateEnd(&strm);
            return QByteArray();
        }
        out.append(buffer, static_cast<int>(sizeof(buffer) - strm.avail_out));
    } while (strm.avail_out == 0 && ret != Z_STREAM_END);

    inflateEnd(&strm);
    return (ret == Z_STREAM_END || !out.isEmpty()) ? out : QByteArray();
}

QByteArray decompress(const QByteArray &data)
{
    if (data.isEmpty()) {
        return QByteArray();
    }

    // Try standard zlib / gzip header (15 + 32)
    QByteArray result = decompressWithWindowBits(data, 15 + 32);
    if (!result.isEmpty()) {
        return result;
    }

    // Try standard zlib (15)
    result = decompressWithWindowBits(data, 15);
    if (!result.isEmpty()) {
        return result;
    }

    // Try raw deflate (-15)
    return decompressWithWindowBits(data, -15);
}

} // namespace ZlibCodec
