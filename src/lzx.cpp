// FastChm — LZX encoder. Format facts per refs/NOTES.md:
//  - bitstream of 16-bit little-endian words, bits placed MSB-first within each word
//  - per 64K reset interval: 1-bit E8 flag (0), then one block:
//    3-bit type, 24-bit uncompressed size, Huffman trees (pretree-delta coded), tokens
//  - 16-bit alignment + reset-table mark at every 0x8000 uncompressed boundary
//  - matches never cross a 0x8000 boundary and never reference data before the
//    last reset point
#include "lzx.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace fastchm {
namespace {

constexpr size_t   FRAME = 0x8000;
constexpr size_t   INTERVAL = 0x10000;
constexpr int      NUM_CHARS = 256;
constexpr int      NUM_SLOTS = 32;  // window 2^16
constexpr int      MAIN_SYMS = NUM_CHARS + NUM_SLOTS * 8;  // 512
constexpr int      LEN_SYMS = 249;
constexpr int      PRETREE_SYMS = 20;
constexpr int      ALIGNED_SYMS = 8;
constexpr int      MIN_MATCH = 2;
constexpr int      MAX_MATCH = 257;
constexpr uint32_t MAX_DIST = 0xFFFD;  // offsets wsize-2..wsize are illegal
constexpr int      HASH_BITS = 15;
constexpr int      MAX_CHAIN = 128;

constexpr int BLOCK_VERBATIM = 1;
constexpr int BLOCK_ALIGNED = 2;

struct SlotTables {
    uint8_t extra[NUM_SLOTS + 1];
    uint32_t base[NUM_SLOTS + 1];
    SlotTables() {
        uint32_t b = 0;
        for (int i = 0; i <= NUM_SLOTS; i++) {
            extra[i] = static_cast<uint8_t>(i < 2 ? 0 : std::min(17, (i - 2) >> 1));
            base[i] = b;
            b += 1u << extra[i];
        }
    }
};
const SlotTables ST;

int slotFor(uint32_t formatted) {
    int lo = 0, hi = NUM_SLOTS - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (ST.base[mid] <= formatted) lo = mid; else hi = mid - 1;
    }
    return lo;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(int nbits, uint32_t value) {
        acc_ = (acc_ << nbits) | (value & ((1ull << nbits) - 1));
        n_ += nbits;
        while (n_ >= 16) {
            uint16_t w = static_cast<uint16_t>(acc_ >> (n_ - 16));
            out_.push_back(static_cast<uint8_t>(w & 0xFF));
            out_.push_back(static_cast<uint8_t>(w >> 8));
            n_ -= 16;
        }
    }
    void align16() {
        if (n_ != 0) put(16 - n_, 0);
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    int n_ = 0;
};

// Computes Huffman code lengths bounded by maxLen. All-zero frequencies produce an
// empty tree; a single used symbol gets length 1 along with the lowest other symbol
// (the format requires at least two codes in a non-empty tree).
void huffLengths(const uint32_t* freqIn, int n, int maxLen, uint8_t* outLen) {
    std::fill(outLen, outLen + n, uint8_t{0});
    std::vector<uint32_t> freq(freqIn, freqIn + n);
    std::vector<int> syms;
    syms.reserve(n);
    for (int i = 0; i < n; i++)
        if (freq[i]) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1) {
        outLen[syms[0]] = 1;
        outLen[syms[0] == 0 ? 1 : 0] = 1;
        return;
    }
    const int m = static_cast<int>(syms.size());
    std::vector<uint64_t> f(2 * m - 1);
    std::vector<int> parent(2 * m - 1, -1);
    for (;;) {
        std::sort(syms.begin(), syms.end(), [&](int a, int b) {
            return freq[a] != freq[b] ? freq[a] < freq[b] : a < b;
        });
        for (int i = 0; i < m; i++) f[i] = freq[syms[i]];
        std::fill(parent.begin(), parent.end(), -1);
        // two-queue merge: leaves f[0..m) sorted; internal nodes f[m..) form a
        // nondecreasing queue as they are produced
        int leaf = 0, inode = m, next = m;
        auto pickMin = [&]() {
            if (leaf < m && (inode >= next || f[leaf] <= f[inode])) return leaf++;
            return inode++;
        };
        for (; next < 2 * m - 1; next++) {
            int a = pickMin(), b = pickMin();
            f[next] = f[a] + f[b];
            parent[a] = next;
            parent[b] = next;
        }
        int maxDepth = 0;
        for (int i = 0; i < m; i++) {
            int d = 0;
            for (int p = parent[i]; p >= 0; p = parent[p]) d++;
            maxDepth = std::max(maxDepth, d);
        }
        if (maxDepth <= maxLen) {
            for (int i = 0; i < m; i++) {
                int d = 0;
                for (int p = parent[i]; p >= 0; p = parent[p]) d++;
                outLen[syms[i]] = static_cast<uint8_t>(d);
            }
            return;
        }
        for (int s : syms) freq[s] = std::max(1u, freq[s] >> 1);
    }
}

