#include "builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bytebuf.h"
#include "chmwriter.h"
#include "fifti.h"
#include "lzx.h"
#include "objinst_data.h"
#include "sitemap.h"
#include "textenc.h"
#include "version.h"

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

bool isHtmlName(const std::string& name) {
    return lowerCopy(name).find(".ht") != std::string::npos &&
           lowerCopy(name).find(".hhc") == std::string::npos &&
           lowerCopy(name).find(".hhk") == std::string::npos;
}

// ---------------- HHP project ----------------

struct Project {
    std::string dir;  // directory of the .hhp, with trailing separator (or empty)
    std::unordered_map<std::string, std::string> options;  // lower-cased keys
    std::vector<std::string> files;                        // [FILES], deduped, ordered
    std::vector<std::string> windowLines;                  // raw [WINDOWS] lines
    std::vector<std::string> mergeFiles;                   // [MERGE FILES] entries
    std::vector<std::string> subsets;                      // [SUBSETS] entries
    std::vector<std::string> infoTypes;                    // [INFOTYPES] entries
    std::vector<std::pair<std::string, std::string>> aliases;  // name -> file
    std::vector<std::pair<std::string, uint32_t>> mapDefs;     // name -> context id

    std::string opt(const std::string& key) const {
        auto it = options.find(key);
        return it == options.end() ? "" : it->second;
    }
    bool optYes(const std::string& key) const {
        const std::string v = lowerCopy(opt(key));
        return v == "yes" || v == "true" || v == "1";
    }
};

void parseMapLine(const std::string& line, Project& p);

void parseAliasLine(const std::string& line, Project& p) {
    if (line[0] == '#') {  // #include alias.inc
        std::string inc = trim(line.substr(line.find_first_of(" \t") + 1));
        if (!inc.empty() && inc.front() == '"') inc = inc.substr(1, inc.size() - 2);
        std::vector<uint8_t> raw;
        if (readFile(p.dir + slashes(inc), raw)) {
            std::string text(raw.begin(), raw.end());
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                const std::string l = trim(text.substr(pos, nl - pos));
                pos = nl + 1;
                if (!l.empty() && l[0] != ';') parseAliasLine(l, p);
            }
        } else {
            fprintf(stderr, "fastchm: warning: cannot read alias include: %s\n", inc.c_str());
        }
        return;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return;
    std::string file = trim(line.substr(eq + 1));
    const size_t semi = file.find(';');  // strip trailing comment
    if (semi != std::string::npos) file = trim(file.substr(0, semi));
    p.aliases.emplace_back(trim(line.substr(0, eq)), slashes(file));
}

void parseMapLine(const std::string& line, Project& p) {
    if (line.compare(0, 7, "#define") == 0) {
        const std::string rest = trim(line.substr(7));
        const size_t sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) return;
        const std::string name = trim(rest.substr(0, sp));
        const uint32_t id =
            static_cast<uint32_t>(strtoul(trim(rest.substr(sp)).c_str(), nullptr, 0));
        p.mapDefs.emplace_back(name, id);
    } else if (line.compare(0, 8, "#include") == 0) {
        std::string inc = trim(line.substr(8));
        if (!inc.empty() && (inc.front() == '"' || inc.front() == '<'))
            inc = inc.substr(1, inc.size() - 2);
        std::vector<uint8_t> raw;
        if (readFile(p.dir + slashes(inc), raw)) {
            std::string text(raw.begin(), raw.end());
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                const std::string l = trim(text.substr(pos, nl - pos));
                pos = nl + 1;
                if (!l.empty()) parseMapLine(l, p);
            }
        } else {
            fprintf(stderr, "fastchm: warning: cannot read map include: %s\n", inc.c_str());
        }
    }
}

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
        } else if (section == "windows") {
            p.windowLines.push_back(line);
        } else if (section == "alias") {
            parseAliasLine(line, p);
        } else if (section == "map") {
            parseMapLine(line, p);
        } else if (section == "merge files") {
            p.mergeFiles.push_back(slashes(line));
        } else if (section == "subsets") {
            p.subsets.push_back(line);
        } else if (section == "infotypes") {
            p.infoTypes.push_back(line);
        }
        // [TEXT POPUPS]: not supported.
    }
    return true;
}

uint32_t parseLcid(const std::string& language) {
    unsigned long v = strtoul(language.c_str(), nullptr, 0);
    return v ? static_cast<uint32_t>(v) : 0x409;
}

// ---------------- [WINDOWS] definitions ----------------

// HHWIN_PARAM_* validity bits for the flags field at offset 0xC
enum : uint32_t {
    WP_PROPERTIES = 0x0002,  // navigation pane style
    WP_STYLES = 0x0004,
    WP_EXSTYLES = 0x0008,
    WP_RECT = 0x0010,
    WP_NAV_WIDTH = 0x0020,
    WP_SHOWSTATE = 0x0040,
    WP_TB_FLAGS = 0x0100,
    WP_EXPANSION = 0x0200,
    WP_TABPOS = 0x0400,
    WP_CUR_TAB = 0x2000,
};

struct Window {
    std::string type, caption, toc, index, defaultFile, home;
    std::string jump1File, jump1Text, jump2File, jump2Text;
    uint32_t navStyle = 0, navWidth = 0, buttons = 0;
    int32_t rect[4] = {0, 0, 0, 0};
    uint32_t styles = 0, exStyles = 0, showState = 0;
    uint32_t navClosed = 0, navDefault = 0, navPos = 0, notifyId = 0;
    uint32_t validFlags = 0;
};

