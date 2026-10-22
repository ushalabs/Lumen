#include "extraction/DeflateCodec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace lumen::extraction {

namespace {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t bytePos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;

    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    bool ensureBits(int n) {
        while (bitCount < n) {
            if (bytePos >= size) {
                return false;
            }
            bitBuf |= static_cast<uint32_t>(data[bytePos++]) << bitCount;
            bitCount += 8;
        }
        return true;
    }

    uint32_t peekBits(int n) const {
        return bitBuf & ((1u << n) - 1u);
    }

    void dropBits(int n) {
        bitBuf >>= n;
        bitCount -= n;
    }

    bool readBits(int n, uint32_t* outVal) {
        if (n == 0) {
            *outVal = 0;
            return true;
        }
        if (!ensureBits(n)) {
            return false;
        }
        *outVal = peekBits(n);
        dropBits(n);
        return true;
    }

    void alignToByte() {
        const int rem = bitCount & 7;
        if (rem > 0) {
            dropBits(rem);
        }
    }

    bool readAlignedBytes(uint8_t* dst, size_t count) {
        alignToByte();
        while (count > 0 && bitCount >= 8) {
            *dst++ = static_cast<uint8_t>(bitBuf & 0xFFu);
            dropBits(8);
            --count;
        }
        if (bytePos + count > size) {
            return false;
        }
        if (count > 0) {
            std::memcpy(dst, data + bytePos, count);
            bytePos += count;
        }
        return true;
    }
};

constexpr int kMaxBits = 15;
constexpr int kMaxSymbols = 288;

struct HuffmanTable {
    std::array<uint16_t, kMaxBits + 1> counts{};
    std::array<uint16_t, kMaxSymbols> symbols{};