// Canonical code assignment matching the LZX decoder's table construction:
// codes increase with (length asc, symbol asc).
void assignCodes(const uint8_t* len, int n, uint16_t* code) {
    uint32_t count[17] = {0};
    for (int i = 0; i < n; i++) count[len[i]]++;
    uint32_t next[17] = {0};
    uint32_t c = 0;
    for (int l = 1; l <= 16; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? static_cast<uint16_t>(next[len[i]]++) : 0;
}

// Pretree-encodes `n` code lengths (previous lengths are all zero: we emit exactly
// one block per reset interval).
void writeTree(BitWriter& bw, const uint8_t* lens, int n) {
    struct Item {
        uint8_t sym;        // pretree symbol 0..19
        uint8_t excessBits; // 0, 1, 4 or 5
        uint8_t excess;
    };
    std::vector<Item> items;
    items.reserve(n);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && lens[j] == lens[i]) j++;
        int run = j - i;
        const int v = lens[i];
        if (v == 0) {
            while (run >= 20) {
                int e = std::min(run - 20, 31);
                items.push_back({18, 5, static_cast<uint8_t>(e)});
                run -= 20 + e;
            }
            while (run >= 4) {
                int e = std::min(run - 4, 15);
                items.push_back({17, 4, static_cast<uint8_t>(e)});
                run -= 4 + e;
            }
            while (run-- > 0) items.push_back({0, 0, 0});  // delta (0-0)%17 = 0
        } else {
            const uint8_t d = static_cast<uint8_t>((17 - v) % 17);  // (prev0 - v) mod 17
            while (run >= 4) {
                int e = run >= 5 ? 1 : 0;
                items.push_back({19, 1, static_cast<uint8_t>(e)});
                items.push_back({d, 0, 0});
                run -= 4 + e;
            }
            while (run-- > 0) items.push_back({d, 0, 0});
        }
        i = j;
    }

    uint32_t freq[PRETREE_SYMS] = {0};
    for (const Item& it : items) freq[it.sym]++;
    uint8_t plen[PRETREE_SYMS];
    uint16_t pcode[PRETREE_SYMS];
    huffLengths(freq, PRETREE_SYMS, 15, plen);
    assignCodes(plen, PRETREE_SYMS, pcode);

    for (int s = 0; s < PRETREE_SYMS; s++) bw.put(4, plen[s]);
    for (const Item& it : items) {
        bw.put(plen[it.sym], pcode[it.sym]);
        if (it.excessBits) bw.put(it.excessBits, it.excess);
    }
}

// Token packing (literal: value < 256):
//   bit 31 set | slot << 25 | positionFooter << 8 | (len - MIN_MATCH)
inline uint32_t makeMatchToken(int slot, uint32_t footer, int len) {
    return 0x80000000u | (static_cast<uint32_t>(slot) << 25) | (footer << 8) |
           static_cast<uint32_t>(len - MIN_MATCH);
}

struct IntervalCoder {
    uint32_t mainFreq[MAIN_SYMS] = {0};
    uint32_t lenFreq[LEN_SYMS] = {0};
    uint32_t alignedFreq[ALIGNED_SYMS] = {0};
    std::vector<uint32_t> tokens;

    void addLiteral(uint8_t c) {
        tokens.push_back(c);
        mainFreq[c]++;
    }
    void addMatch(int slot, uint32_t footer, int len) {
        tokens.push_back(makeMatchToken(slot, footer, len));
        const int lenHeader = std::min(len - MIN_MATCH, 7);
        mainFreq[NUM_CHARS + (slot << 3 | lenHeader)]++;
        if (lenHeader == 7) lenFreq[len - MIN_MATCH - 7]++;
        if (ST.extra[slot] >= 3) alignedFreq[footer & 7]++;
    }
};

