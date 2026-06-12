// FastChm — compiles an HTML Help project (.hhp) into a .chm.
#pragma once
#include <string>

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

}  // namespace fastchm
