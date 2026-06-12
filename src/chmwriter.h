// FastChm — ITSF/ITSP container writer. Layouts per refs/NOTES.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fastchm {

struct DirEntry {
    std::string name;  // full archive path, e.g. "/index.htm" or "::DataSpace/NameList"
    int section = 0;   // 0 = Uncompressed, 1 = MSCompressed
    uint64_t offset = 0;  // within the (decompressed) section
    uint64_t size = 0;
};

// Writes a complete .chm: ITSF header, directory chunks, section-0 data, then the
// raw LZX bytes (whose directory entry must be the ::DataSpace/.../Content entry).
// Returns false and sets `err` on I/O failure.
bool writeContainer(const std::string& path, uint32_t langId,
                    std::vector<DirEntry> entries,
                    const std::vector<uint8_t>& section0,
                    const std::vector<uint8_t>& compressed, std::string& err);

}  // namespace fastchm
