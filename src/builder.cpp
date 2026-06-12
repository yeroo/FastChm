#include "builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bytebuf.h"
#include "chmwriter.h"
#include "lzx.h"

namespace fastchm {
namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lowerCopy(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(n));
    return n == 0 || !!f.read(reinterpret_cast<char*>(out.data()), n);
}

// ---------------- HHP project ----------------

struct Project {
    std::string dir;  // directory of the .hhp, with trailing separator
    std::unordered_map<std::string, std::string> options;  // lower-cased keys
    std::vector<std::string> files;                        // [FILES], deduped, ordered
    std::string opt(const std::string& key) const {
        auto it = options.find(key);
        return it == options.end() ? "" : it->second;
    }
};

bool parseHhp(const std::string& path, Project& p, std::string& err) {
    std::vector<uint8_t> raw;
    if (!readFile(path, raw)) {
        err = "cannot read project file: " + path;
        return false;
    }
    const size_t slash = slashes(path).find_last_of('/');
    p.dir = slash == std::string::npos ? "" : slashes(path).substr(0, slash + 1);

    std::string text(raw.begin(), raw.end());
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    std::string section;
    std::unordered_set<std::string> seen;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lowerCopy(line.substr(1, line.size() - 2));
            continue;
        }
        if (section == "options") {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                p.options[lowerCopy(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        } else if (section == "files") {
            const std::string f = slashes(line);
            if (seen.insert(lowerCopy(f)).second) p.files.push_back(f);
        }
        // [WINDOWS], [ALIAS], [MAP], [MERGE FILES], full-text search options:
        // not yet supported; ignored.
    }
    return true;
}

uint32_t parseLcid(const std::string& language) {
    // e.g. "0x409 English (United States)"
    unsigned long v = strtoul(language.c_str(), nullptr, 0);
    return v ? static_cast<uint32_t>(v) : 0x409;
}

// ---------------- HTML <title> extraction ----------------

std::string extractTitle(const std::vector<uint8_t>& html) {
    const std::string text(html.begin(), html.end());
    const std::string lower = lowerCopy(text);
    size_t t = lower.find("<title");
    if (t == std::string::npos) return "";
    t = lower.find('>', t);
    if (t == std::string::npos) return "";
    const size_t end = lower.find("</title", ++t);
    if (end == std::string::npos) return "";
    std::string title = text.substr(t, end - t);
    // collapse whitespace runs to single spaces
    std::string outs;
    bool ws = false;
    for (char c : title) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            ws = !outs.empty();
        } else {
            if (ws) outs.push_back(' ');
            ws = false;
            outs.push_back(c);
        }
    }
    return outs;
}

// ---------------- #STRINGS / #URLSTR+#URLTBL / #TOPICS ----------------

struct StringsTable {
    Buf buf;
    std::unordered_map<std::string, uint32_t> map;
    uint32_t add(const std::string& s) {
        if (buf.size() == 0) buf.u8(0);
        auto it = map.find(s);
        if (it != map.end()) return it->second;
        size_t pos = buf.size();
        // entries must not cross 0x1000 block boundaries
        const size_t nextBlock = (pos & ~static_cast<size_t>(0xFFF)) + 0x1000;
        if (pos + s.size() + 1 > nextBlock && s.size() + 1 <= 0x1000) {
            buf.zeros(nextBlock - pos);
            pos = nextBlock;
        }
        buf.strz(s);
        map[s] = static_cast<uint32_t>(pos);
        return static_cast<uint32_t>(pos);
    }
};

struct UrlTables {
    Buf urlstr, urltbl;
    std::unordered_map<std::string, uint32_t> strmap;

    uint32_t addUrlStr(const std::string& url) {
        auto it = strmap.find(url);
        if (it != strmap.end()) return it->second;
        const size_t entryLen = 9 + url.size();  // 2 DWORDs + string + NUL
        const size_t rem = 0x4000 - urlstr.size() % 0x4000;
        if (rem < entryLen) urlstr.zeros(rem);
        if (urlstr.size() % 0x4000 == 0) urlstr.u8(0);  // block lead byte
        const uint32_t pos = static_cast<uint32_t>(urlstr.size());
        urlstr.u32(0);
        urlstr.u32(0);
        urlstr.strz(url);
        strmap[url] = pos;
        return pos;
    }
    uint32_t addUrl(const std::string& url, uint32_t topicIndex) {
        const uint32_t us = addUrlStr(url);
        if ((urltbl.size() & 0xFFF) == 0xFFC) urltbl.u32(0);  // block pad DWORD
        const uint32_t pos = static_cast<uint32_t>(urltbl.size());
        urltbl.u32(0);
        urltbl.u32(topicIndex);
        urltbl.u32(us);
        return pos;
    }
};

