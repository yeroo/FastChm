// FastChm — little-endian byte buffer helpers.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fastchm {

struct Buf {
    std::vector<uint8_t> v;

    void u8(uint8_t x) { v.push_back(x); }
    void u16(uint16_t x) {
        v.push_back(static_cast<uint8_t>(x));
        v.push_back(static_cast<uint8_t>(x >> 8));
    }
    void u32(uint32_t x) {
        u16(static_cast<uint16_t>(x));
        u16(static_cast<uint16_t>(x >> 16));
    }
    void u64(uint64_t x) {
        u32(static_cast<uint32_t>(x));
        u32(static_cast<uint32_t>(x >> 32));
    }
    void i32(int32_t x) { u32(static_cast<uint32_t>(x)); }
    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        v.insert(v.end(), b, b + n);
    }
    void str(const std::string& s) { raw(s.data(), s.size()); }
    void strz(const std::string& s) {
        str(s);
        u8(0);
    }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
    // GUID layout: DWORD, WORD, WORD, BYTE[8]
    void guid(uint32_t a, uint16_t b, uint16_t c, const uint8_t (&d)[8]) {
        u32(a);
        u16(b);
        u16(c);
        raw(d, 8);
    }
    size_t size() const { return v.size(); }
};

// ITSS variable-length integer: 7 bits per byte, most significant group first,
// high bit set on all but the last byte.
inline void encint(std::vector<uint8_t>& out, uint64_t x) {
    uint8_t tmp[10];
    int n = 0;
    do {
        tmp[n++] = static_cast<uint8_t>(x & 0x7F);
        x >>= 7;
    } while (x);
    for (int i = n - 1; i > 0; i--) out.push_back(tmp[i] | 0x80);
    out.push_back(tmp[0]);
}

}  // namespace fastchm
