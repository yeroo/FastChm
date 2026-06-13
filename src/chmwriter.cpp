#include "chmwriter.h"

#include <algorithm>
#include <fstream>

#include "bytebuf.h"

namespace fastchm {
namespace {

constexpr size_t CHUNK = 0x1000;
constexpr size_t PMGL_HDR = 0x14;
constexpr size_t PMGI_HDR = 0x08;
constexpr int QR_DENSITY_STRIDE = 5;  // density 2 -> quickref every 1+(1<<2) entries

inline char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool nameLess(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const unsigned char ca = static_cast<unsigned char>(lower(a[i]));
        const unsigned char cb = static_cast<unsigned char>(lower(b[i]));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

// One directory chunk under construction. Quickref area (built backward from the
// chunk end): WORD entry count at end-2, then a WORD offset-from-entry-0 for every
// QR_DENSITY_STRIDE-th entry.
struct ChunkBuilder {
    size_t headerSize;
    std::vector<uint8_t> body;
    std::vector<uint16_t> entryOffsets;
    std::string firstName;

    explicit ChunkBuilder(size_t hdr) : headerSize(hdr) {}

    static size_t quickrefBytes(size_t entryCount) {
        return entryCount == 0 ? 2 : 2 + 2 * ((entryCount - 1) / QR_DENSITY_STRIDE);
    }
    bool canAdd(size_t entrySize) const {
        return headerSize + body.size() + entrySize + quickrefBytes(entryOffsets.size() + 1) <=
               CHUNK;
    }
    void add(const std::string& name, const std::vector<uint8_t>& entry) {
        if (entryOffsets.empty()) firstName = name;
        entryOffsets.push_back(static_cast<uint16_t>(body.size()));
        body.insert(body.end(), entry.begin(), entry.end());
    }
    // Fills everything after the chunk header into `chunk` (size CHUNK).
    void finalize(uint8_t* chunk) const {
        std::copy(body.begin(), body.end(), chunk + headerSize);
        const size_t count = entryOffsets.size();
        chunk[CHUNK - 2] = static_cast<uint8_t>(count);
        chunk[CHUNK - 1] = static_cast<uint8_t>(count >> 8);
        for (size_t j = 1; j * QR_DENSITY_STRIDE < count; j++) {
            const uint16_t off = entryOffsets[j * QR_DENSITY_STRIDE];
            chunk[CHUNK - 2 - 2 * j] = static_cast<uint8_t>(off);
            chunk[CHUNK - 1 - 2 * j] = static_cast<uint8_t>(off >> 8);
        }
    }
    uint32_t freeSpace() const {
        return static_cast<uint32_t>(CHUNK - headerSize - body.size());
    }
};

}  // namespace

bool writeContainer(const std::string& path, uint32_t langId,
                    std::vector<DirEntry> entries,
                    const std::vector<uint8_t>& section0,
                    const std::vector<uint8_t>& compressed, std::string& err) {
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return nameLess(a.name, b.name); });

    // ---- PMGL listing chunks ----
    std::vector<ChunkBuilder> listChunks;
    ChunkBuilder cur(PMGL_HDR);
    for (const DirEntry& e : entries) {
        std::vector<uint8_t> enc;
        encint(enc, e.name.size());
        enc.insert(enc.end(), e.name.begin(), e.name.end());
        encint(enc, static_cast<uint64_t>(e.section));
        encint(enc, e.offset);
        encint(enc, e.size);
        if (!cur.canAdd(enc.size())) {
            listChunks.push_back(std::move(cur));
            cur = ChunkBuilder(PMGL_HDR);
        }
        cur.add(e.name, enc);
    }
    if (!cur.entryOffsets.empty()) listChunks.push_back(std::move(cur));
    const int numPMGL = static_cast<int>(listChunks.size());