size_t matchLen(const uint8_t* a, const uint8_t* b, size_t maxLen) {
    size_t i = 0;
    while (i < maxLen && a[i] == b[i]) i++;
    return i;
}

// Greedy LZ parse of one reset interval [start, end). `data` is the padded buffer;
// hash structures are caller-provided scratch reused across intervals.
void parseInterval(const uint8_t* data, size_t start, size_t end, IntervalCoder& coder,
                   std::vector<int32_t>& head, std::vector<int32_t>& prev) {
    std::fill(head.begin(), head.end(), -1);
    uint32_t R0 = 1, R1 = 1, R2 = 1;

    auto hashAt = [&](size_t p) {
        uint32_t h = (static_cast<uint32_t>(data[p]) << 16) |
                     (static_cast<uint32_t>(data[p + 1]) << 8) | data[p + 2];
        return (h * 2654435761u) >> (32 - HASH_BITS);
    };
    auto insert = [&](size_t p) {
        if (p + 2 < end) {
            uint32_t h = hashAt(p);
            prev[p - start] = head[h];
            head[h] = static_cast<int32_t>(p);
        }
    };

    size_t pos = start;
    while (pos < end) {
        const size_t frameEnd = (pos / FRAME + 1) * FRAME;
        const size_t maxLen =
            std::min({static_cast<size_t>(MAX_MATCH), end - pos, frameEnd - pos});

        size_t bestLen = 0;
        uint32_t bestDist = 0;
        int repSlot = -1;

        if (maxLen >= static_cast<size_t>(MIN_MATCH)) {
            // repeated offsets first; R0 may pay off from length 2, R1/R2 from 3
            const uint32_t reps[3] = {R0, R1, R2};
            for (int r = 0; r < 3; r++) {
                if (reps[r] > pos - start) continue;  // source would precede the reset
                size_t len = matchLen(data + pos - reps[r], data + pos, maxLen);
                if (len >= static_cast<size_t>(r == 0 ? 2 : 3) && len > bestLen) {
                    bestLen = len;
                    bestDist = reps[r];
                    repSlot = r;
                }
            }
            if (pos + 2 < end) {
                int chain = MAX_CHAIN;
                for (int32_t cand = head[hashAt(pos)]; cand >= 0 && chain-- > 0;
                     cand = prev[cand - start]) {
                    const uint32_t dist = static_cast<uint32_t>(pos - cand);
                    if (dist > MAX_DIST) break;
                    if (bestLen > 0 && (bestLen >= maxLen ||
                                        data[cand + bestLen] != data[pos + bestLen]))
                        continue;
                    size_t len = matchLen(data + cand, data + pos, maxLen);
                    if (len <= bestLen || len < 3) continue;
                    // skip matches whose footer bits likely cost more than literals
                    const uint32_t fmt = dist + 2;
                    if ((fmt >= 64 && len < 4) || (fmt >= 2048 && len < 5)) continue;
                    bestLen = len;
                    bestDist = dist;
                    repSlot = -1;
                }
            }
        }

        if (bestLen >= 2) {
            if (repSlot == 0) {
                coder.addMatch(0, 0, static_cast<int>(bestLen));
            } else if (repSlot == 1) {
                std::swap(R0, R1);
                coder.addMatch(1, 0, static_cast<int>(bestLen));
            } else if (repSlot == 2) {
                std::swap(R0, R2);
                coder.addMatch(2, 0, static_cast<int>(bestLen));
            } else {
                R2 = R1;
                R1 = R0;
                R0 = bestDist;
                const uint32_t fmt = bestDist + 2;
                const int slot = slotFor(fmt);
                coder.addMatch(slot, fmt - ST.base[slot], static_cast<int>(bestLen));
            }
            for (size_t p = pos; p < pos + bestLen; p++) insert(p);
            pos += bestLen;
        } else {
            coder.addLiteral(data[pos]);
            insert(pos);
            pos++;
        }
    }
}

}  // namespace

