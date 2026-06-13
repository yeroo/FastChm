// FastChm — LZX decompressor for CHM MSCompressed sections (inverse of lzx.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastchm {

// Decompresses a CHM LZX stream. `resetIntervalBytes` and `windowBits` come from the
// section ControlData (CHM uses 0x10000 and 16). The stream is treated per the CHM
// rules: full state reset every reset interval, 16-bit realignment every 0x8000
// output bytes. Returns false and sets `err` on malformed input.
bool lzxDecompress(const uint8_t* comp, size_t compSize, uint64_t uncompressedSize,
                   uint32_t resetIntervalBytes, uint32_t windowBits,
                   std::vector<uint8_t>& out, std::string& err);

}  // namespace fastchm