// Splits one "name=arg,arg,..." window definition; quoted strings, hex numbers and
// a [l,t,r,b] rectangle, all positional (HH Workshop format).
Window parseWindowLine(const std::string& rawLine) {
    std::string line = rawLine;
    const size_t eq = line.find('=');
    if (eq != std::string::npos) line[eq] = ',';

    std::vector<std::string> tok;
    size_t i = 0;
    while (i <= line.size()) {
        std::string cur;
        if (i < line.size() && line[i] == '"') {
            const size_t close = line.find('"', i + 1);
            cur = line.substr(i + 1, close == std::string::npos ? std::string::npos
                                                                : close - i - 1);
            i = close == std::string::npos ? line.size() : close + 1;
            i = line.find(',', i);
            i = i == std::string::npos ? line.size() + 1 : i + 1;
        } else {
            size_t comma = line.find(',', i);
            if (comma == std::string::npos) comma = line.size();
            cur = trim(line.substr(i, comma - i));
            i = comma + 1;
        }
        tok.push_back(cur);
    }

    Window w;
    auto str = [&](size_t idx) { return idx < tok.size() ? tok[idx] : std::string(); };
    auto num = [&](size_t idx, uint32_t bit) -> uint32_t {
        if (idx >= tok.size() || tok[idx].empty()) return 0;
        if (bit) w.validFlags |= bit;
        return static_cast<uint32_t>(strtoul(tok[idx].c_str(), nullptr, 0));
    };

    w.type = str(0);
    w.caption = str(1);
    w.toc = slashes(str(2));
    w.index = slashes(str(3));
    w.defaultFile = slashes(str(4));
    w.home = slashes(str(5));
    w.jump1File = slashes(str(6));
    w.jump1Text = str(7);
    w.jump2File = slashes(str(8));
    w.jump2Text = str(9);
    w.navStyle = num(10, WP_PROPERTIES);
    w.navWidth = num(11, WP_NAV_WIDTH);
    w.buttons = num(12, WP_TB_FLAGS);

    // [l,t,r,b] spans four comma-separated tokens
    size_t idx = 13;
    if (idx < tok.size() && !tok[idx].empty() && tok[idx][0] == '[') {
        bool any = false;
        for (int k = 0; k < 4 && idx < tok.size(); k++, idx++) {
            std::string v = tok[idx];
            v.erase(std::remove(v.begin(), v.end(), '['), v.end());
            const size_t br = v.find(']');
            const bool last = br != std::string::npos;
            if (last) v = v.substr(0, br);
            if (!trim(v).empty()) any = true;
            w.rect[k] = static_cast<int32_t>(strtol(v.c_str(), nullptr, 0));
            if (last) {
                idx++;
                break;
            }
        }
        if (any) w.validFlags |= WP_RECT;
    } else if (idx < tok.size()) {
        idx++;  // empty rect slot
    }
    w.styles = num(idx++, WP_STYLES);
    w.exStyles = num(idx++, WP_EXSTYLES);
    w.showState = num(idx++, WP_SHOWSTATE);
    w.navClosed = num(idx++, WP_EXPANSION);
    w.navDefault = num(idx++, WP_CUR_TAB);
    w.navPos = num(idx++, WP_TABPOS);
    w.notifyId = num(idx++, 0);
    return w;
}

// ---------------- HTML scanning ----------------

// Extracts the <title>, decoding the file's encoding (UTF-8/UTF-16 BOM, charset, or
// `cp`) and returning the text re-encoded in `cp` for storage in #STRINGS.
std::string extractTitle(const std::vector<uint8_t>& html, uint32_t cp) {
    const std::vector<uint32_t> cps = decodeText(html.data(), html.size(), cp);
    auto lc = [](uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
    auto find = [&](const char* pat, size_t from) -> size_t {
        size_t plen = 0;
        while (pat[plen]) plen++;
        for (size_t i = from; i + plen <= cps.size(); i++) {
            bool ok = true;
            for (size_t j = 0; j < plen; j++)
                if (lc(cps[i + j]) != static_cast<uint32_t>(pat[j])) {
                    ok = false;
                    break;
                }
            if (ok) return i;
        }
        return std::string::npos;
    };
    size_t t = find("<title", 0);
    if (t == std::string::npos) return "";
    while (t < cps.size() && cps[t] != '>') t++;
    if (t >= cps.size()) return "";
    t++;
    const size_t end = find("</title", t);
    if (end == std::string::npos) return "";
    std::vector<uint32_t> title;
    bool ws = false;
    for (size_t i = t; i < end; i++) {
        const uint32_t c = cps[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ws = !title.empty();
        } else {
            if (ws) title.push_back(' ');
            ws = false;
            title.push_back(c);
        }
    }
    return encodeCodepage(title, cp);
}

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

// Collects href=/src= attribute values.
void extractRefs(const std::vector<uint8_t>& html, std::vector<std::string>& out) {
    const std::string text(html.begin(), html.end());
    const std::string lower = lowerCopy(text);
    for (const char* key : {"href", "src"}) {
        const size_t klen = strlen(key);
        size_t pos = 0;
        while ((pos = lower.find(key, pos)) != std::string::npos) {
            const size_t at = pos;
            pos += klen;
            if (at > 0 && isWordChar(lower[at - 1])) continue;  // e.g. data-src
            size_t i = pos;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
            if (i >= text.size() || text[i] != '=') continue;
            i++;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
            std::string value;
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                const char q = text[i++];
                while (i < text.size() && text[i] != q) value.push_back(text[i++]);
            } else {
                while (i < text.size() && text[i] != '>' &&
                       !std::isspace(static_cast<unsigned char>(text[i])))
                    value.push_back(text[i++]);
            }
            if (!value.empty()) out.push_back(value);
        }
    }
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Resolves a link found in `baseDir` (project-relative dir of the referring file,
// "" = project root) to a project-relative path; "" if external/unusable.
std::string resolveRef(const std::string& baseDir, std::string link) {
    link = trim(link);
    if (link.empty() || link[0] == '#') return "";
    const size_t colon = link.find(':');
    if (colon != std::string::npos && link.find('/') > colon) return "";  // scheme/drive
    link = link.substr(0, std::min(link.find('#'), link.find('?')));
    link = slashes(percentDecode(link));
    if (link.empty()) return "";

    std::string full = link[0] == '/' ? link.substr(1) : baseDir + link;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= full.size()) {
        size_t sl = full.find('/', i);
        if (sl == std::string::npos) sl = full.size();
        const std::string seg = full.substr(i, sl - i);
        i = sl + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.empty()) return "";  // escapes the project root
            parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); k++) out += (k ? "/" : "") + parts[k];
    return out;
}

std::string dirOf(const std::string& rel) {
    const size_t sl = rel.find_last_of('/');
    return sl == std::string::npos ? "" : rel.substr(0, sl + 1);
}

// ---------------- #STRINGS / #URLSTR+#URLTBL / #TOPICS ----------------

