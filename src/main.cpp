// FastChm — a fast, zero-dependency CHM (HTML Help) compiler.
#include <chrono>
#include <cstdio>
#include <string>

#include "builder.h"

int main(int argc, char** argv) {
    std::string hhp, out;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "-h" || arg == "--help" || arg == "/?") {
            hhp.clear();
            break;
        } else if (hhp.empty()) {
            hhp = arg;
        } else {
            fprintf(stderr, "fastchm: unexpected argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (hhp.empty()) {
        printf("FastChm 0.1 — zero-dependency CHM compiler\n"
               "usage: fastchm <project.hhp> [-o output.chm]\n");
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    fastchm::CompileStats stats;
    std::string outUsed, err;
    if (!fastchm::compileProject(hhp, out, stats, outUsed, err)) {
        fprintf(stderr, "fastchm: error: %s\n", err.c_str());
        return 1;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    printf("%s: %zu files, %llu -> %llu bytes (%.1f%%), output %llu bytes, %.1f ms\n",
           outUsed.c_str(), stats.fileCount,
           static_cast<unsigned long long>(stats.uncompressedBytes),
           static_cast<unsigned long long>(stats.compressedBytes),
           stats.uncompressedBytes
               ? 100.0 * stats.compressedBytes / stats.uncompressedBytes
               : 0.0,
           static_cast<unsigned long long>(stats.outputBytes), ms);
    return 0;
}
