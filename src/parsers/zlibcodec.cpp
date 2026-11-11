/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 */

#include "zlibcodec.h"
#include "lyriclimits.h"

#include <cstring>
#include <zlib.h>

namespace ZlibCodec {

static QByteArray decompressWithWindowBits(const QByteArray &data, int windowBits, bool *outLimitExceeded = nullptr)
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
        const int produced = static_cast<int>(sizeof(buffer) - strm.avail_out);
        if (produced > 0) {
            if (static_cast<qint64>(out.size()) + produced > LyricsLimits::kMaxDecodedSizeBytes) {
                if (outLimitExceeded) {
                    *outLimitExceeded = true;
                }
                inflateEnd(&strm);
                return QByteArray();
            }
            out.append(buffer, produced);
        }
    } while (strm.avail_out == 0 && ret != Z_STREAM_END);

    inflateEnd(&strm);
    return (ret == Z_STREAM_END || !out.isEmpty()) ? out : QByteArray();
}

QByteArray decompress(const QByteArray &data, bool *outLimitExceeded)
{
    if (outLimitExceeded) {
        *outLimitExceeded = false;
    }
    if (data.isEmpty()) {
        return QByteArray();
    }
    if (!LyricsLimits::checkInputSize(data.size())) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return QByteArray();
    }

    // Try standard zlib / gzip header (15 + 32)
    bool limitExceeded = false;
    QByteArray result = decompressWithWindowBits(data, 15 + 32, &limitExceeded);
    if (limitExceeded) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return QByteArray();
    }
    if (!result.isEmpty()) {
        return result;
    }

    // Try standard zlib (15)
    result = decompressWithWindowBits(data, 15, &limitExceeded);
    if (limitExceeded) {
        if (outLimitExceeded) {
            *outLimitExceeded = true;
        }
        return QByteArray();
    }
    if (!result.isEmpty()) {
        return result;
    }

    // Try raw deflate (-15)
    result = decompressWithWindowBits(data, -15, &limitExceeded);
    if (limitExceeded && outLimitExceeded) {
        *outLimitExceeded = true;
    }
    return result;
}

} // namespace ZlibCodec