struct StringsTable {
    Buf buf;
    std::unordered_map<std::string, uint32_t> map;
    uint32_t add(const std::string& s) {
        if (buf.size() == 0) buf.u8(0);
        if (s.empty()) return 0;  // offset 0 is the leading NUL
        auto it = map.find(s);
        if (it != map.end()) return it->second;
        size_t pos = buf.size();
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
        const size_t entryLen = 9 + url.size();
        const size_t rem = 0x4000 - urlstr.size() % 0x4000;
        if (rem < entryLen) urlstr.zeros(rem);
        if (urlstr.size() % 0x4000 == 0) urlstr.u8(0);
        const uint32_t pos = static_cast<uint32_t>(urlstr.size());
        urlstr.u32(0);
        urlstr.u32(0);
        urlstr.strz(url);
        strmap[url] = pos;
        return pos;
    }
    uint32_t addUrl(const std::string& url, uint32_t topicIndex) {
        const uint32_t us = addUrlStr(url);
        if ((urltbl.size() & 0xFFF) == 0xFFC) urltbl.u32(0);
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
    std::unordered_map<std::string, uint32_t> byUrl;  // lower-cased url -> topic index
    TopicsTable(StringsTable& s, UrlTables& u) : strings(s), urls(u) {}

    uint32_t count() const { return static_cast<uint32_t>(buf.size() / 16); }

    uint32_t add(const std::string& title, std::string url, int code) {
        if (!url.empty() && url[0] == '/') url.erase(0, 1);
        const uint32_t topicIndex = count();
        const uint32_t strOff = title.empty() ? 0xFFFFFFFFu : strings.add(title);
        const uint32_t tblOff = urls.addUrl(url, topicIndex);
        uint16_t inContents;
        if (code >= 0)
            inContents = static_cast<uint16_t>(code);
        else if (url.find('#') != std::string::npos)
            inContents = 0;
        else
            inContents = title.empty() ? 2 : 6;
        buf.u32(0);  // patched when a binary TOC is generated
        buf.u32(strOff);
        buf.u32(tblOff);
        buf.u16(inContents);
        buf.u16(0);
        byUrl.emplace(lowerCopy(url), topicIndex);
        return topicIndex;
    }
    // index of the topic for `url`, or -1
    int find(std::string url) const {
        if (!url.empty() && url[0] == '/') url.erase(0, 1);
        auto it = byUrl.find(lowerCopy(url));
        return it == byUrl.end() ? -1 : static_cast<int>(it->second);
    }
};

// ---------------- #SYSTEM ----------------

void sysEntryStr(Buf& b, uint16_t code, const std::string& value) {
    b.u16(code);
    b.u16(static_cast<uint16_t>(value.size() + 1));
    b.strz(value);
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
    b.u16(0);
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
    b.u32(6);
    b.raw("LZXC", 4);
    b.u32(2);  // version
    b.u32(2);  // reset interval (0x8000 units)
    b.u32(2);  // window size (0x8000 units)
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
    b.u32(8);
    b.u32(0x28);
    b.u64(uncompressed);
    b.u64(compressed);
    b.u64(0x8000);
    for (uint64_t off : frameStarts) b.u64(off);
    return std::move(b.v);
}

std::vector<uint8_t> buildTransformList() {
    static const char* g = "{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}";
    Buf b;
    for (int i = 0; i < 19; i++) b.u16(static_cast<uint16_t>(g[i]));
    return std::move(b.v);
}

// #SUBSETS: WORD 0, WORD byte-count, then 12-byte entries (one per [SUBSETS] line;
// entry contents are unspecified scratch in real files, written as zeros here).
std::vector<uint8_t> buildSubsets(const std::vector<std::string>& subsets) {
    if (subsets.empty()) return {};
    Buf b;
    b.u16(0);
    b.u16(static_cast<uint16_t>(subsets.size() * 12));
    b.zeros(subsets.size() * 12);
    return std::move(b.v);
}

// ---------------- binary TOC (#TOCIDX) ----------------

// Layout: 4096-byte header {blockSize=4096, offset of 16-byte entries, count of
// local entries, offset of topic-index DWORD list}; then 20/28-byte page/book
// structs in level order; then one DWORD (#TOPICS index) per local entry; then
// 16-byte entries {pageBookOffset, 0x29A+seq, offset into DWORD list, topic index}.
// All offsets are file-relative (header included).
class TocIdxBuilder {
public:
    TocIdxBuilder(StringsTable& strings, TopicsTable& topics)
        : strings_(strings), topics_(topics) {}

