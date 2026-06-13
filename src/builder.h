// FastChm — compiles an HTML Help project (.hhp) into a .chm.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fastchm {

struct CompileStats {
    size_t fileCount = 0;
    uint64_t uncompressedBytes = 0;
    uint64_t compressedBytes = 0;
    uint64_t outputBytes = 0;
};

// Returns false and sets `err` on failure. `outOverride` empty = use the project's
// "Compiled file" option (or <project>.chm).
bool compileProject(const std::string& hhpPath, const std::string& outOverride,
                    CompileStats& stats, std::string& outPathUsed, std::string& err);

// One member of a collection build.
struct CollectionMember {
    std::string hhp;       // project compiled (empty if a prebuilt child was reused)
    std::string chm;       // output path
    bool isMaster = false;
    bool ok = false;
    bool reused = false;   // child .chm referenced but no .hhp found — left as-is
    CompileStats stats;
    std::string err;
};

// Builds a collection: compiles the master and every child referenced in its
// [MERGE FILES], emitting all CHMs into the master's directory so the runtime merge
// resolves them. A child <name>.chm is compiled from <name>.hhp or <name>/<name>.hhp
// next to the master; if neither exists the existing <name>.chm is kept. Returns
// false only on a hard failure (sets `err`); per-member errors are in `out`.
bool compileCollection(const std::string& masterHhp, std::vector<CollectionMember>& out,
                       std::string& err);

}  // namespace fastchm
