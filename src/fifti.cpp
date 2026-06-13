// $FIftiMain layout (refs/NOTES.md, chmspec 5.3.13):
//   0x400-byte header; then, interleaved, WLC bit-data and 4096-byte leaf nodes;
//   then 4096-byte index nodes, bottom level first, root last.
// Words are stored prefix-compressed against the previous word in the same node.
// WLC data per (word, context): per document {topic-index delta, occurrence count,
// position deltas}, scale/root coded (s=2), each document entry right-padded to a
// byte. Integers in leaf entries use little-endian 7-bit ENCINTs.
#include "fifti.h"

#include <algorithm>

#include "bytebuf.h"

namespace fastchm {
namespace {

constexpr size_t NODE = 4096;
constexpr int R_DOC = 2, R_CODE = 1, R_LOC = 5;

char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// ---- tokenizer ----

struct TextRun {
    std::string text;
    bool title;
};

std::string decodeEntity(const std::string& ent) {
    if (ent == "amp") return "&";
    if (ent == "lt") return "<";
    if (ent == "gt") return ">";
    if (ent == "quot") return "\"";
    if (ent == "apos") return "'";
    if (ent == "nbsp") return " ";
    if (!ent.empty() && ent[0] == '#') {
        const bool hex = ent.size() > 1 && lower(ent[1]) == 'x';
        const long code = strtol(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
        if (code > 0 && code < 256) return std::string(1, static_cast<char>(code));
    }
    return " ";
}

// Splits the document into text runs, tracking <title> (before <body>) and <body>;
// script/style content is dropped. Mirrors the reference indexer's state machine.
std::vector<TextRun> extractText(const std::vector<uint8_t>& html) {
    const std::string s(html.begin(), html.end());
    std::vector<TextRun> runs;
    bool inTitle = false, inBody = false, inScript = false, inStyle = false;
    std::string cur;
    bool curTitle = false;
    auto flush = [&]() {
        if (!cur.empty()) runs.push_back({std::move(cur), curTitle});
        cur.clear();
    };
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            if (s.compare(i, 4, "<!--") == 0) {
                const size_t end = s.find("-->", i);
                i = end == std::string::npos ? s.size() : end + 3;
                continue;
            }
            size_t close = s.find('>', i);
            if (close == std::string::npos) break;
            std::string tag;
            for (size_t k = i + 1; k < close && tag.size() < 8; k++) tag.push_back(lower(s[k]));
            flush();
            if (inBody) {
                if (tag.compare(0, 5, "/body") == 0) inBody = false;
                else if (tag.compare(0, 6, "script") == 0) inScript = true;
                else if (tag.compare(0, 7, "/script") == 0) inScript = false;
                else if (tag.compare(0, 5, "style") == 0) inStyle = true;
                else if (tag.compare(0, 6, "/style") == 0) inStyle = false;
            } else {
                if (tag.compare(0, 5, "title") == 0) inTitle = true;
                else if (tag.compare(0, 6, "/title") == 0) inTitle = false;
                else if (tag.compare(0, 4, "body") == 0) inBody = true;
            }
            i = close + 1;
            continue;
        }
        char c = s[i];
        if (c == '&') {
            const size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                cur += decodeEntity(s.substr(i + 1, semi - i - 1));
                i = semi + 1;
                if ((inTitle && !inBody) || (inBody && !inScript && !inStyle))
                    curTitle = inTitle && !inBody;
                continue;
            }
        }
        if ((inTitle && !inBody) || (inBody && !inScript && !inStyle)) {
            curTitle = inTitle && !inBody;
            cur.push_back(c);
        }
        i++;
    }
    flush();
    return runs;
}

bool isWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

// ---- scale/root bit encoding ----

class BitWriterMSB {
public:
    explicit BitWriterMSB(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint32_t value, int nbits) {
        for (int b = nbits - 1; b >= 0; b--) {
            buf_ = static_cast<uint8_t>((buf_ << 1) | ((value >> b) & 1));
            if (++used_ == 8) {
                out_.push_back(buf_);
                buf_ = 0;
                used_ = 0;
            }
        }
    }
    void alignByte() {
        if (used_) put(0, 8 - used_);
    }

private:
    std::vector<uint8_t>& out_;
    uint8_t buf_ = 0;
    int used_ = 0;
};