    std::vector<uint8_t> build(const SiteMap& toc) {
        struct Group {
            const std::vector<SiteMapItem>* items;
            uint32_t parentPos;
            bool hasParent;
        };
        std::vector<Group> level{{&toc.items, 0, false}}, next;
        while (!level.empty()) {
            next.clear();
            for (const Group& g : level) {
                for (size_t j = 0; j < g.items->size(); j++) {
                    const SiteMapItem& item = (*g.items)[j];
                    const std::string local = slashes(item.param("local"));
                    const bool hasChildren = !item.children.empty();
                    const uint32_t props = (hasChildren ? 4u : 0) | (local.empty() ? 0 : 8u);
                    const uint32_t pos = static_cast<uint32_t>(info_.size());

                    if (j == 0 && g.hasParent)  // patch parent's FirstChildOffset
                        patch32(info_.v, g.parentPos + 0x14, 4096 + pos);

                    uint32_t topicsOrStrings;
                    if (!local.empty()) {
                        int t = topics_.find(local);
                        if (t < 0) t = static_cast<int>(topics_.add(item.param("name"), local, 2));
                        patch32(topics_.buf.v, static_cast<uint32_t>(t) * 16, 4096 + pos);
                        topicsOrStrings = static_cast<uint32_t>(t);
                        topicDwords_.u32(static_cast<uint32_t>(t));
                        tocEntries_.emplace_back(4096 + pos, static_cast<uint32_t>(t));
                        localSeq_++;
                    } else {
                        topicsOrStrings = strings_.add(item.param("name"));
                    }

                    info_.u16(0);
                    info_.u16(static_cast<uint16_t>(localSeq_));
                    info_.u32(props);
                    info_.u32(topicsOrStrings);
                    info_.u32(g.hasParent ? 4096 + g.parentPos : 0);
                    const bool lastSibling = j + 1 == g.items->size();
                    info_.u32(lastSibling ? 0 : 4096 + pos + (hasChildren ? 28 : 20));
                    if (hasChildren) {
                        info_.u32(0);  // FirstChildOffset, patched by the child
                        info_.u32(0);
                        next.push_back({&item.children, pos, true});
                    }
                }
            }
            level.swap(next);
        }

        const uint32_t topicsOffset = 4096 + static_cast<uint32_t>(info_.size());
        const uint32_t entriesOffset =
            topicsOffset + static_cast<uint32_t>(topicDwords_.size());
        Buf out;
        out.u32(4096);
        out.u32(entriesOffset);
        out.u32(localSeq_);
        out.u32(topicsOffset);
        out.zeros(4096 - out.size());
        out.raw(info_.v.data(), info_.size());
        out.raw(topicDwords_.v.data(), topicDwords_.size());
        for (size_t i = 0; i < tocEntries_.size(); i++) {
            out.u32(tocEntries_[i].first);
            out.u32(0x29A + static_cast<uint32_t>(i));
            out.u32(topicsOffset + static_cast<uint32_t>(i) * 4);
            out.u32(tocEntries_[i].second);
        }
        return std::move(out.v);
    }

private:
    static void patch32(std::vector<uint8_t>& v, size_t off, uint32_t value) {
        for (int k = 0; k < 4; k++) v[off + k] = static_cast<uint8_t>(value >> (k * 8));
    }
    StringsTable& strings_;
    TopicsTable& topics_;
    Buf info_, topicDwords_;
    std::vector<std::pair<uint32_t, uint32_t>> tocEntries_;
    uint32_t localSeq_ = 0;
};

// ---------------- BTree links ($WWKeywordLinks / $WWAssociativeLinks) ----------------

struct BinIndexFiles {
    std::vector<uint8_t> btree, data, map, property;
    uint32_t keywordCount = 0;
};

// Assembles the BTree/Data/Map/Property quad from pre-serialized keyword entries
// (already sorted by key). Shared by KLinks, ALinks and the .hhk binary index.
BinIndexFiles assembleBTree(const std::vector<std::vector<uint8_t>>& entries,
                            uint32_t lcid, uint32_t codepage) {
    // pack entries into 2048-byte listing blocks
    const size_t BS = 2048;
        std::vector<std::vector<uint8_t>> listBlocks;     // payloads (no header)
        std::vector<uint32_t> blockEntryCounts;
        std::vector<std::vector<uint8_t>> blockFirstIdx;  // first entry in index form
        std::vector<uint32_t> entriesBefore;              // per block, for the Map file
        std::vector<uint8_t> cur;
        uint32_t curCount = 0, doneEntries = 0;
        auto flush = [&]() {
            entriesBefore.push_back(doneEntries);
            doneEntries += curCount;
            listBlocks.push_back(std::move(cur));
            blockEntryCounts.push_back(curCount);
            cur.clear();
            curCount = 0;
        };
        for (const std::vector<uint8_t>& e : entries) {
            if (e.size() > BS - 12) continue;  // pathological keyword; skip
            if (12 + cur.size() + e.size() >= BS) flush();
            if (cur.empty()) {
                // index form: drop the trailing "increments by 13" DWORD and replace
                // the now-trailing constant-1 DWORD with the child block number
                std::vector<uint8_t> idx(e.begin(), e.end() - 8);
                const uint32_t child = static_cast<uint32_t>(listBlocks.size());
                for (int k = 0; k < 4; k++) idx.push_back(static_cast<uint8_t>(child >> (k * 8)));
                blockFirstIdx.push_back(std::move(idx));
            }
            cur.insert(cur.end(), e.begin(), e.end());
            curCount++;
        }
        if (!cur.empty() || listBlocks.empty()) flush();
        const uint32_t numList = static_cast<uint32_t>(listBlocks.size());

        // index levels
        std::vector<std::vector<std::vector<uint8_t>>> levels;  // [level][block] payload
        std::vector<std::vector<uint32_t>> levelCounts, levelFirstChild;
        std::vector<std::vector<uint8_t>> curIdxForms = std::move(blockFirstIdx);
        uint32_t levelBase = 0;  // block number of curIdxForms' child level start
        while (numList > 1) {
            std::vector<std::vector<uint8_t>> blocks;
            std::vector<uint32_t> counts, firstChild;
            std::vector<std::vector<uint8_t>> nextForms;
            std::vector<uint8_t> blk;
            uint32_t cnt = 0;
            const uint32_t thisLevelBase =
                levelBase + static_cast<uint32_t>(curIdxForms.size());
            auto flushIdx = [&](uint32_t firstChildBlock) {
                (void)firstChildBlock;
                blocks.push_back(std::move(blk));
                counts.push_back(cnt);
                blk.clear();
                cnt = 0;
            };
            for (size_t i = 0; i < curIdxForms.size(); i++) {
                std::vector<uint8_t>& e = curIdxForms[i];
                // child of this entry = block number levelBase + i
                const uint32_t child = levelBase + static_cast<uint32_t>(i);
                for (int k = 0; k < 4; k++)
                    e[e.size() - 4 + k] = static_cast<uint8_t>(child >> (k * 8));
                if (8 + blk.size() + e.size() >= BS) flushIdx(0);
                if (blk.empty()) {
                    firstChild.push_back(child);
                    // this block's first entry, child patched to the block we are
                    // building now (filled by the next level)
                    std::vector<uint8_t> up(e);
                    const uint32_t myNr =
                        thisLevelBase + static_cast<uint32_t>(blocks.size());
                    for (int k = 0; k < 4; k++)
                        up[up.size() - 4 + k] = static_cast<uint8_t>(myNr >> (k * 8));
                    nextForms.push_back(std::move(up));
                }
                blk.insert(blk.end(), e.begin(), e.end());
                cnt++;
            }
            if (!blk.empty()) flushIdx(0);
            levels.push_back(std::move(blocks));
            levelCounts.push_back(std::move(counts));
            levelFirstChild.push_back(std::move(firstChild));
            levelBase = thisLevelBase;
            curIdxForms = std::move(nextForms);
            if (curIdxForms.size() <= 1) break;
        }

        uint32_t totalBlocks = numList;
        for (const auto& lv : levels) totalBlocks += static_cast<uint32_t>(lv.size());

        // assemble BTree
        Buf bt;
        bt.u8(0x3B);
        bt.u8(0x29);
        bt.u16(2);  // flags
        bt.u16(static_cast<uint16_t>(BS));
        bt.raw("X44\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        bt.u32(0);
        bt.u32(numList - 1);   // last listing block
        bt.u32(totalBlocks - 1);  // root block (last listing block when no index)
        bt.i32(-1);
        bt.u32(totalBlocks);
        bt.u16(static_cast<uint16_t>(1 + levels.size()));  // tree depth
        bt.u32(static_cast<uint32_t>(entries.size()));
        bt.u32(codepage);
        bt.u32(lcid);
        bt.u32(1);  // CHM (not CHW)
        bt.u32(10031);
        bt.u32(0);
        bt.u32(0);
        bt.u32(0);
        for (uint32_t i = 0; i < numList; i++) {
            Buf h;
            h.u16(static_cast<uint16_t>(BS - 12 - listBlocks[i].size()));
            h.u16(static_cast<uint16_t>(blockEntryCounts[i]));
            h.i32(i == 0 ? -1 : static_cast<int32_t>(i - 1));
            h.i32(i + 1 == numList ? -1 : static_cast<int32_t>(i + 1));
            bt.raw(h.v.data(), h.size());
            bt.raw(listBlocks[i].data(), listBlocks[i].size());
            bt.zeros(BS - 12 - listBlocks[i].size());
        }
        for (size_t lv = 0; lv < levels.size(); lv++) {
            for (size_t b = 0; b < levels[lv].size(); b++) {
                Buf h;
                h.u16(static_cast<uint16_t>(BS - 8 - levels[lv][b].size()));
                h.u16(static_cast<uint16_t>(levelCounts[lv][b]));
                h.u32(levelFirstChild[lv][b]);
                bt.raw(h.v.data(), h.size());
                bt.raw(levels[lv][b].data(), levels[lv][b].size());
                bt.zeros(BS - 8 - levels[lv][b].size());
            }
        }

        BinIndexFiles out;
        out.keywordCount = static_cast<uint32_t>(entries.size());
        out.btree = std::move(bt.v);
        {
            Buf d;
            static const uint8_t dataEntry[13] = {0, 0, 0, 0, 5, 0, 0, 0, 0x80, 0, 0, 0, 0};
            for (size_t i = 0; i < entries.size(); i++) d.raw(dataEntry, 13);
            out.data = std::move(d.v);
        }
        {
            Buf m;
            m.u16(0);
            for (uint32_t i = 0; i < numList; i++) {
                m.u32(entriesBefore[i]);
                m.u32(i);
            }
            out.map = std::move(m.v);
        }
        {
            Buf pr;
            pr.u32(0);
            if (!entries.empty()) {
                pr.u32(0);
                pr.u32(0);
                pr.u32(0xC);
                pr.u32(1);
                pr.u32(1);
                pr.u32(0);
                pr.u32(0);
            }
            out.property = std::move(pr.v);
        }
        return out;
}

// One keyword contribution to a KLink/ALink BTree. Hierarchical .hhk items keep
// their comma-joined path, depth and char index; flat object-tag keywords are
// depth 0.
struct KeywordEntry {
    std::string display;
    std::string sortKey;
    uint16_t depth = 0;
    uint32_t charIndex = 0;
    bool seeAlso = false;
    std::string seeAlsoTarget;
    std::vector<uint32_t> topicIds;
};

// Collects keyword entries from .hhk sitemaps and/or embedded object-tag keywords,
// then emits one BTree quad.
class BTreeBuilder {
public:
    BTreeBuilder(TopicsTable& topics, uint32_t cp) : topics_(topics), cp_(cp) {}

    void addSiteMap(const SiteMap& sm) {
        std::vector<const SiteMapItem*> top;
        for (const SiteMapItem& it : sm.items) top.push_back(&it);
        std::sort(top.begin(), top.end(), [](const SiteMapItem* a, const SiteMapItem* b) {
            return lowerCopy(a->param("name")) < lowerCopy(b->param("name"));
        });
        for (const SiteMapItem* it : top) flatten(*it, it->param("name"), 0, 0);
    }

    // Registers a flat keyword pointing at a topic; duplicates accumulate topics.
    void addKeyword(const std::string& word, uint32_t topicId) {
        const std::string key = lowerCopy(word);
        KeywordEntry& e = flat_[key];
        if (e.display.empty()) {
            e.display = word;
            e.sortKey = key;
        }
        if (std::find(e.topicIds.begin(), e.topicIds.end(), topicId) == e.topicIds.end())
            e.topicIds.push_back(topicId);
    }

    bool empty() const { return entries_.empty() && flat_.empty(); }

    BinIndexFiles assemble(uint32_t lcid, uint32_t codepage) {
        for (auto& kv : flat_) entries_.push_back(std::move(kv.second));
        std::stable_sort(entries_.begin(), entries_.end(),
                         [](const KeywordEntry& a, const KeywordEntry& b) {
                             return a.sortKey < b.sortKey;
                         });
        std::vector<std::vector<uint8_t>> blobs;
        blobs.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); i++)
            blobs.push_back(serialize(entries_[i], static_cast<uint32_t>(i)));
        return assembleBTree(blobs, lcid, codepage);
    }

private:
    std::vector<uint8_t> serialize(const KeywordEntry& e, uint32_t seq) const {
        Buf b;
        appendUtf16le(b.v, decodeAuto(e.display, cp_));
        b.u16(0);
        b.u16(e.seeAlso ? 2 : 0);
        b.u16(e.depth);
        b.u32(e.charIndex);
        b.u32(0);
        if (e.seeAlso) {
            b.u32(1);
            appendUtf16le(b.v, decodeAuto(e.seeAlsoTarget, cp_));
            b.u16(0);
        } else {
            b.u32(static_cast<uint32_t>(e.topicIds.size()));
            for (uint32_t t : e.topicIds) b.u32(t);
        }
        b.u32(1);
        b.u32(seq * 13);
        return std::move(b.v);
    }

