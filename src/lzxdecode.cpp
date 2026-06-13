// LZX decompressor for CHM. Mirrors the encoder in lzx.cpp: 16-bit little-endian
// words, bits consumed MSB-first; verbatim / aligned-offset / uncompressed blocks;
// pretree-delta-coded Huffman trees; full state reset each reset interval and a
// 16-bit realignment every 0x8000 output bytes.
#include "lzxdecode.h"

#include <algorithm>
#include <array>

namespace fastchm {
namespace {

constexpr int NUM_CHARS = 256;
constexpr int PRETREE_SYMS = 20;
constexpr int ALIGNED_SYMS = 8;
constexpr int LEN_SYMS = 249;
constexpr int MIN_MATCH = 2;
constexpr int NUM_PRIMARY_LENGTHS = 7;
constexpr size_t FRAME = 0x8000;

const uint8_t kExtraBits[51] = {
    0,  0,  0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6, 7,
    7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15,
    16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17};
const uint32_t kPosBase[51] = {
    0,       1,       2,       3,       4,       6,       8,       12,
    16,      24,      32,      48,      64,      96,      128,     192,
    256,     384,     512,     768,     1024,    1536,    2048,    3072,
    4096,    6144,    8192,    12288,   16384,   24576,   32768,   49152,
    65536,   98304,   131072,  196608,  262144,  393216,  524288,  655360,
    786432,  917504,  1048576, 1179648, 1310720, 1441792, 1572864, 1703936,
    1835008, 1966080, 2097152};

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ensure(int bits) {
        while (count_ < bits) {
            uint32_t w = 0;
            if (pos_ + 1 < n_) {
                w = static_cast<uint32_t>(p_[pos_]) | (static_cast<uint32_t>(p_[pos_ + 1]) << 8);
                pos_ += 2;
            } else {
                pos_ = n_;  // pad with zero words past the end
            }
            buf_ = (buf_ << 16) | w;
            count_ += 16;
        }
        return true;
    }
    uint32_t read(int bits) {
        if (bits == 0) return 0;
        ensure(bits);
        uint32_t v = (buf_ >> (count_ - bits)) & ((1u << bits) - 1);
        count_ -= bits;
        return v;
    }
    uint32_t peek(int bits) {
        ensure(bits);
        return (buf_ >> (count_ - bits)) & ((1u << bits) - 1);
    }
    void drop(int bits) { count_ -= bits; }
    void alignToWord() {
        // discard bits up to the next 16-bit input-word boundary
        count_ -= count_ % 16;
    }
    // reads `bytes` raw bytes after a word alignment (for uncompressed blocks)
    void readBytes(uint8_t* dst, size_t bytes) {
        // after alignToWord, count_ is a multiple of 16; consume from buffer first
        while (bytes && count_ >= 8) {
            *dst++ = static_cast<uint8_t>((buf_ >> (count_ - 8)) & 0xFF);
            count_ -= 8;
            bytes--;
        }
        while (bytes && pos_ < n_) {
            *dst++ = p_[pos_++];
            bytes--;
        }
        while (bytes--) *dst++ = 0;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    uint64_t buf_ = 0;
    int count_ = 0;
};

// Canonical Huffman decoder matching the encoder's assignCodes (codes increase by
// length then symbol). Decodes bit-by-bit.
class HuffDecoder {
public:
    // RFC-canonical first-codes. (The encoder's assignCodes inflates first-codes by
    // the count[0] term, but put(len,code) masks to the low `len` bits and that term
    // is a multiple of 2^len, so the emitted codes are RFC-canonical.)
    void build(const uint8_t* lens, int n) {
        n_ = n;
        for (int i = 0; i <= 17; i++) count_[i] = 0;
        for (int i = 0; i < n; i++) count_[lens[i]]++;
        uint32_t code = 0, idx = 0;
        for (int l = 1; l <= 16; l++) {
            firstCode_[l] = code;
            firstIndex_[l] = idx;
            idx += count_[l];
            maxCodeExclusive_[l] = code + count_[l];
            code = (code + count_[l]) << 1;
        }
        // symbols sorted by (length, symbol)
        syms_.assign(idx, 0);
        std::array<uint32_t, 18> next{};
        for (int l = 1; l <= 16; l++) next[l] = firstIndex_[l];
        for (int s = 0; s < n; s++)
            if (lens[s]) syms_[next[lens[s]]++] = static_cast<uint16_t>(s);
        empty_ = (idx == 0);
    }
    bool empty() const { return empty_; }
    int decode(BitReader& br) const {
        uint32_t code = 0;
        for (int l = 1; l <= 16; l++) {
            code = (code << 1) | br.read(1);
            if (count_[l] && code >= firstCode_[l] && code < maxCodeExclusive_[l])
                return syms_[firstIndex_[l] + (code - firstCode_[l])];
        }
        return -1;  // corrupt
    }

private:
    int n_ = 0;
    bool empty_ = true;
    uint32_t count_[18] = {0};
    uint32_t firstCode_[18] = {0};
    uint32_t firstIndex_[18] = {0};
    uint32_t maxCodeExclusive_[18] = {0};
    std::vector<uint16_t> syms_;
};

// Reads one pretree-coded length array of `size` entries into `lens` (delta against
// the existing `lens` contents, which the caller zeroes at each reset interval).
bool readLengths(BitReader& br, uint8_t* lens, int size) {
    uint8_t preLen[PRETREE_SYMS];
    for (int i = 0; i < PRETREE_SYMS; i++) preLen[i] = static_cast<uint8_t>(br.read(4));
    HuffDecoder pre;
    pre.build(preLen, PRETREE_SYMS);

    int i = 0;
    while (i < size) {
        int sym = pre.decode(br);
        if (sym < 0) return false;
        if (sym == 17) {
            int run = br.read(4) + 4;
            while (run-- && i < size) lens[i++] = 0;
        } else if (sym == 18) {
            int run = br.read(5) + 20;
            while (run-- && i < size) lens[i++] = 0;
        } else if (sym == 19) {
            int run = br.read(1) + 4;
            int v = pre.decode(br);
            if (v < 0) return false;
            int newLen = (lens[i] - v + 17) % 17;
            while (run-- && i < size) lens[i++] = static_cast<uint8_t>(newLen);
        } else {
            lens[i] = static_cast<uint8_t>((lens[i] - sym + 17) % 17);
            i++;
        }
    }
    return true;
}

}  // namespace

bool lzxDecompress(const uint8_t* comp, size_t compSize, uint64_t uncompressedSize,
                   uint32_t resetIntervalBytes, uint32_t windowBits,
                   std::vector<uint8_t>& out, std::string& err) {
    static const int slotsForWindow[] = {30, 32, 34, 36, 38, 42, 50};
    if (windowBits < 15 || windowBits > 21) {
        err = "unsupported LZX window";
        return false;
    }
    const int numSlots = slotsForWindow[windowBits - 15];
    const int mainSyms = NUM_CHARS + 8 * numSlots;
    if (resetIntervalBytes == 0) resetIntervalBytes = 0x10000;

    out.clear();
    out.reserve(static_cast<size_t>(uncompressedSize));

    BitReader br(comp, compSize);
    int32_t R0 = 1, R1 = 1, R2 = 1;
    std::vector<uint8_t> mainLen(mainSyms, 0), lenLen(LEN_SYMS, 0),
        alignLen(ALIGNED_SYMS, 0);
    HuffDecoder mainTree, lenTree, alignTree;

    uint64_t produced = 0;
    uint64_t intervalRemaining = 0;  // output bytes left in the current LZX block
    uint64_t sinceFrame = 0;         // output bytes since the last 0x8000 realignment
    bool needReset = true;
    int blockType = 0;

    while (produced < uncompressedSize) {
        if (needReset) {
            // new reset interval: fresh state, fresh block
            R0 = R1 = R2 = 1;
            std::fill(mainLen.begin(), mainLen.end(), uint8_t{0});
            std::fill(lenLen.begin(), lenLen.end(), uint8_t{0});
            br.read(1);  // E8 translation flag (ignored; encoder writes 0)
            needReset = false;
            intervalRemaining = 0;
        }
        if (intervalRemaining == 0) {
            blockType = br.read(3);
            uint32_t blockLen = br.read(24);
            intervalRemaining = blockLen;
            if (blockType == 1 || blockType == 2) {  // verbatim / aligned
                if (blockType == 2)
                    for (int i = 0; i < ALIGNED_SYMS; i++)
                        alignLen[i] = static_cast<uint8_t>(br.read(3));
                if (!readLengths(br, mainLen.data(), NUM_CHARS) ||
                    !readLengths(br, mainLen.data() + NUM_CHARS, mainSyms - NUM_CHARS) ||
                    !readLengths(br, lenLen.data(), LEN_SYMS)) {
                    err = "corrupt LZX trees";
                    return false;
                }
                mainTree.build(mainLen.data(), mainSyms);
                lenTree.build(lenLen.data(), LEN_SYMS);
                if (blockType == 2) alignTree.build(alignLen.data(), ALIGNED_SYMS);
            } else if (blockType == 3) {  // uncompressed
                br.alignToWord();
                uint8_t r[12];
                br.readBytes(r, 12);
                R0 = r[0] | (r[1] << 8) | (r[2] << 16) | (r[3] << 24);
                R1 = r[4] | (r[5] << 8) | (r[6] << 16) | (r[7] << 24);
                R2 = r[8] | (r[9] << 8) | (r[10] << 16) | (r[11] << 24);
            } else {
                err = "bad LZX block type";
                return false;
            }
        }

        // decode one element (literal or match), honoring frame realignment
        if (blockType == 3) {
            size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(intervalRemaining, FRAME - sinceFrame));
            chunk = std::min<size_t>(chunk, static_cast<size_t>(uncompressedSize - produced));
            size_t base = out.size();
            out.resize(base + chunk);
            br.readBytes(out.data() + base, chunk);
            produced += chunk;
            intervalRemaining -= chunk;
            sinceFrame += chunk;
        } else {
            int sym = mainTree.decode(br);
            if (sym < 0) {
                err = "corrupt LZX stream";
                return false;
            }
            if (sym < NUM_CHARS) {
                out.push_back(static_cast<uint8_t>(sym));
                produced++;
                intervalRemaining--;
                sinceFrame++;
            } else {
                int slot = (sym - NUM_CHARS) >> 3;
                int lenHeader = (sym - NUM_CHARS) & 7;
                int matchLen = lenHeader + MIN_MATCH;
                if (lenHeader == NUM_PRIMARY_LENGTHS) {
                    int extra = lenTree.decode(br);
                    if (extra < 0) {
                        err = "corrupt LZX length";
                        return false;
                    }
                    matchLen = extra + NUM_PRIMARY_LENGTHS + MIN_MATCH;
                }
                int32_t matchOffset;
                if (slot == 0) {
                    matchOffset = R0;
                } else if (slot == 1) {
                    matchOffset = R1;
                    R1 = R0;
                    R0 = matchOffset;
                } else if (slot == 2) {
                    matchOffset = R2;
                    R2 = R0;
                    R0 = matchOffset;
                } else {
                    const int eb = kExtraBits[slot];
                    uint32_t footer;
                    if (blockType == 2 && eb >= 3) {
                        uint32_t verbatim = eb > 3 ? br.read(eb - 3) : 0;
                        int a = alignTree.decode(br);
                        if (a < 0) {
                            err = "corrupt LZX aligned";
                            return false;
                        }
                        footer = (verbatim << 3) | static_cast<uint32_t>(a);
                    } else {
                        footer = br.read(eb);
                    }
                    matchOffset = static_cast<int32_t>(kPosBase[slot] + footer) - 2;
                    R2 = R1;
                    R1 = R0;
                    R0 = matchOffset;
                }
                if (matchOffset <= 0 || static_cast<size_t>(matchOffset) > out.size()) {
                    err = "LZX match out of range";
                    return false;
                }
                // matches never cross a frame boundary, so a flat copy is safe
                size_t src = out.size() - matchOffset;
                for (int k = 0; k < matchLen; k++) out.push_back(out[src + k]);
                produced += matchLen;
                intervalRemaining -= matchLen;
                sinceFrame += matchLen;
            }
        }

        if (sinceFrame >= FRAME) {
            br.alignToWord();
            sinceFrame = 0;
            if (produced % resetIntervalBytes == 0) needReset = true;
        }
    }
    out.resize(static_cast<size_t>(uncompressedSize));  // drop frame padding / overshoot
    return true;
}

}  // namespace fastchm
