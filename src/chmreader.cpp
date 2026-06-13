#include "chmreader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "lzxdecode.h"

namespace fastchm {
namespace {

uint32_t rd32(const std::vector<uint8_t>& v, size_t p) {
    return v[p] | (v[p + 1] << 8) | (v[p + 2] << 16) | (uint32_t(v[p + 3]) << 24);
}
uint64_t rd64(const std::vector<uint8_t>& v, size_t p) {
    return rd32(v, p) | (uint64_t(rd32(v, p + 4)) << 32);
}

// ITSS variable-length integer (big-endian groups, high bit = continue).
uint64_t encint(const std::vector<uint8_t>& v, size_t& p) {
    uint64_t x = 0;
    for (;;) {
        uint8_t b = v[p++];
        x = (x << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return x;
}

int windowBitsFromBytes(uint32_t bytes) {
    int b = 0;
    while ((1u << b) < bytes) b++;
    return b;
}

}  // namespace

bool ChmFile::open(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "cannot open: " + path;
        return false;
    }
    const std::streamsize n = f.tellg();
    f.seekg(0);
    file_.resize(static_cast<size_t>(n));
    if (n && !f.read(reinterpret_cast<char*>(file_.data()), n)) {
        err = "cannot read: " + path;
        return false;
    }
    if (file_.size() < 0x60 || std::memcmp(file_.data(), "ITSF", 4) != 0) {
        err = "not an ITSF (CHM) file";
        return false;
    }
    const uint64_t hs1 = rd64(file_, 0x48);
    contentOffset_ = rd64(file_, 0x58);
    if (hs1 + 0x54 > file_.size() || std::memcmp(&file_[hs1], "ITSP", 4) != 0) {
        err = "bad ITSP directory";
        return false;
    }
    const uint32_t chunkSize = rd32(file_, hs1 + 0x10);
    const uint32_t nChunks = rd32(file_, hs1 + 0x2C);
    const size_t dirBase = static_cast<size_t>(hs1) + 0x54;
    for (uint32_t c = 0; c < nChunks; c++) {
        const size_t base = dirBase + static_cast<size_t>(c) * chunkSize;
        if (base + chunkSize > file_.size()) break;
        if (std::memcmp(&file_[base], "PMGL", 4) != 0) continue;
        const uint32_t freeSpace = rd32(file_, base + 4);
        size_t p = base + 0x14;
        const size_t end = base + chunkSize - freeSpace;
        while (p < end) {
            const uint64_t nlen = encint(file_, p);
            ChmEntry e;
            e.name.assign(reinterpret_cast<const char*>(&file_[p]),
                          static_cast<size_t>(nlen));
            p += static_cast<size_t>(nlen);
            e.section = static_cast<int>(encint(file_, p));
            e.offset = encint(file_, p);
            e.length = encint(file_, p);
            entries_.push_back(std::move(e));
        }
    }
    return true;
}

bool ChmFile::ensureSection1(std::string& err) {
    if (section1Ready_) return true;
    auto find = [&](const char* name) -> const ChmEntry* {
        for (const ChmEntry& e : entries_)
            if (e.name == name) return &e;
        return nullptr;
    };
    const ChmEntry* content =
        find("::DataSpace/Storage/MSCompressed/Content");
    const ChmEntry* control =
        find("::DataSpace/Storage/MSCompressed/ControlData");
    const ChmEntry* span = find("::DataSpace/Storage/MSCompressed/SpanInfo");
    if (!content || !control || !span) {
        err = "missing MSCompressed metadata";
        return false;
    }
    // section-0 files sit at contentOffset + entry.offset
    auto sec0 = [&](const ChmEntry* e) {
        return contentOffset_ + e->offset;
    };
    const size_t ctl = static_cast<size_t>(sec0(control));
    if (std::memcmp(&file_[ctl + 4], "LZXC", 4) != 0) {
        err = "section is not LZX-compressed";
        return false;
    }
    const uint32_t version = rd32(file_, ctl + 8);
    uint32_t resetIv = rd32(file_, ctl + 12);
    uint32_t window = rd32(file_, ctl + 16);
    if (version == 2) {
        resetIv *= 0x8000;
        window *= 0x8000;
    }
    const uint64_t uncompressed = rd64(file_, static_cast<size_t>(sec0(span)));
    const size_t cstart = static_cast<size_t>(sec0(content));
    return lzxDecompress(&file_[cstart], static_cast<size_t>(content->length),
                         uncompressed, resetIv, windowBitsFromBytes(window),
                         section1_, err)
               ? (section1Ready_ = true)
               : false;
}

bool ChmFile::read(const std::string& name, std::vector<uint8_t>& out,
                   std::string& err) {
    const ChmEntry* e = nullptr;
    for (const ChmEntry& x : entries_)
        if (x.name == name) {
            e = &x;
            break;
        }
    if (!e) {
        err = "no such entry: " + name;
        return false;
    }
    if (e->section == 0) {
        const size_t start = static_cast<size_t>(contentOffset_ + e->offset);
        if (start + e->length > file_.size()) {
            err = "entry out of range: " + name;
            return false;
        }
        out.assign(file_.begin() + start, file_.begin() + start + e->length);
        return true;
    }
    if (!ensureSection1(err)) return false;
    if (e->offset + e->length > section1_.size()) {
        err = "entry out of range in section: " + name;
        return false;
    }
    out.assign(section1_.begin() + e->offset,
               section1_.begin() + e->offset + e->length);
    return true;
}

int chmList(const std::string& path) {
    ChmFile chm;
    std::string err;
    if (!chm.open(path, err)) {
        fprintf(stderr, "fastchm: %s\n", err.c_str());
        return 1;
    }
    for (const ChmEntry& e : chm.entries())
        printf("%-8s %10llu  %s\n", e.section ? "lzx" : "raw",
               static_cast<unsigned long long>(e.length), e.name.c_str());
    return 0;
}

int chmExtract(const std::string& path, const std::string& outDir) {
    ChmFile chm;
    std::string err;
    if (!chm.open(path, err)) {
        fprintf(stderr, "fastchm: %s\n", err.c_str());
        return 1;
    }
    int count = 0, failures = 0;
    for (const ChmEntry& e : chm.entries()) {
        if (e.name.empty() || e.name[0] != '/' || e.length == 0) continue;  // user files
        std::vector<uint8_t> data;
        if (!chm.read(e.name, data, err)) {
            fprintf(stderr, "fastchm: %s\n", err.c_str());
            failures++;
            continue;
        }
        std::filesystem::path dst =
            std::filesystem::path(outDir) / e.name.substr(1);  // strip leading '/'
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream of(dst, std::ios::binary);
        if (!of) {
            fprintf(stderr, "fastchm: cannot write %s\n", dst.string().c_str());
            failures++;
            continue;
        }
        if (!data.empty())
            of.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        count++;
    }
    printf("extracted %d files to %s\n", count, outDir.c_str());
    return failures ? 1 : 0;
}

}  // namespace fastchm