    void flatten(const SiteMapItem& item, const std::string& path, uint32_t charIndex,
                 uint16_t depth) {
        KeywordEntry e;
        e.display = path;
        e.sortKey = lowerCopy(path);
        e.depth = depth;
        e.charIndex = depth == 0 ? 0 : charIndex;
        const std::string seeAlso = item.param("see also");
        if (!seeAlso.empty()) {
            e.seeAlso = true;
            e.seeAlsoTarget = seeAlso;
        } else {
            std::string lastName = item.param("name");
            bool firstName = true;
            for (const auto& pr : item.params) {
                if (pr.first == "name") {
                    if (!firstName) lastName = pr.second;
                    firstName = false;
                } else if (pr.first == "local") {
                    const std::string local = slashes(pr.second);
                    int idx = topics_.find(local);
                    if (idx < 0) idx = static_cast<int>(topics_.add(lastName, local, -1));
                    e.topicIds.push_back(static_cast<uint32_t>(idx));
                }
            }
        }
        entries_.push_back(std::move(e));
        for (const SiteMapItem& child : item.children)
            flatten(child, path + ", " + child.param("name"),
                    static_cast<uint32_t>(path.size()) + 2, depth + 1);
    }

    TopicsTable& topics_;
    uint32_t cp_;
    std::vector<KeywordEntry> entries_;
    std::map<std::string, KeywordEntry> flat_;
};

// ---------------- $OBJINST ----------------

