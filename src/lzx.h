// FastChm — LZX encoder with CHM parameters (64K window, reset interval 64K).
// Written from scratch against the public format documentation; see refs/NOTES.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastchm {

struct LzxResult {
    std::vector<uint8_t> data;           // compressed bitstream (16-bit aligned)
    std::vector<uint64_t> frameStarts;   // compressed offset of each 0x8000-byte frame
    uint64_t paddedSize = 0;             // input size after zero-padding to frame multiple
};

// Compresses `size` bytes for a CHM MSCompressed section. The input is zero-padded
// to a 0x8000 multiple internally; every 64K of input is compressed independently
// (full LZX reset), and the bitstream is 16-bit aligned at every 0x8000 boundary.
LzxResult lzxCompress(const uint8_t* input, size_t size);

}  // namespace fastchm
