// FastChm — full-text search index ($FIftiMain) writer.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fastchm {

class FtsIndexer {
public:
    // Tokenizes the HTML (title + body text, script/style excluded) and records
    // word positions for the given #TOPICS index.
    void indexFile(const std::vector<uint8_t>& html, uint32_t topicIndex);

    // Builds the complete $FIftiMain file; empty when nothing was indexed.
    std::vector<uint8_t> build(uint32_t lcid, uint32_t codepage) const;

    bool hasData() const { return fileCount_ > 0 && !words_.empty(); }

private:
    // (word, context 0=body/1=title) -> topic index -> word positions (ascending)
    std::map<std::pair<std::string, int>, std::map<uint32_t, std::vector<uint32_t>>>
        words_;
    uint32_t fileCount_ = 0;
    uint64_t totalWords_ = 0, totalWordLength_ = 0;
    uint32_t longestWord_ = 0;
};

}  // namespace fastchm