// Word-breaker / stemmer instantiation data required by the HH full-text search
// engine. Fixed content; byte layout as produced by hhc.exe.
std::vector<uint8_t> buildObjInst(uint32_t cp, uint32_t lcid) {
    static const uint8_t gWordBreaker[8] = {0x9A, 0x56, 0x00, 0xC0, 0x4F, 0xB6, 0x8B, 0xF7};
    static const uint8_t gStemmer[8] = {0x9A, 0x61, 0x00, 0xC0, 0x4F, 0xB6, 0x8B, 0xF7};
    static const uint8_t gSystemSort[8] = {0x9A, 0x56, 0x00, 0xC0, 0x4F, 0xB6, 0x8B, 0x66};
    Buf b;
    b.u32(0x04000000);
    b.u32(2);     // entries
    b.u32(24);    // entry 1 offset
    b.u32(2691);  // entry 1 size
    b.u32(2715);  // entry 2 offset
    b.u32(36);    // entry 2 size
    // entry 1: standard word breaker {4662DAAF-D393-11D0-9A56-00C04FB68BF7}
    b.guid(0x4662DAAF, 0xD393, 0x11D0, gWordBreaker);
    b.u32(0x04000000);
    b.u32(11);  // flags
    b.u32(cp);
    b.u32(lcid);
    b.u32(0);
    b.u32(0);
    b.u32(0x00145555);
    b.u32(0x00000A0F);
    b.u16(0x0100);
    b.u32(0x00030005);
    b.zeros(6 * 4);
    b.u16(0);
    for (int i = 0; i < 256; i++) b.raw(kObjInstCharTable[i], 10);
    b.u32(0xE66561C6);
    b.u32(0x73DF6561);
    b.u32(0x656F8C73);
    b.u16(0x6F9C);
    b.u8(0x65);
    // stemmer {8FA0D5A8-DEDF-11D0-9A61-00C04FB68BF7}
    b.guid(0x8FA0D5A8, 0xDEDF, 0x11D0, gStemmer);
    b.u32(0x04000000);
    b.u32(1);
    b.u32(cp);
    b.u32(lcid);
    b.u32(0);
    // entry 2: system sort {4662DAB0-D393-11D0-9A56-00C04FB68B66}
    b.guid(0x4662DAB0, 0xD393, 0x11D0, gSystemSort);
    b.u32(666);
    b.u32(cp);
    b.u32(lcid);
    b.u32(10031);
    b.u32(0);
    return std::move(b.v);
}

// ---------------- compiler ----------------

struct LoadedFile {
    std::string rel;  // project-relative path with forward slashes
    std::vector<uint8_t> data;
};

class Compiler {
public:
    Compiler(Project p) : p_(std::move(p)), topics_(strings_, urls_) {}

    bool run(const std::string& outOverride, CompileStats& stats,
             std::string& outPathUsed, std::string& err);

private:
    bool gatherFiles(std::string& err);
    std::vector<uint8_t> buildIvb();
    std::vector<uint8_t> buildWindows(const std::vector<Window>& windows);
    std::vector<uint8_t> buildIdxHdr();
    std::vector<uint8_t> buildSystem(const std::string& defaultWindow, bool binaryToc,
                                     bool hasKLinks, bool hasALinks,
                                     const std::vector<uint8_t>& idxhdr, bool fts);

    Project p_;
    uint32_t lcid_ = 0x409;
    uint32_t cp_ = 1252;
    std::string hhcName_, hhkName_;
    std::vector<LoadedFile> loaded_;
    SiteMap toc_, index_;
    bool haveToc_ = false, haveIndex_ = false;

    StringsTable strings_;
    UrlTables urls_;
    TopicsTable topics_;
    FtsIndexer fts_;
};

// BFS over [FILES] plus everything reachable through HTML href/src and sitemap
// Local params. Explicitly listed files keep their order and must exist; discovered
// files only warn when missing.
bool Compiler::gatherFiles(std::string& err) {
    std::vector<std::pair<std::string, bool>> queue;  // path, explicit
    std::unordered_set<std::string> enqueued;
    auto push = [&](const std::string& rel, bool explicitFile) {
        if (rel.empty()) return;
        if (enqueued.insert(lowerCopy(rel)).second) queue.emplace_back(rel, explicitFile);
    };
    for (const std::string& f : p_.files) push(f, true);

    for (size_t qi = 0; qi < queue.size(); qi++) {
        const std::string rel = queue[qi].first;
        const bool explicitFile = queue[qi].second;
        LoadedFile lf;
        lf.rel = rel;
        if (!readFile(p_.dir + rel, lf.data)) {
            if (explicitFile) {
                err = "cannot read file: " + p_.dir + rel;
                return false;
            }
            fprintf(stderr, "fastchm: warning: referenced file not found: %s\n",
                    rel.c_str());
            continue;
        }
        const std::string lrel = lowerCopy(rel);
        const bool isHhc = lrel == lowerCopy(hhcName_);
        const bool isHhk = !hhkName_.empty() && lrel == lowerCopy(hhkName_);
        if (isHhc || isHhk) {
            SiteMap sm = parseSiteMap(lf.data.data(), lf.data.size());
            std::vector<std::string> locals;
            sm.collectLocals(locals);
            for (const std::string& l : locals) push(resolveRef("", l), false);
            if (isHhc) {
                toc_ = std::move(sm);
                haveToc_ = true;
            } else {
                index_ = std::move(sm);
                haveIndex_ = true;
            }
        } else if (isHtmlName(rel)) {
            std::vector<std::string> refs;
            extractRefs(lf.data, refs);
            for (const std::string& r : refs) push(resolveRef(dirOf(rel), r), false);
        }
        loaded_.push_back(std::move(lf));
    }
    return true;
}

std::vector<uint8_t> Compiler::buildIvb() {
    std::map<uint32_t, std::string> ctx;  // id -> file, sorted by id
    for (const auto& def : p_.mapDefs) {
        bool found = false;
        for (const auto& alias : p_.aliases) {
            if (alias.first == def.first) {
                ctx[def.second] = alias.second;
                found = true;
                break;
            }
        }
        if (!found)
            fprintf(stderr, "fastchm: warning: [MAP] name has no [ALIAS] entry: %s\n",
                    def.first.c_str());
    }
    if (ctx.empty()) return {};
    Buf b;
    b.u32(static_cast<uint32_t>(ctx.size() * 8));
    for (const auto& c : ctx) {
        b.u32(c.first);
        b.u32(strings_.add(c.second));
    }
    return std::move(b.v);
}

std::vector<uint8_t> Compiler::buildWindows(const std::vector<Window>& windows) {
    if (windows.empty()) return {};
    Buf b;
    b.u32(static_cast<uint32_t>(windows.size()));
    b.u32(196);  // entry size (1.1+)
    for (const Window& w : windows) {
        b.u32(196);
        b.u32(0);  // unicode strings: no
        b.u32(strings_.add(w.type));
        b.u32(w.validFlags);
        b.u32(w.navStyle);
        b.u32(strings_.add(w.caption));
        b.u32(w.styles);
        b.u32(w.exStyles);
        for (int k = 0; k < 4; k++) b.i32(w.rect[k]);
        b.u32(w.showState);
        b.zeros(6 * 4);  // hwndHelp/Caller/paInfoTypes/Toolbar/Nav/Html (out params)
        b.u32(w.navWidth);
        b.zeros(4 * 4);  // topic pane rect (out)
        b.u32(strings_.add(w.toc));
        b.u32(strings_.add(w.index));
        b.u32(strings_.add(w.defaultFile));
        b.u32(strings_.add(w.home));
        b.u32(w.buttons);
        b.u32(w.navClosed);
        b.u32(w.navDefault);
        b.u32(w.navPos);
        b.u32(w.notifyId);
        b.zeros(5 * 4);  // tab order
        b.u32(0);        // history count
        b.u32(strings_.add(w.jump1Text));
        b.u32(strings_.add(w.jump2Text));
        b.u32(strings_.add(w.jump1File));
        b.u32(strings_.add(w.jump2File));
        b.zeros(4 * 4);  // rcMinSize
        b.u32(0);        // cbInfoTypes
        b.u32(0);        // pszCustomTabs
    }
    return std::move(b.v);
}