LzxResult lzxCompress(const uint8_t* input, size_t size) {
    LzxResult res;
    if (size == 0) return res;

    const size_t padded = (size + FRAME - 1) / FRAME * FRAME;
    res.paddedSize = padded;
    std::vector<uint8_t> data(padded, 0);
    std::memcpy(data.data(), input, size);

    BitWriter bw(res.data);
    std::vector<int32_t> head(static_cast<size_t>(1) << HASH_BITS);
    std::vector<int32_t> prev(INTERVAL);

    res.frameStarts.push_back(0);

    for (size_t start = 0; start < padded; start += INTERVAL) {
        const size_t end = std::min(padded, start + INTERVAL);
        IntervalCoder coder;
        parseInterval(data.data(), start, end, coder, head, prev);

        // trees
        uint8_t mainLen[MAIN_SYMS], lenLen[LEN_SYMS], alignedLen[ALIGNED_SYMS];
        uint16_t mainCode[MAIN_SYMS], lenCode[LEN_SYMS], alignedCode[ALIGNED_SYMS];
        huffLengths(coder.mainFreq, MAIN_SYMS, 16, mainLen);
        huffLengths(coder.lenFreq, LEN_SYMS, 16, lenLen);
        huffLengths(coder.alignedFreq, ALIGNED_SYMS, 7, alignedLen);
        assignCodes(mainLen, MAIN_SYMS, mainCode);
        assignCodes(lenLen, LEN_SYMS, lenCode);
        assignCodes(alignedLen, ALIGNED_SYMS, alignedCode);

        // verbatim vs aligned: compare raw 3-bit footers against Huffman + 24-bit tree
        uint64_t rawBits = 0, alignedBits = 24;
        for (int s = 0; s < ALIGNED_SYMS; s++) {
            rawBits += 3ull * coder.alignedFreq[s];
            alignedBits += static_cast<uint64_t>(coder.alignedFreq[s]) * alignedLen[s];
        }
        const int blockType = alignedBits < rawBits ? BLOCK_ALIGNED : BLOCK_VERBATIM;

        bw.put(1, 0);  // E8 translation off (written once per reset)
        bw.put(3, static_cast<uint32_t>(blockType));
        bw.put(24, static_cast<uint32_t>(end - start));
        if (blockType == BLOCK_ALIGNED)
            for (int s = 0; s < ALIGNED_SYMS; s++) bw.put(3, alignedLen[s]);
        writeTree(bw, mainLen, NUM_CHARS);
        writeTree(bw, mainLen + NUM_CHARS, MAIN_SYMS - NUM_CHARS);
        writeTree(bw, lenLen, LEN_SYMS);

        size_t inFrame = (start % FRAME);  // always 0: intervals are frame-aligned
        for (uint32_t tok : coder.tokens) {
            size_t tokBytes;
            if (tok & 0x80000000u) {
                const int lenM2 = static_cast<int>(tok & 0xFF);
                const uint32_t footer = (tok >> 8) & 0x1FFFF;
                const int slot = static_cast<int>((tok >> 25) & 0x3F);
                const int lenHeader = std::min(lenM2, 7);
                const int mainSym = NUM_CHARS + (slot << 3 | lenHeader);
                bw.put(mainLen[mainSym], mainCode[mainSym]);
                if (lenHeader == 7) {
                    const int ls = lenM2 - 7;
                    bw.put(lenLen[ls], lenCode[ls]);
                }
                const int eb = ST.extra[slot];
                if (blockType == BLOCK_ALIGNED && eb >= 3) {
                    bw.put(eb - 3, footer >> 3);
                    bw.put(alignedLen[footer & 7], alignedCode[footer & 7]);
                } else if (eb > 0) {
                    bw.put(eb, footer);
                }
                tokBytes = static_cast<size_t>(lenM2) + MIN_MATCH;
            } else {
                bw.put(mainLen[tok], mainCode[tok]);
                tokBytes = 1;
            }
            inFrame += tokBytes;
            assert(inFrame <= FRAME);
            if (inFrame == FRAME) {
                bw.align16();
                res.frameStarts.push_back(res.data.size());
                inFrame = 0;
            }
        }
        assert(inFrame == 0);
    }

    // the final mark points past the last frame; the reset table only lists starts
    res.frameStarts.pop_back();
    assert(res.frameStarts.size() == padded / FRAME);
    return res;
}

}  // namespace fastchm