struct TopicsTable {
    Buf buf;
    StringsTable& strings;
    UrlTables& urls;
    TopicsTable(StringsTable& s, UrlTables& u) : strings(s), urls(u) {}

    // code -1: derive (6 = has title, 2 = none, 0 = anchor URL)
    void add(const std::string& title, std::string url, int code) {
        if (!url.empty() && url[0] == '/') url.erase(0, 1);
        const uint32_t topicIndex = static_cast<uint32_t>(buf.size() / 16);
        const uint32_t strOff = title.empty() ? 0xFFFFFFFFu : strings.add(title);
        const uint32_t tblOff = urls.addUrl(url, topicIndex);
        uint16_t inContents;
        if (code >= 0)
            inContents = static_cast<uint16_t>(code);
        else if (url.find('#') != std::string::npos)
            inContents = 0;
        else
            inContents = title.empty() ? 2 : 6;
        buf.u32(0);  // #TOCIDX offset (no binary TOC)
        buf.u32(strOff);
        buf.u32(tblOff);
        buf.u16(inContents);
        buf.u16(0);
    }
};

// ---------------- #SYSTEM ----------------

void sysEntryStr(Buf& b, uint16_t code, const std::string& value) {
    b.u16(code);
    b.u16(static_cast<uint16_t>(value.size() + 1));
    b.strz(value);
}

std::vector<uint8_t> buildSystem(const Project& p, uint32_t lcid, const std::string& hhcName,
                                 const std::string& hhkName) {
    Buf b;
    b.u32(3);  // version (compatibility 1.1+)
    // 10: time_t timestamp
    b.u16(10);
    b.u16(4);
    b.u32(static_cast<uint32_t>(time(nullptr)));
    // 9: compiler id
    sysEntryStr(b, 9, "FastChm 0.1");
    // 4: 36-byte info struct
    b.u16(4);
    b.u16(36);
    b.u32(lcid);
    b.u32(0);  // DBCS
    b.u32(0);  // full-text search
    b.u32(0);  // KLinks
    b.u32(0);  // ALinks
    b.u64(0);  // FILETIME
    b.u32(0);
    b.u32(0);
    const std::string defTopic = slashes(p.opt("default topic"));
    if (!defTopic.empty()) sysEntryStr(b, 2, defTopic);
    if (!p.opt("title").empty()) sysEntryStr(b, 3, p.opt("title"));
    if (!p.opt("default font").empty()) sysEntryStr(b, 16, p.opt("default font"));
    if (!hhcName.empty()) sysEntryStr(b, 0, hhcName);
    if (!hhkName.empty()) sysEntryStr(b, 1, hhkName);
    if (!p.opt("default window").empty()) sysEntryStr(b, 5, p.opt("default window"));
    return std::move(b.v);
}

// ---------------- ::DataSpace control files ----------------

void utf16NameListEntry(Buf& b, const char* name) {
    const size_t n = strlen(name);
    b.u16(static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; i++) b.u16(static_cast<uint16_t>(name[i]));
    b.u16(0);
}

std::vector<uint8_t> buildNameList() {
    Buf b;
    b.u16(0);  // total length in words, patched below
    b.u16(2);
    utf16NameListEntry(b, "Uncompressed");
    utf16NameListEntry(b, "MSCompressed");
    const uint16_t words = static_cast<uint16_t>(b.size() / 2);
    b.v[0] = static_cast<uint8_t>(words);
    b.v[1] = static_cast<uint8_t>(words >> 8);
    return std::move(b.v);
}

std::vector<uint8_t> buildControlData() {
    Buf b;
    b.u32(6);  // DWORDs following 'LZXC'
    b.raw("LZXC", 4);
    b.u32(2);  // version
    b.u32(2);  // reset interval, in 0x8000 units
    b.u32(2);  // window size, in 0x8000 units
    b.u32(1);  // cache size
    b.u32(0);
    b.u32(0);
    return std::move(b.v);
}

std::vector<uint8_t> buildResetTable(uint64_t uncompressed, uint64_t compressed,
                                     const std::vector<uint64_t>& frameStarts) {
    Buf b;
    b.u32(2);
    b.u32(static_cast<uint32_t>(frameStarts.size()));
    b.u32(8);     // entry size
    b.u32(0x28);  // header size
    b.u64(uncompressed);
    b.u64(compressed);
    b.u64(0x8000);
    for (uint64_t off : frameStarts) b.u64(off);
    return std::move(b.v);
}

std::vector<uint8_t> buildTransformList() {
    // MS bug replicated: 38 bytes = first 19 chars of the LZX transform GUID string
    // stored as UTF-16.
    static const char* g = "{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}";
    Buf b;
    for (int i = 0; i < 19; i++) b.u16(static_cast<uint16_t>(g[i]));
    return std::move(b.v);
}

}  // namespace