// 4096-byte #IDXHDR (also duplicated as #SYSTEM code 13). Emitted with binary index.
std::vector<uint8_t> Compiler::buildIdxHdr() {
    Buf b;
    b.raw("T#SM", 4);
    b.u32(0);  // timestamp/checksum
    b.u32(1);
    b.u32(topics_.count());
    b.u32(0);
    b.u32(0xFFFFFFFF);  // ImageList string ("none")
    b.u32(0);
    b.u32(0);           // ImageType=Folder flag
    b.u32(0xFFFFFFFF);  // background
    b.u32(0xFFFFFFFF);  // foreground
    b.u32(0xFFFFFFFF);  // font
    b.u32(0xFFFFFFFF);  // window styles
    b.u32(0);           // ex window styles
    b.u32(0xFFFFFFFF);
    b.u32(0);           // frame name
    b.u32(0xFFFFFFFF);  // window name
    b.u32(0);           // info types
    b.u32(1);
    b.u32(static_cast<uint32_t>(p_.mergeFiles.size()));
    b.u32(p_.mergeFiles.empty() ? 0 : 1);
    // 1004 DWORDs: merge-file #STRINGS offsets, zero-padded
    for (const std::string& mf : p_.mergeFiles) b.u32(strings_.add(mf));
    for (size_t i = p_.mergeFiles.size(); i < 1004; i++) b.u32(0);
    b.zeros(4096 - b.size());
    return std::move(b.v);
}

std::vector<uint8_t> Compiler::buildSystem(const std::string& defaultWindow,
                                           bool binaryToc, bool hasKLinks, bool hasALinks,
                                           const std::vector<uint8_t>& idxhdr, bool fts) {
    Buf b;
    b.u32(3);
    b.u16(10);
    b.u16(4);
    b.u32(static_cast<uint32_t>(time(nullptr)));
    sysEntryStr(b, 9, "FastChm " FASTCHM_VERSION);
    b.u16(4);
    b.u16(36);
    b.u32(lcid_);
    b.u32(isDbcsCodepage(cp_) ? 1 : 0);  // DBCS
    b.u32(fts ? 1 : 0);
    b.u32(hasKLinks ? 1 : 0);
    b.u32(hasALinks ? 1 : 0);
    b.u64(0);  // FILETIME
    b.u32(0);
    b.u32(0);
    const std::string defTopic = slashes(p_.opt("default topic"));
    if (!defTopic.empty()) sysEntryStr(b, 2, defTopic);
    if (!p_.opt("title").empty()) sysEntryStr(b, 3, p_.opt("title"));
    if (!p_.opt("default font").empty()) sysEntryStr(b, 16, p_.opt("default font"));
    if (!hhcName_.empty()) sysEntryStr(b, 0, hhcName_);
    if (!hhkName_.empty()) sysEntryStr(b, 1, hhkName_);
    if (!defaultWindow.empty()) sysEntryStr(b, 5, defaultWindow);
    if (!idxhdr.empty()) {  // binary index on
        b.u16(7);
        b.u16(4);
        b.u32(0);
    }
    if (binaryToc) {
        b.u16(11);
        b.u16(4);
        b.u32(0);
    }
    if (!p_.infoTypes.empty()) {  // code 12: number of information types
        b.u16(12);
        b.u16(4);
        b.u32(static_cast<uint32_t>(p_.infoTypes.size()));
    }
    if (!idxhdr.empty()) {
        b.u16(13);
        b.u16(static_cast<uint16_t>(idxhdr.size()));
        b.raw(idxhdr.data(), idxhdr.size());
    }
    return std::move(b.v);
}

