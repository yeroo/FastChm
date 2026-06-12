#include "sitemap.h"

#include <algorithm>
#include <cstdlib>

namespace fastchm {
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

std::string lowerCopy(std::string s) {
    for (char& c : s) c = lower(c);
    return s;
}

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(s[i++]);
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            const long code = strtol(ent.c_str() + 1 + (ent.size() > 1 && lower(ent[1]) == 'x'),
                                     nullptr, ent.size() > 1 && lower(ent[1]) == 'x' ? 16 : 10);
            if (code > 0 && code < 256) out.push_back(static_cast<char>(code));
            // non-Latin-1 entities are dropped; sitemaps are ANSI in practice
        } else {
            out.append(s, i, semi - i + 1);
        }
        i = semi + 1;
    }
    return out;
}

struct Tag {
    std::string name;  // lower-cased, includes leading '/' for close tags
    std::vector<std::pair<std::string, std::string>> attrs;  // lower-cased names
};

// Parses the tag starting at `i` (s[i] == '<'); advances `i` past the tag.
Tag parseTag(const std::string& s, size_t& i) {
    Tag t;
    i++;  // '<'
    while (i < s.size() && (s[i] == '/' || !isspace(static_cast<unsigned char>(s[i])))) {
        if (s[i] == '>') break;
        t.name.push_back(lower(s[i++]));
    }
    while (i < s.size() && s[i] != '>') {
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i >= s.size() || s[i] == '>' || s[i] == '/') {
            if (i < s.size() && s[i] == '/') i++;
            continue;
        }
        std::string name;
        while (i < s.size() && s[i] != '=' && s[i] != '>' &&
               !isspace(static_cast<unsigned char>(s[i])))
            name.push_back(lower(s[i++]));
        std::string value;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i < s.size() && s[i] == '=') {
            i++;
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                const char q = s[i++];
                while (i < s.size() && s[i] != q) value.push_back(s[i++]);
                if (i < s.size()) i++;
            } else {
                while (i < s.size() && s[i] != '>' &&
                       !isspace(static_cast<unsigned char>(s[i])))
                    value.push_back(s[i++]);
            }
        }
        if (!name.empty()) t.attrs.emplace_back(name, decodeEntities(value));
    }
    if (i < s.size()) i++;  // '>'
    return t;
}

std::string attr(const Tag& t, const char* name) {
    for (const auto& a : t.attrs)
        if (a.first == name) return a.second;
    return "";
}

}  // namespace

std::string SiteMapItem::param(const char* name) const {
    for (const auto& p : params)
        if (p.first == name) return p.second;
    return "";
}

void SiteMap::collectLocals(std::vector<std::string>& out) const {
    struct Rec {
        static void walk(const std::vector<SiteMapItem>& items,
                         std::vector<std::string>& out) {
            for (const SiteMapItem& it : items) {
                for (const auto& p : it.params)
                    if (p.first == "local" && !p.second.empty()) out.push_back(p.second);
                walk(it.children, out);
            }
        }
    };
    Rec::walk(items, out);
}

SiteMap parseSiteMap(const uint8_t* data, size_t size) {
    SiteMap sm;
    const std::string s(reinterpret_cast<const char*>(data), size);

    std::vector<std::vector<SiteMapItem>*> stack{&sm.items};
    enum { NONE, SITEMAP, PROPERTIES } objectMode = NONE;
    SiteMapItem pending;

    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            i++;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            const size_t end = s.find("-->", i);
            i = end == std::string::npos ? s.size() : end + 3;
            continue;
        }
        Tag t = parseTag(s, i);
        if (t.name == "ul") {
            std::vector<SiteMapItem>* top = stack.back();
            stack.push_back(top->empty() ? top : &top->back().children);
        } else if (t.name == "/ul") {
            if (stack.size() > 1) stack.pop_back();
        } else if (t.name == "object") {
            const std::string type = lowerCopy(attr(t, "type"));
            if (type == "text/sitemap") {
                objectMode = SITEMAP;
                pending = SiteMapItem();
            } else if (type == "text/site properties") {
                objectMode = PROPERTIES;
            }
        } else if (t.name == "param" && objectMode != NONE) {
            const std::string name = lowerCopy(attr(t, "name"));
            const std::string value = attr(t, "value");
            if (!name.empty()) {
                if (objectMode == SITEMAP)
                    pending.params.emplace_back(name, value);
                else
                    sm.properties.emplace_back(name, value);
            }
        } else if (t.name == "/object") {
            if (objectMode == SITEMAP) stack.back()->push_back(std::move(pending));
            objectMode = NONE;
        }
    }
    return sm;
}

}  // namespace fastchm
