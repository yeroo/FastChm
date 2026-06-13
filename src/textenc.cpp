#include "textenc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

#include "codepage_tables.h"

namespace fastchm {
namespace {

const CodepageTable* findCp(uint32_t cp) {
    for (int i = 0; i < kNumCodepages; i++)
        if (kCodepages[i].cp == static_cast<int>(cp)) return &kCodepages[i];
    return nullptr;
}

uint32_t decodeByte(uint8_t b, const CodepageTable* t) {
    if (b < 0x80) return b;
    if (t) {
        uint16_t u = t->high[b - 0x80];
        return u;  // 0xFFFD for undefined slots
    }
    return b;  // no table: Latin-1 passthrough
}

// Reads a UTF-8 sequence at p; returns code point and advances p. Invalid bytes are
// passed through as Latin-1 so we never lose data.
uint32_t nextUtf8(const uint8_t* d, size_t n, size_t& p) {
    uint8_t b = d[p];
    if (b < 0x80) {
        p++;
        return b;
    }
    int len = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 0;
    if (len == 0 || p + len > n) {
        p++;
        return b;
    }
    for (int i = 1; i < len; i++)
        if ((d[p + i] & 0xC0) != 0x80) {  // malformed
            p++;
            return b;
        }
    uint32_t cp = b & (0x7F >> len);
    for (int i = 1; i < len; i++) cp = (cp << 6) | (d[p + i] & 0x3F);
    p += len;
    return cp;
}

// Looks for charset= in the first ~2KB (meta tag) and maps a few common names.
uint32_t sniffCharset(const uint8_t* d, size_t n) {
    const size_t lim = std::min<size_t>(n, 2048);
    std::string s;
    s.reserve(lim);
    for (size_t i = 0; i < lim; i++) s.push_back(static_cast<char>(std::tolower(d[i])));
    const size_t pos = s.find("charset");
    if (pos == std::string::npos) return 0;
    size_t i = pos + 7;
    while (i < s.size() && (s[i] == '=' || s[i] == ' ' || s[i] == '"' || s[i] == '\'' ||
                            s[i] == ':'))
        i++;
    std::string name;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-'))
        name.push_back(s[i++]);
    if (name == "utf-8" || name == "utf8") return 65001;
    if (name == "utf-16" || name == "utf-16le" || name == "unicode") return 1200;
    if (name.rfind("windows-", 0) == 0) return std::strtoul(name.c_str() + 8, nullptr, 10);
    if (name == "iso-8859-1" || name == "latin1") return 1252;
    return 0;
}

}  // namespace

uint32_t codepageForLcid(uint32_t lcid) {
    const uint32_t primary = lcid & 0x3FF;
    switch (primary) {
        case 0x04: case 0x11: case 0x12:           // Chinese(simplified handled below)/Japanese/Korean
            break;
        default: break;
    }
    static const std::unordered_map<uint32_t, uint32_t> map = {
        {0x0409, 1252}, {0x0809, 1252}, {0x0407, 1252}, {0x040C, 1252},  // en/de/fr
        {0x0410, 1252}, {0x040A, 1252}, {0x0413, 1252}, {0x041D, 1252},  // it/es/nl/sv
        {0x0419, 1251}, {0x0422, 1251}, {0x0402, 1251}, {0x041A, 1250},  // ru/uk/bg/hr
        {0x0405, 1250}, {0x040E, 1250}, {0x0415, 1250}, {0x0418, 1250},  // cs/hu/pl/ro
        {0x0408, 1253}, {0x041F, 1254}, {0x0427, 1257}, {0x0425, 1257},  // el/tr/lt/et
        {0x0426, 1257}, {0x040D, 1255}, {0x0401, 1256}, {0x041E, 874},   // lv/he/ar/th
        {0x0411, 932}, {0x0804, 936}, {0x0412, 949}, {0x0404, 950},      // ja/zh-CN/ko/zh-TW
        {0x042A, 1258},                                                  // vi
    };
    auto it = map.find(lcid);
    if (it != map.end()) return it->second;
    // fall back by primary language for unlisted sublocales
    static const std::unordered_map<uint32_t, uint32_t> byPrimary = {
        {0x19, 1251}, {0x22, 1251}, {0x02, 1251}, {0x1A, 1250}, {0x05, 1250},
        {0x0E, 1250}, {0x15, 1250}, {0x18, 1250}, {0x08, 1253}, {0x1F, 1254},
        {0x27, 1257}, {0x25, 1257}, {0x26, 1257}, {0x0D, 1255}, {0x01, 1256},
        {0x1E, 874},  {0x11, 932},  {0x12, 949},  {0x04, 936},  {0x2A, 1258},
    };
    auto jt = byPrimary.find(primary);
    return jt != byPrimary.end() ? jt->second : 1252;
}

bool isDbcsCodepage(uint32_t cp) {
    return cp == 932 || cp == 936 || cp == 949 || cp == 950 || cp == 1361;
}

std::vector<uint32_t> decodeCodepage(const std::string& s, uint32_t cp) {
    const CodepageTable* t = findCp(cp);
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(decodeByte(c, t));
    return out;
}

std::vector<uint32_t> decodeAuto(const std::string& s, uint32_t cp) {
    // valid UTF-8 with at least one multibyte sequence?
    bool multibyte = false, valid = true;
    for (size_t i = 0; i < s.size() && valid;) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        if (b < 0x80) {
            i++;
            continue;
        }
        int len = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 0;
        if (len == 0 || i + len > s.size()) {
            valid = false;
            break;
        }
        for (int k = 1; k < len; k++)
            if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) valid = false;
        multibyte = true;
        i += len;
    }
    if (valid && multibyte) {
        std::vector<uint32_t> out;
        size_t p = 0;
        const uint8_t* d = reinterpret_cast<const uint8_t*>(s.data());
        while (p < s.size()) out.push_back(nextUtf8(d, s.size(), p));
        return out;
    }
    return decodeCodepage(s, cp);
}

