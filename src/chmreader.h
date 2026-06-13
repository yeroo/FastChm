// FastChm — CHM (ITSF) reader: directory listing and file extraction.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fastchm {

struct ChmEntry {
    std::string name;
    int section = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

// Opens a CHM, parses its directory and (lazily) decompresses the MSCompressed
// section so any file can be read. All-or-nothing: returns false + `err` on a
// malformed file.
class ChmFile {
public:
    bool open(const std::string& path, std::string& err);
    const std::vector<ChmEntry>& entries() const { return entries_; }
    // Reads the named entry's bytes; false if absent or unreadable.
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string& err);

private:
    bool ensureSection1(std::string& err);
    std::vector<uint8_t> file_;       // whole CHM
    std::vector<ChmEntry> entries_;
    uint64_t contentOffset_ = 0;      // file offset of content section 0
    std::vector<uint8_t> section1_;   // decompressed MSCompressed section
    bool section1Ready_ = false;
};

// CLI helpers: list entries / extract every user file ("/..." names) to `outDir`.
int chmList(const std::string& path);
int chmExtract(const std::string& path, const std::string& outDir);

}  // namespace fastchm