int bitlen(uint32_t v) {
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

void srPut(BitWriterMSB& bw, uint32_t v, int root) {
    const int m = bitlen(v);
    if (m <= root) {
        bw.put(0, 1);
        bw.put(v, root);
    } else {
        bw.put(0xFFFFFFFFu, m - root);  // prefix of (m-root) ones
        bw.put(0, 1);
        bw.put(v - (1u << (m - 1)), m - 1);
    }
}

void encintLE(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t groups[5];
    int n = 0;
    do {
        groups[n++] = v & 0x7F;
        v >>= 7;
    } while (v);
    for (int i = 0; i < n; i++)
        out.push_back(static_cast<uint8_t>(groups[i] | (i + 1 < n ? 0x80 : 0)));
}

size_t encintLen(uint32_t v) {
    size_t n = 0;
    do {
        n++;
        v >>= 7;
    } while (v);
    return n;
}

// shared prefix length against the previous word in the node
size_t commonPrefix(const std::string& a, const std::string& b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
    return n;
}

// ---- node assembly ----

struct NodeBuf {
    std::vector<uint8_t> content;  // entries only (header added at flush)
    std::string lastWord;
    bool used() const { return !content.empty(); }
};

class FiftiWriter {
public:
    explicit FiftiWriter(Buf& file) : file_(file) {}

    void addWord(const std::string& word, int context,
                 const std::map<uint32_t, std::vector<uint32_t>>& docs) {
        // WLC bit data
        std::vector<uint8_t> wlc;
        BitWriterMSB bw(wlc);
        uint32_t lastDoc = 0;
        for (const auto& d : docs) {
            srPut(bw, d.first - lastDoc, R_DOC);
            lastDoc = d.first;
            srPut(bw, static_cast<uint32_t>(d.second.size()), R_CODE);
            uint32_t lastLoc = 0;
            for (uint32_t loc : d.second) {
                srPut(bw, loc - lastLoc, R_LOC);
                lastLoc = loc;
            }
            bw.alignByte();
        }

        auto entrySize = [&](const std::string& base) {
            const size_t part = word.size() - commonPrefix(word, base);
            return 2 + part + 1 + encintLen(static_cast<uint32_t>(docs.size())) + 6 +
                   encintLen(static_cast<uint32_t>(wlc.size()));
        };
        if (leaf_.used() && 8 + leaf_.content.size() + entrySize(leaf_.lastWord) > NODE)
            flushLeaf(true);

        const uint32_t wlcOffset = static_cast<uint32_t>(file_.size());
        file_.raw(wlc.data(), wlc.size());

        const size_t prefix = leaf_.used() ? commonPrefix(word, leaf_.lastWord) : 0;
        std::vector<uint8_t>& c = leaf_.content;
        c.push_back(static_cast<uint8_t>(word.size() - prefix + 1));
        c.push_back(static_cast<uint8_t>(prefix));
        c.insert(c.end(), word.begin() + prefix, word.end());
        c.push_back(static_cast<uint8_t>(context));
        encintLE(c, static_cast<uint32_t>(docs.size()));
        for (int k = 0; k < 4; k++) c.push_back(static_cast<uint8_t>(wlcOffset >> (k * 8)));
        c.push_back(0);
        c.push_back(0);
        encintLE(c, static_cast<uint32_t>(wlc.size()));
        leaf_.lastWord = word;
    }

    // flushes everything; returns {rootOffset, leafCount, treeDepth, lastLeafFree}
    void finish(uint32_t& rootOffset, uint32_t& leafCount, uint16_t& treeDepth,
                uint32_t& lastLeafFree) {
        if (leaf_.used()) flushLeaf(false);
        for (size_t lv = 0; lv < levels_.size(); lv++) {
            // register in the parent unless this is the (single-block) root
            flushIndexNode(lv, lv + 1 < levels_.size());
        }
        rootOffset = static_cast<uint32_t>(file_.size()) - NODE;
        leafCount = leafCount_;
        treeDepth = static_cast<uint16_t>(1 + levels_.size());
        lastLeafFree = lastLeafFree_;
    }

private:
    void flushLeaf(bool more) {
        const uint32_t myStart = static_cast<uint32_t>(file_.size());
        if (prevLeafStart_ != UINT32_MAX) {  // patch previous leaf's next pointer
            for (int k = 0; k < 4; k++)
                file_.v[prevLeafStart_ + k] = static_cast<uint8_t>(myStart >> (k * 8));
        }
        prevLeafStart_ = myStart;
        const uint32_t free =
            static_cast<uint32_t>(NODE - 8 - leaf_.content.size());
        lastLeafFree_ = free;
        Buf h;
        h.u32(0);  // next leaf (patched by the next flush)
        h.u16(0);
        h.u16(static_cast<uint16_t>(free));
        file_.raw(h.v.data(), h.size());
        file_.raw(leaf_.content.data(), leaf_.content.size());
        file_.zeros(free);
        leafCount_++;
        // register in the bottom index level when the tree needs one
        if (more || !levels_.empty()) addIndexEntry(0, leaf_.lastWord, myStart);
        leaf_ = NodeBuf();
    }

    void addIndexEntry(size_t level, const std::string& word, uint32_t childOffset) {
        if (level >= levels_.size()) levels_.emplace_back();
        NodeBuf* node = &levels_[level];
        size_t prefix = node->used() ? commonPrefix(word, node->lastWord) : 0;
        if (2 + node->content.size() + (2 + word.size() - prefix + 6) > NODE) {
            flushIndexNode(level, true);
            node = &levels_[level];  // levels_ may have grown
            prefix = 0;
        }
        std::vector<uint8_t>& c = node->content;
        c.push_back(static_cast<uint8_t>(word.size() - prefix + 1));
        c.push_back(static_cast<uint8_t>(prefix));
        c.insert(c.end(), word.begin() + prefix, word.end());
        for (int k = 0; k < 4; k++) c.push_back(static_cast<uint8_t>(childOffset >> (k * 8)));
        c.push_back(0);
        c.push_back(0);
        node->lastWord = word;
    }

    void flushIndexNode(size_t level, bool registerInParent) {
        NodeBuf& node = levels_[level];
        if (!node.used()) return;
        const uint32_t myStart = static_cast<uint32_t>(file_.size());
        if (registerInParent) addIndexEntry(level + 1, node.lastWord, myStart);
        const uint16_t free = static_cast<uint16_t>(NODE - 2 - node.content.size());
        Buf h;
        h.u16(free);
        file_.raw(h.v.data(), h.size());
        file_.raw(node.content.data(), node.content.size());
        file_.zeros(free);
        node = NodeBuf();
    }

    Buf& file_;
    NodeBuf leaf_;
    std::vector<NodeBuf> levels_;
    uint32_t leafCount_ = 0;
    uint32_t prevLeafStart_ = UINT32_MAX;
    uint32_t lastLeafFree_ = 0;
};

}  // namespace