    bool build(const uint8_t* lengths, int numSymbols) {
        counts.fill(0);
        for (int i = 0; i < numSymbols; ++i) {
            if (lengths[i] > kMaxBits) {
                return false;
            }
            counts[lengths[i]]++;
        }
        if (counts[0] == numSymbols) {
            return true;
        }

        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) {
                return false; // Over-subscribed tree
            }
        }

        std::array<uint16_t, kMaxBits + 1> offsets{};
        offsets[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) {
            offsets[len + 1] = offsets[len] + counts[len];
        }

        for (int sym = 0; sym < numSymbols; ++sym) {
            const uint8_t len = lengths[sym];
            if (len != 0) {
                symbols[offsets[len]++] = static_cast<uint16_t>(sym);
            }
        }
        return true;
    }

    bool decode(BitReader& br, int* outSymbol) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t bit = 0;
            if (!br.readBits(1, &bit)) {
                return false;
            }
            code |= static_cast<int>(bit);
            const int count = counts[len];
            if (code - count < first) {
                *outSymbol = symbols[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

constexpr uint16_t kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
constexpr uint8_t kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
constexpr uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
constexpr uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
constexpr uint8_t kCodeLenOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

} // namespace

bool DeflateCodec::inflateRaw(const uint8_t* src,
                              size_t srcLen,
                              QByteArray* outBuffer,
                              size_t maxOutputBytes) {
    if (src == nullptr || outBuffer == nullptr) {
        return false;
    }

    outBuffer->clear();
    std::vector<uint8_t> out;
    out.reserve(std::min<size_t>(srcLen * 2 + 256, 65536));

    BitReader br(src, srcLen);
    uint32_t bfinal = 0;

    do {
        if (!br.readBits(1, &bfinal)) {
            return false;
        }
        uint32_t btype = 0;
        if (!br.readBits(2, &btype)) {
            return false;
        }

        if (btype == 0) {
            // Uncompressed stored block
            uint8_t hdr[4]{};
            if (!br.readAlignedBytes(hdr, 4)) {
                return false;
            }
            const uint16_t len = static_cast<uint16_t>(hdr[0]) | (static_cast<uint16_t>(hdr[1]) << 8);
            const uint16_t nlen = static_cast<uint16_t>(hdr[2]) | (static_cast<uint16_t>(hdr[3]) << 8);
            if ((len ^ 0xFFFFu) != nlen) {
                return false;
            }
            if (out.size() + len > maxOutputBytes) {
                return false; // ZIP bomb limit exceeded
            }
            const size_t oldSize = out.size();
            out.resize(oldSize + len);
            if (!br.readAlignedBytes(out.data() + oldSize, len)) {
                return false;
            }
        } else if (btype == 1 || btype == 2) {
            HuffmanTable litLenTable;
            HuffmanTable distTable;

            if (btype == 1) {
                uint8_t litLengths[288]{};
                for (int i = 0; i <= 143; ++i) litLengths[i] = 8;
                for (int i = 144; i <= 255; ++i) litLengths[i] = 9;
                for (int i = 256; i <= 279; ++i) litLengths[i] = 7;
                for (int i = 280; i <= 287; ++i) litLengths[i] = 8;
                uint8_t distLengths[30]{};
                std::fill_n(distLengths, 30, 5);

                if (!litLenTable.build(litLengths, 288) || !distTable.build(distLengths, 30)) {
                    return false;
                }
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!br.readBits(5, &hlit) || !br.readBits(5, &hdist) || !br.readBits(4, &hclen)) {
                    return false;
                }
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 30) {
                    return false;
                }

                uint8_t codeLengths[19]{};
                for (uint32_t i = 0; i < hclen; ++i) {
                    uint32_t cl = 0;
                    if (!br.readBits(3, &cl)) {
                        return false;
                    }
                    codeLengths[kCodeLenOrder[i]] = static_cast<uint8_t>(cl);
                }

                HuffmanTable codeLenTable;
                if (!codeLenTable.build(codeLengths, 19)) {
                    return false;
                }

                uint8_t allLengths[288 + 32]{};
                const int totalCodes = static_cast<int>(hlit + hdist);
                int idx = 0;
                while (idx < totalCodes) {
                    int sym = 0;
                    if (!codeLenTable.decode(br, &sym)) {
                        return false;
                    }
                    if (sym < 16) {
                        allLengths[idx++] = static_cast<uint8_t>(sym);
                    } else {
                        uint8_t repeatVal = 0;
                        uint32_t repeatCount = 0;
                        if (sym == 16) {
                            if (idx == 0 || !br.readBits(2, &repeatCount)) {
                                return false;
                            }
                            repeatVal = allLengths[idx - 1];
                            repeatCount += 3;
                        } else if (sym == 17) {
                            if (!br.readBits(3, &repeatCount)) {
                                return false;
                            }
                            repeatCount += 3;
                        } else if (sym == 18) {
                            if (!br.readBits(7, &repeatCount)) {
                                return false;
                            }
                            repeatCount += 11;
                        } else {
                            return false;
                        }
                        if (idx + static_cast<int>(repeatCount) > totalCodes) {
                            return false;
                        }
                        for (uint32_t r = 0; r < repeatCount; ++r) {
                            allLengths[idx++] = repeatVal;
                        }
                    }
                }

                if (!litLenTable.build(allLengths, static_cast<int>(hlit)) ||
                    !distTable.build(allLengths + hlit, static_cast<int>(hdist))) {
                    return false;
                }
            }

            // Decode symbols until end-of-block (256)
            for (;;) {
                int sym = 0;
                if (!litLenTable.decode(br, &sym)) {
                    return false;
                }
                if (sym < 256) {
                    if (out.size() + 1 > maxOutputBytes) {
                        return false;
                    }
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    const int lenIdx = sym - 257;
                    if (lenIdx < 0 || lenIdx >= 29) {
                        return false;
                    }
                    uint32_t extraBits = 0;
                    if (!br.readBits(kLenExtra[lenIdx], &extraBits)) {
                        return false;
                    }
                    const size_t matchLen = kLenBase[lenIdx] + extraBits;

                    int distSym = 0;
                    if (!distTable.decode(br, &distSym) || distSym < 0 || distSym >= 30) {
                        return false;
                    }
                    uint32_t distExtra = 0;
                    if (!br.readBits(kDistExtra[distSym], &distExtra)) {
                        return false;
                    }
                    const size_t distance = kDistBase[distSym] + distExtra;
                    if (distance == 0 || distance > out.size()) {
                        return false;
                    }
                    if (out.size() + matchLen > maxOutputBytes) {
                        return false; // ZIP bomb protection
                    }
                    for (size_t m = 0; m < matchLen; ++m) {
                        const uint8_t b = out[out.size() - distance];
                        out.push_back(b);
                    }
                }
            }
        } else {
            return false; // Reserved BTYPE=3
        }
    } while (bfinal == 0);

    outBuffer->resize(static_cast<qsizetype>(out.size()));
    if (!out.empty()) {
        std::memcpy(outBuffer->data(), out.data(), out.size());
    }
    return true;
}

