// SPDX-License-Identifier: GPL-2.0-or-later
//
// Byte-level serialization primitives for the EPM1 container.
//
// Everything is written little-endian bit by bit, so the format does not
// depend on host endianness. uvarint is LEB128 (low 7 bits first, high bit
// = continuation).
#ifndef PROMETHEIA_VARINT_HPP
#define PROMETHEIA_VARINT_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace prometheia {

inline void put_uvarint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(char((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
}

inline uint64_t uvarint_size(uint64_t v) {
    uint64_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

// Returns the pointer past the consumed bytes, or nullptr if the value is
// truncated or overlong (>10 bytes).
inline const char* get_uvarint(const char* p, const char* end, uint64_t& out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (p == end)
            return nullptr;
        uint8_t byte = uint8_t(*p++);
        v |= uint64_t(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = v;
            return p;
        }
        shift += 7;
    }
    return nullptr;
}

inline void put_f64(std::string& out, double v) {
    uint64_t bits = std::bit_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        out.push_back(char((bits >> (8 * i)) & 0xFF));
}

inline const char* get_f64(const char* p, const char* end, double& v) {
    if (end - p < 8)
        return nullptr;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
        bits |= uint64_t(uint8_t(p[i])) << (8 * i);
    v = std::bit_cast<double>(bits);
    return p + 8;
}

inline void put_f32(std::string& out, float v) {
    uint32_t bits = std::bit_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i)
        out.push_back(char((bits >> (8 * i)) & 0xFF));
}

inline const char* get_f32(const char* p, const char* end, float& v) {
    if (end - p < 4)
        return nullptr;
    uint32_t bits = 0;
    for (int i = 0; i < 4; ++i)
        bits |= uint32_t(uint8_t(p[i])) << (8 * i);
    v = std::bit_cast<float>(bits);
    return p + 4;
}

inline void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(char((v >> (8 * i)) & 0xFF));
}

inline void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(char((v >> (8 * i)) & 0xFF));
}

inline uint32_t get_u32(const char* p) {
    return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
           (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
}

inline uint64_t get_u64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(uint8_t(p[i])) << (8 * i);
    return v;
}

inline void put_u16(std::string& out, uint16_t v) {
    out.push_back(char(v & 0xFF));
    out.push_back(char((v >> 8) & 0xFF));
}

inline uint16_t get_u16(const char* p) {
    return uint16_t(uint16_t(uint8_t(p[0])) | (uint16_t(uint8_t(p[1])) << 8));
}

} // namespace prometheia

#endif // PROMETHEIA_VARINT_HPP