    // ---- PMGI index levels (only when more than one listing chunk) ----
    struct Ref {
        std::string name;
        int chunk;
    };
    std::vector<Ref> level;
    for (int i = 0; i < numPMGL; i++) level.push_back({listChunks[i].firstName, i});
    std::vector<ChunkBuilder> indexChunks;
    int nextChunkIdx = numPMGL;
    int depth = 1;
    int rootChunk = -1;
    while (level.size() > 1) {
        depth++;
        std::vector<Ref> next;
        ChunkBuilder ic(PMGI_HDR);
        auto flush = [&]() {
            next.push_back({ic.firstName, nextChunkIdx++});
            indexChunks.push_back(std::move(ic));
            ic = ChunkBuilder(PMGI_HDR);
        };
        for (const Ref& r : level) {
            std::vector<uint8_t> enc;
            encint(enc, r.name.size());
            enc.insert(enc.end(), r.name.begin(), r.name.end());
            encint(enc, static_cast<uint64_t>(r.chunk));
            if (!ic.canAdd(enc.size())) flush();
            ic.add(r.name, enc);
        }
        if (!ic.entryOffsets.empty()) flush();
        level = std::move(next);
        rootChunk = level.back().chunk;
    }
    const int totalChunks = nextChunkIdx;

    // ---- assemble directory chunk bytes ----
    std::vector<uint8_t> chunks(static_cast<size_t>(totalChunks) * CHUNK, 0);
    for (int i = 0; i < numPMGL; i++) {
        uint8_t* p = chunks.data() + static_cast<size_t>(i) * CHUNK;
        Buf h;
        h.raw("PMGL", 4);
        h.u32(listChunks[i].freeSpace());
        h.u32(0);
        h.i32(i == 0 ? -1 : i - 1);
        h.i32(i == numPMGL - 1 ? -1 : i + 1);
        std::copy(h.v.begin(), h.v.end(), p);
        listChunks[i].finalize(p);
    }
    for (size_t i = 0; i < indexChunks.size(); i++) {
        uint8_t* p = chunks.data() + (numPMGL + i) * CHUNK;
        Buf h;
        h.raw("PMGI", 4);
        h.u32(indexChunks[i].freeSpace());
        std::copy(h.v.begin(), h.v.end(), p);
        indexChunks[i].finalize(p);
    }

    // ---- headers ----
    const uint64_t hs1Len = 0x54 + chunks.size();
    const uint64_t contentOffset = 0x78 + hs1Len;
    const uint64_t fileSize = contentOffset + section0.size() + compressed.size();

    static const uint8_t g1d[8] = {0x9E, 0x0C, 0x00, 0xA0, 0xC9, 0x22, 0xE6, 0xEC};
    static const uint8_t g3d[8] = {0x9D, 0xF9, 0x00, 0xA0, 0xC9, 0x22, 0xE6, 0xEC};

    Buf out;
    // ITSF header
    out.raw("ITSF", 4);
    out.u32(3);     // version
    out.u32(0x60);  // header length
    out.u32(1);
    out.u32(0);  // timestamp (don't-care; zero for deterministic output)
    out.u32(langId);
    out.guid(0x7C01FD10, 0x7BAA, 0x11D0, g1d);
    out.guid(0x7C01FD11, 0x7BAA, 0x11D0, g1d);
    out.u64(0x60);  // header section 0 offset
    out.u64(0x18);  // header section 0 length
    out.u64(0x78);  // header section 1 offset
    out.u64(hs1Len);
    out.u64(contentOffset);
    // header section 0
    out.u32(0x01FE);
    out.u32(0);
    out.u64(fileSize);
    out.u32(0);
    out.u32(0);
    // header section 1: ITSP directory header
    out.raw("ITSP", 4);
    out.u32(1);     // version
    out.u32(0x54);  // header length
    out.u32(0x0A);
    out.u32(0x1000);  // chunk size
    out.u32(2);       // quickref density
    out.u32(static_cast<uint32_t>(depth));
    out.i32(rootChunk);
    out.u32(0);  // first PMGL
    out.u32(static_cast<uint32_t>(numPMGL - 1));  // last PMGL
    out.i32(-1);
    out.u32(static_cast<uint32_t>(totalChunks));
    out.u32(langId);
    out.guid(0x5D02926A, 0x212E, 0x11D0, g3d);
    out.u32(0x54);
    out.i32(-1);
    out.i32(-1);
    out.i32(-1);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open output file: " + path;
        return false;
    }
    auto put = [&](const std::vector<uint8_t>& b) {
        if (!b.empty()) f.write(reinterpret_cast<const char*>(b.data()),
                                static_cast<std::streamsize>(b.size()));
    };
    put(out.v);
    put(chunks);
    put(section0);
    put(compressed);
    f.close();
    if (!f) {
        err = "error writing " + path;
        return false;
    }
    return true;
}

}  // namespace fastchm