std::vector<uint32_t> decodeText(const uint8_t* data, size_t size, uint32_t fallbackCp) {
    std::vector<uint32_t> out;
    // BOM detection
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        size_t p = 3;
        while (p < size) out.push_back(nextUtf8(data, size, p));
        return out;
    }
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {  // UTF-16LE
        for (size_t p = 2; p + 1 < size; p += 2)
            out.push_back(data[p] | (data[p + 1] << 8));
        return out;
    }
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {  // UTF-16BE
        for (size_t p = 2; p + 1 < size; p += 2)
            out.push_back((data[p] << 8) | data[p + 1]);
        return out;
    }
    uint32_t cp = sniffCharset(data, size);
    if (cp == 0) cp = fallbackCp;
    if (cp == 65001) {  // UTF-8 without BOM
        size_t p = 0;
        while (p < size) out.push_back(nextUtf8(data, size, p));
        return out;
    }
    const CodepageTable* t = findCp(cp);
    out.reserve(size);
    for (size_t p = 0; p < size; p++) out.push_back(decodeByte(data[p], t));
    return out;
}

void appendUtf16le(std::vector<uint8_t>& out, const std::vector<uint32_t>& cps) {
    auto put = [&](uint16_t u) {
        out.push_back(static_cast<uint8_t>(u));
        out.push_back(static_cast<uint8_t>(u >> 8));
    };
    for (uint32_t c : cps) {
        if (c <= 0xFFFF) {
            put(static_cast<uint16_t>(c));
        } else {
            c -= 0x10000;
            put(static_cast<uint16_t>(0xD800 + (c >> 10)));
            put(static_cast<uint16_t>(0xDC00 + (c & 0x3FF)));
        }
    }
}

std::string encodeCodepage(const std::vector<uint32_t>& cps, uint32_t cp) {
    if (isDbcsCodepage(cp) || cp == 65001) {
        // best effort: UTF-8 (content is stored verbatim; this only affects metadata)
        std::string out;
        for (uint32_t c : cps) {
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else if (c < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else if (c < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (c >> 18)));
                out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return out;
    }
    const CodepageTable* t = findCp(cp);
    std::string out;
    out.reserve(cps.size());
    for (uint32_t c : cps) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        char mapped = '?';
        if (t) {
            for (int i = 0; i < 128; i++)
                if (t->high[i] == c) {
                    mapped = static_cast<char>(0x80 + i);
                    break;
                }
        } else if (c <= 0xFF) {
            mapped = static_cast<char>(c);  // Latin-1
        }
        out.push_back(mapped);
    }
    return out;
}

}  // namespace fastchm