bool DeflateCodec::inflateZlibOrRaw(const uint8_t* src,
                                    size_t srcLen,
                                    QByteArray* outBuffer,
                                    size_t maxOutputBytes) {
    if (src == nullptr || srcLen == 0 || outBuffer == nullptr) {
        return false;
    }

    // Check for RFC 1950 2-byte zlib header (CMF + FLG where (CMF*256 + FLG) % 31 == 0 and CM == 8)
    if (srcLen >= 6 && (src[0] & 0x0Fu) == 8 && ((static_cast<uint32_t>(src[0]) << 8 | src[1]) % 31 == 0)) {
        size_t headerSkip = 2;
        if ((src[1] & 0x20u) != 0) {
            headerSkip += 4; // FDICT preset dictionary
        }
        if (srcLen > headerSkip) {
            if (inflateRaw(src + headerSkip, srcLen - headerSkip, outBuffer, maxOutputBytes)) {
                return true;
            }
        }
    }

    return inflateRaw(src, srcLen, outBuffer, maxOutputBytes);
}

QByteArray DeflateCodec::compressZlibStored(const QByteArray& rawInput) {
    QByteArray out;
    // RFC 1950 header: CMF = 0x78, FLG = 0x01 (0x7801 % 31 == 0)
    out.append(char(0x78));
    out.append(char(0x01));

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(rawInput.constData());
    size_t remaining = static_cast<size_t>(rawInput.size());

    if (remaining == 0) {
        out.append(char(0x01)); // BFINAL=1, BTYPE=00
        out.append(char(0x00));
        out.append(char(0x00));
        out.append(char(0xFF));
        out.append(char(0xFF));
    } else {
        while (remaining > 0) {
            const uint16_t chunk = static_cast<uint16_t>(std::min<size_t>(remaining, 65535));
            const bool isFinal = (remaining == chunk);
            out.append(char(isFinal ? 0x01 : 0x00));
            out.append(char(chunk & 0xFF));
            out.append(char((chunk >> 8) & 0xFF));
            const uint16_t nlen = chunk ^ 0xFFFFu;
            out.append(char(nlen & 0xFF));
            out.append(char((nlen >> 8) & 0xFF));
            out.append(reinterpret_cast<const char*>(ptr), chunk);
            ptr += chunk;
            remaining -= chunk;
        }
    }

    // Compute Adler-32 checksum
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(rawInput.constData());
    for (qsizetype i = 0; i < rawInput.size(); ++i) {
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    const uint32_t adler = (s2 << 16) | s1;
    out.append(char((adler >> 24) & 0xFF));
    out.append(char((adler >> 16) & 0xFF));
    out.append(char((adler >> 8) & 0xFF));
    out.append(char(adler & 0xFF));
    return out;
}

} // namespace lumen::extraction
