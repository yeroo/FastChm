// FastChm — text encoding: decode source bytes (UTF-8/UTF-16/codepage) to Unicode
// code points, and re-encode metadata as UTF-16LE or a single-byte codepage.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fastchm {

// Maps a Windows LCID to its default ANSI codepage (common locales; 1252 default).
uint32_t codepageForLcid(uint32_t lcid);

// True for the East-Asian double-byte codepages (932/936/949/950/1361).
bool isDbcsCodepage(uint32_t codepage);

// Decodes raw file bytes to Unicode code points. Honours a UTF-8/UTF-16 BOM and an
// HTML `charset=` declaration; otherwise interprets bytes in `fallbackCp`.
std::vector<uint32_t> decodeText(const uint8_t* data, size_t size, uint32_t fallbackCp);

// Decodes a NUL-free byte string already known to be in `cp` (no BOM/sniffing).
std::vector<uint32_t> decodeCodepage(const std::string& s, uint32_t cp);

// Decodes a string of unknown encoding: valid multibyte UTF-8 → UTF-8, else `cp`.
// (For .hhc/.hhk param values, whose encoding is not separately declared.)
std::vector<uint32_t> decodeAuto(const std::string& s, uint32_t cp);

// UTF-16LE bytes for the code points (with surrogate pairs for > 0xFFFF).
void appendUtf16le(std::vector<uint8_t>& out, const std::vector<uint32_t>& cps);

// Encodes code points back to a single-byte codepage; un-representable code points
// become '?'. For DBCS codepages, falls back to UTF-8 bytes (best effort).
std::string encodeCodepage(const std::vector<uint32_t>& cps, uint32_t cp);

}  // namespace fastchm
