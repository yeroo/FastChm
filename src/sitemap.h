// FastChm — parser for HTML Help "sitemap" files (.hhc table of contents,
// .hhk index): nested <UL>/<OBJECT type="text/sitemap"><param ...> structure.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fastchm {

struct SiteMapItem {
    // ordered (name, value) pairs with lower-cased names; an .hhk entry may carry
    // several Name/Local pairs for multi-target keywords
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<SiteMapItem> children;

    // first value for the given lower-case param name ("" if absent)
    std::string param(const char* name) const;
};

struct SiteMap {
    std::vector<SiteMapItem> items;
    // params of <OBJECT type="text/site properties"> blocks (merged)
    std::vector<std::pair<std::string, std::string>> properties;

    // every "local" param value in the tree, in document order
    void collectLocals(std::vector<std::string>& out) const;
};

SiteMap parseSiteMap(const uint8_t* data, size_t size);

// Keyword/ALink references harvested from HTML Help <OBJECT> controls embedded in
// a topic page (param "Keyword" and "ALink Name", any object type).
struct LinkObjects {
    std::vector<std::string> keywords;
    std::vector<std::string> alinks;
};

LinkObjects scanLinkObjects(const uint8_t* data, size_t size);

}  // namespace fastchm
