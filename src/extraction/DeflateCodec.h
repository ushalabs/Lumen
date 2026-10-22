#pragma once

#include <QByteArray>
#include <cstddef>
#include <cstdint>

namespace lumen::extraction {

class DeflateCodec {
public:
    // Decompresses a raw RFC 1951 DEFLATE bitstream with a strict output size cap (ZIP-bomb protection).
    static bool inflateRaw(const uint8_t* src,
                           size_t srcLen,
                           QByteArray* outBuffer,
                           size_t maxOutputBytes = 16 * 1024 * 1024);

    // Decompresses a zlib-wrapped (RFC 1950) or raw DEFLATE stream (used by PDF /FlateDecode).
    static bool inflateZlibOrRaw(const uint8_t* src,
                                 size_t srcLen,
                                 QByteArray* outBuffer,
                                 size_t maxOutputBytes = 16 * 1024 * 1024);

    // Creates a valid raw DEFLATE or zlib-wrapped compressed buffer (useful for unit tests & synthetic DOCX/PDF).
    static QByteArray compressZlibStored(const QByteArray& rawInput);
};

} // namespace lumen::extraction