bool compileProject(const std::string& hhpPath, const std::string& outOverride,
                    CompileStats& stats, std::string& outPathUsed, std::string& err) {
    Project p;
    if (!parseHhp(hhpPath, p, err)) return false;

    const uint32_t lcid = parseLcid(p.opt("language"));
    std::string hhcName = slashes(p.opt("contents file"));
    std::string hhkName = slashes(p.opt("index file"));

    // hhc/hhk participate as ordinary archive files; make sure they are in the list
    auto ensureListed = [&](const std::string& f) {
        if (f.empty()) return;
        const std::string key = lowerCopy(f);
        for (const std::string& existing : p.files)
            if (lowerCopy(existing) == key) return;
        p.files.push_back(f);
    };
    ensureListed(hhcName);
    ensureListed(hhkName);
    if (p.files.empty()) {
        err = "project has no [FILES]";
        return false;
    }

    // ---- compressed section: content files, then #TOPICS/#URLSTR/#URLTBL/#STRINGS
    StringsTable strings;
    UrlTables urls;
    TopicsTable topics(strings, urls);

    std::vector<DirEntry> entries;
    Buf section1;
    auto addSec1 = [&](const std::string& archiveName, const std::vector<uint8_t>& data) {
        entries.push_back({archiveName, 1, section1.size(), data.size()});
        section1.raw(data.data(), data.size());
    };

    for (const std::string& f : p.files) {
        std::vector<uint8_t> data;
        if (!readFile(p.dir + f, data)) {
            err = "cannot read file: " + p.dir + f;
            return false;
        }
        const std::string archiveName = "/" + f;
        addSec1(archiveName, data);
        if (lowerCopy(f).find(".ht") != std::string::npos)
            topics.add(extractTitle(data), archiveName, -1);
    }
    stats.fileCount = p.files.size();

    if (!hhcName.empty()) topics.add("", hhcName, 2);
    if (!hhkName.empty()) topics.add("", hhkName, 2);

    if (topics.buf.size() != 0) addSec1("/#TOPICS", topics.buf.v);
    if (urls.urlstr.size() != 0) addSec1("/#URLSTR", urls.urlstr.v);
    if (urls.urltbl.size() != 0) addSec1("/#URLTBL", urls.urltbl.v);
    if (strings.buf.size() == 0) strings.buf.u8(0);
    addSec1("/#STRINGS", strings.buf.v);

    // ---- compress
    stats.uncompressedBytes = section1.size();
    const LzxResult lzx = lzxCompress(section1.v.data(), section1.size());
    stats.compressedBytes = lzx.data.size();

    // ---- section 0: #ITBITS, #SYSTEM, ::DataSpace files, Content last
    Buf section0;
    auto addSec0 = [&](const std::string& name, const std::vector<uint8_t>& data) {
        entries.push_back({name, 0, section0.size(), data.size()});
        section0.raw(data.data(), data.size());
    };
    entries.push_back({"/#ITBITS", 0, 0, 0});
    addSec0("/#SYSTEM", buildSystem(p, lcid, hhcName, hhkName));
    addSec0("::DataSpace/NameList", buildNameList());
    addSec0("::DataSpace/Storage/MSCompressed/ControlData", buildControlData());
    {
        Buf span;
        span.u64(section1.size());
        addSec0("::DataSpace/Storage/MSCompressed/SpanInfo", span.v);
    }
    addSec0("::DataSpace/Storage/MSCompressed/Transform/List", buildTransformList());
    addSec0(
        "::DataSpace/Storage/MSCompressed/Transform/"
        "{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable",
        buildResetTable(section1.size(), lzx.data.size(), lzx.frameStarts));
    // the LZX bytes are appended right after section 0; this entry points at them
    entries.push_back({"::DataSpace/Storage/MSCompressed/Content", 0, section0.size(),
                       lzx.data.size()});

    // ---- output path
    std::string out = outOverride;
    if (out.empty()) {
        const std::string compiled = slashes(p.opt("compiled file"));
        if (!compiled.empty()) {
            out = p.dir + compiled;
        } else {
            out = hhpPath;
            const size_t dot = out.find_last_of('.');
            out = (dot == std::string::npos ? out : out.substr(0, dot)) + ".chm";
        }
    }
    outPathUsed = out;

    if (!writeContainer(out, lcid, std::move(entries), section0.v, lzx.data, err))
        return false;
    stats.outputBytes = 0x78 /* headers */;  // refined below by file size query
    {
        std::ifstream f(out, std::ios::binary | std::ios::ate);
        if (f) stats.outputBytes = static_cast<uint64_t>(f.tellg());
    }
    return true;
}

}  // namespace fastchm
