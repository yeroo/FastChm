// FastChm — a fast, zero-dependency CHM (HTML Help) compiler.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "builder.h"
#include "version.h"

static void printStats(const char* path, const fastchm::CompileStats& s) {
    printf("%s: %zu files, %llu -> %llu bytes (%.1f%%), output %llu bytes\n", path,
           s.fileCount, static_cast<unsigned long long>(s.uncompressedBytes),
           static_cast<unsigned long long>(s.compressedBytes),
           s.uncompressedBytes ? 100.0 * s.compressedBytes / s.uncompressedBytes : 0.0,
           static_cast<unsigned long long>(s.outputBytes));
}

int main(int argc, char** argv) {
    std::string hhp, out;
    bool collection = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (arg == "-c" || arg == "--collection") {
            collection = true;
        } else if (arg == "-v" || arg == "--version") {
            printf("FastChm %s\n", FASTCHM_VERSION);
            return 0;
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
        printf("FastChm %s — zero-dependency CHM compiler\n"
               "usage: fastchm <project.hhp> [-o output.chm]\n"
               "       fastchm --collection <master.hhp>   build master + [MERGE FILES] children\n"
               "       fastchm --version\n",
               FASTCHM_VERSION);
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();

    if (collection) {
        std::vector<fastchm::CollectionMember> members;
        std::string err;
        if (!fastchm::compileCollection(hhp, members, err)) {
            fprintf(stderr, "fastchm: error: %s\n", err.c_str());
            return 1;
        }
        int failures = 0;
        for (const fastchm::CollectionMember& m : members) {
            const char* tag = m.isMaster ? "master" : (m.reused ? "child*" : "child ");
            if (m.ok && m.reused) {
                printf("[%s] %s: reused prebuilt CHM (no .hhp)\n", tag, m.chm.c_str());
            } else if (m.ok) {
                printf("[%s] ", tag);
                printStats(m.chm.c_str(), m.stats);
            } else {
                fprintf(stderr, "[%s] %s: FAILED: %s\n", tag,
                        (m.hhp.empty() ? m.chm : m.hhp).c_str(), m.err.c_str());
                failures++;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        printf("collection: %zu members, %d failed, %.1f ms\n", members.size(), failures, ms);
        return failures ? 1 : 0;
    }

    fastchm::CompileStats stats;
    std::string outUsed, err;
    if (!fastchm::compileProject(hhp, out, stats, outUsed, err)) {
        fprintf(stderr, "fastchm: error: %s\n", err.c_str());
        return 1;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    printStats(outUsed.c_str(), stats);
    printf("  (%.1f ms)\n", ms);
    return 0;
}