void FtsIndexer::indexFile(const std::vector<uint8_t>& html, uint32_t topicIndex) {
    const std::vector<TextRun> runs = extractText(html);
    if (runs.empty()) return;
    fileCount_++;
    uint32_t pos = 0;  // word position counter, shared across title and body
    for (const TextRun& run : runs) {
        const std::string& t = run.text;
        size_t i = 0;
        while (i < t.size()) {
            const char c0 = lower(t[i]);
            if (!isWordChar(c0)) {
                i++;
                continue;
            }
            const bool numberWord = c0 >= '0' && c0 <= '9';
            std::string word;
            while (i < t.size()) {
                const char c = lower(t[i]);
                if (isWordChar(c)) word.push_back(c);
                else if (numberWord && c == '.' && !word.empty()) word.push_back(c);
                else if (c == '\'' && !word.empty()) { /* elided */ }
                else break;
                i++;
            }
            if (word.size() <= 99) {
                auto& docs = words_[{word, run.title ? 1 : 0}];
                docs[topicIndex].push_back(pos);
                totalWords_++;
                totalWordLength_ += word.size();
                longestWord_ = std::max(longestWord_, static_cast<uint32_t>(word.size()));
            }
            pos++;
        }
    }
}

std::vector<uint8_t> FtsIndexer::build(uint32_t lcid, uint32_t codepage) const {
    if (!hasData()) return {};
    Buf file;
    file.zeros(0x400);  // header written last

    FiftiWriter writer(file);
    uint64_t uniqueLen = 0;
    for (const auto& w : words_) {
        writer.addWord(w.first.first, w.first.second, w.second);
        uniqueLen += w.first.first.size();
    }
    uint32_t rootOffset, leafCount, lastLeafFree;
    uint16_t treeDepth;
    writer.finish(rootOffset, leafCount, treeDepth, lastLeafFree);

    Buf h;
    h.u8(0);
    h.u8(0);
    h.u8(0x28);
    h.u8(0);
    h.u32(fileCount_);
    h.u32(rootOffset);
    h.u32(0);
    h.u32(leafCount);
    h.u32(rootOffset);
    h.u16(treeDepth);
    h.u32(7);
    h.u8(2);  // doc index scale/root
    h.u8(R_DOC);
    h.u8(2);  // code count scale/root
    h.u8(R_CODE);
    h.u8(2);  // location code scale/root
    h.u8(R_LOC);
    h.zeros(10);
    h.u32(NODE);
    h.u32(0);
    h.u32(1);  // "word index of the last duplicate" (statistic; value from chmcmd)
    h.u32(5);  // "char index of the last duplicate"
    h.u32(longestWord_);
    h.u32(static_cast<uint32_t>(totalWords_));
    h.u32(static_cast<uint32_t>(words_.size()));
    h.u32(static_cast<uint32_t>(totalWordLength_));
    h.u32(0);
    h.u32(static_cast<uint32_t>(uniqueLen));
    h.u32(lastLeafFree);
    h.u32(0);
    h.u32(fileCount_ - 1);
    h.zeros(24);
    h.u32(codepage);
    h.u32(lcid);
    std::copy(h.v.begin(), h.v.end(), file.v.begin());
    return std::move(file.v);
}

}  // namespace fastchm