bool Compiler::run(const std::string& outOverride, CompileStats& stats,
                   std::string& outPathUsed, std::string& err) {
    lcid_ = parseLcid(p_.opt("language"));
    cp_ = p_.opt("charset").empty()
              ? codepageForLcid(lcid_)
              : static_cast<uint32_t>(strtoul(p_.opt("charset").c_str(), nullptr, 0));
    hhcName_ = slashes(p_.opt("contents file"));
    hhkName_ = slashes(p_.opt("index file"));

    auto ensureListed = [&](const std::string& f) {
        if (f.empty()) return;
        for (const std::string& existing : p_.files)
            if (lowerCopy(existing) == lowerCopy(f)) return;
        p_.files.push_back(f);
    };
    ensureListed(hhcName_);
    ensureListed(hhkName_);
    if (p_.files.empty()) {
        err = "project has no [FILES]";
        return false;
    }

    if (!gatherFiles(err)) return false;

    std::vector<Window> windows;
    for (const std::string& line : p_.windowLines) windows.push_back(parseWindowLine(line));
    std::string defaultWindow = p_.opt("default window");
    if (defaultWindow.empty() && !windows.empty()) defaultWindow = windows[0].type;

    // ---- compressed section ----
    std::vector<DirEntry> entries;
    Buf section1;
    auto addSec1 = [&](const std::string& archiveName, const std::vector<uint8_t>& data) {
        entries.push_back({archiveName, 1, section1.size(), data.size()});
        section1.raw(data.data(), data.size());
    };

    const std::vector<uint8_t> ivb = buildIvb();
    if (!ivb.empty()) addSec1("/#IVB", ivb);
    const bool ftsOption =
        p_.optYes("full-text search") || p_.optYes("full text search");
    if (ftsOption) addSec1("/$OBJINST", buildObjInst(cp_, lcid_));

    const bool ftsWanted = ftsOption;
    BTreeBuilder klinks(topics_, cp_), alinks(topics_, cp_);
    for (const LoadedFile& lf : loaded_) {
        addSec1("/" + lf.rel, lf.data);
        if (isHtmlName(lf.rel)) {
            const uint32_t topicIdx =
                topics_.add(extractTitle(lf.data, cp_), "/" + lf.rel, -1);
            if (ftsWanted) fts_.indexFile(lf.data, topicIdx);
            const LinkObjects lo = scanLinkObjects(lf.data.data(), lf.data.size());
            for (const std::string& kw : lo.keywords) klinks.addKeyword(kw, topicIdx);
            for (const std::string& al : lo.alinks) alinks.addKeyword(al, topicIdx);
        }
    }
    stats.fileCount = loaded_.size();

    if (!hhcName_.empty()) topics_.add("", hhcName_, 2);
    if (!hhkName_.empty()) topics_.add("", hhkName_, 2);

    // binary TOC / binary index / links (built before #TOPICS et al. are serialized:
    // they add topics/strings and patch topic entries)
    const bool binaryToc = p_.optYes("binary toc") && haveToc_;
    const bool binaryIndex = p_.optYes("binary index") && haveIndex_;
    std::vector<uint8_t> tocIdx;
    if (binaryToc) tocIdx = TocIdxBuilder(strings_, topics_).build(toc_);
    if (binaryIndex) klinks.addSiteMap(index_);  // .hhk keywords join the KLink BTree

    const bool haveKLinks = !klinks.empty();
    const bool haveALinks = !alinks.empty();
    BinIndexFiles kFiles, aFiles;
    if (haveKLinks) kFiles = klinks.assemble(lcid_, cp_);
    if (haveALinks) aFiles = alinks.assemble(lcid_, cp_);

    // #IDXHDR is emitted for the binary-index tab and also to carry [MERGE FILES]
    std::vector<uint8_t> idxhdr;
    if (binaryIndex || !p_.mergeFiles.empty()) idxhdr = buildIdxHdr();

    if (topics_.buf.size() != 0) addSec1("/#TOPICS", topics_.buf.v);
    if (urls_.urlstr.size() != 0) addSec1("/#URLSTR", urls_.urlstr.v);
    if (urls_.urltbl.size() != 0) addSec1("/#URLTBL", urls_.urltbl.v);
    if (!tocIdx.empty()) addSec1("/#TOCIDX", tocIdx);
    if (haveKLinks) {
        addSec1("/$WWKeywordLinks/BTree", kFiles.btree);
        addSec1("/$WWKeywordLinks/Data", kFiles.data);
        addSec1("/$WWKeywordLinks/Map", kFiles.map);
        addSec1("/$WWKeywordLinks/Property", kFiles.property);
    }
    if (haveALinks) {
        addSec1("/$WWAssociativeLinks/BTree", aFiles.btree);
        addSec1("/$WWAssociativeLinks/Data", aFiles.data);
        addSec1("/$WWAssociativeLinks/Map", aFiles.map);
        addSec1("/$WWAssociativeLinks/Property", aFiles.property);
    } else if (haveKLinks) {
        Buf dummy;  // HH expects a (possibly empty) associative-links property file
        dummy.u32(0);
        addSec1("/$WWAssociativeLinks/Property", dummy.v);
    }
    const std::vector<uint8_t> windowsFile = buildWindows(windows);
    if (!windowsFile.empty()) addSec1("/#WINDOWS", windowsFile);
    const std::vector<uint8_t> subsetsFile = buildSubsets(p_.subsets);
    if (!subsetsFile.empty()) addSec1("/#SUBSETS", subsetsFile);
    if (!idxhdr.empty()) addSec1("/#IDXHDR", idxhdr);
    if (strings_.buf.size() == 0) strings_.buf.u8(0);
    addSec1("/#STRINGS", strings_.buf.v);
    std::vector<uint8_t> fifti;
    if (ftsWanted && fts_.hasData()) fifti = fts_.build(lcid_, cp_);
    if (!fifti.empty()) addSec1("/$FIftiMain", fifti);

    // ---- compress ----
    stats.uncompressedBytes = section1.size();
    const LzxResult lzx = lzxCompress(section1.v.data(), section1.size());
    stats.compressedBytes = lzx.data.size();

    // ---- section 0 ----
    Buf section0;
    auto addSec0 = [&](const std::string& name, const std::vector<uint8_t>& data) {
        entries.push_back({name, 0, section0.size(), data.size()});
        section0.raw(data.data(), data.size());
    };
    entries.push_back({"/#ITBITS", 0, 0, 0});
    addSec0("/#SYSTEM", buildSystem(defaultWindow, binaryToc, haveKLinks, haveALinks,
                                    idxhdr, !fifti.empty()));
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
    entries.push_back({"::DataSpace/Storage/MSCompressed/Content", 0, section0.size(),
                       lzx.data.size()});

    // ---- output ----
    std::string out = outOverride;
    if (out.empty()) {
        const std::string compiled = slashes(p_.opt("compiled file"));
        out = compiled.empty() ? "" : p_.dir + compiled;
    }
    outPathUsed = out;
    if (!writeContainer(out, lcid_, std::move(entries), section0.v, lzx.data, err))
        return false;
    std::ifstream f(out, std::ios::binary | std::ios::ate);
    if (f) stats.outputBytes = static_cast<uint64_t>(f.tellg());
    return true;
}

}  // namespace

bool compileProject(const std::string& hhpPath, const std::string& outOverride,
                    CompileStats& stats, std::string& outPathUsed, std::string& err) {
    Project p;
    if (!parseHhp(hhpPath, p, err)) return false;
    std::string out = outOverride;
    if (out.empty() && p.opt("compiled file").empty()) {
        out = hhpPath;
        const size_t dot = out.find_last_of('.');
        out = (dot == std::string::npos ? out : out.substr(0, dot)) + ".chm";
    }
    Compiler c(p);
    return c.run(out, stats, outPathUsed, err);
}

bool compileCollection(const std::string& masterHhp, std::vector<CollectionMember>& out,
                       std::string& err) {
    Project master;
    if (!parseHhp(masterHhp, master, err)) return false;
    const std::string dir = master.dir;  // master directory, trailing slash or empty

    auto fileExists = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return static_cast<bool>(f);
    };
    auto stem = [](const std::string& chm) {
        std::string s = chm;
        const size_t sl = s.find_last_of('/');
        if (sl != std::string::npos) s = s.substr(sl + 1);
        const size_t dot = s.find_last_of('.');
        return dot == std::string::npos ? s : s.substr(0, dot);
    };

    // children first, so they exist before the master is opened
    for (const std::string& mf : master.mergeFiles) {
        const std::string name = stem(mf);
        CollectionMember m;
        m.chm = dir + name + ".chm";
        const std::string flat = dir + name + ".hhp";
        const std::string nested = dir + name + "/" + name + ".hhp";
        const std::string childHhp =
            fileExists(flat) ? flat : (fileExists(nested) ? nested : "");
        if (childHhp.empty()) {
            m.reused = true;
            m.ok = fileExists(m.chm);
            if (!m.ok) m.err = "no " + name + ".hhp and no prebuilt " + name + ".chm";
            out.push_back(std::move(m));
            continue;
        }
        m.hhp = childHhp;
        std::string used;
        m.ok = compileProject(childHhp, m.chm, m.stats, used, m.err);
        out.push_back(std::move(m));
    }

    // master last
    CollectionMember mm;
    mm.isMaster = true;
    mm.hhp = masterHhp;
    std::string used;
    mm.ok = compileProject(masterHhp, "", mm.stats, used, mm.err);
    mm.chm = used;
    out.push_back(std::move(mm));

    return true;
}

}  // namespace fastchm
