// SPDX-License-Identifier: GPL-2.0-or-later
//
// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) over raw chunk
// bytes. Stored in the EPM1 chunk index; verified on every read.
#ifndef PROMETHEIA_CRC32_HPP
#define PROMETHEIA_CRC32_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace prometheia {

namespace detail {

// Slicing-by-8: table k is the CRC of a byte followed by k zero bytes, so
// eight input bytes fold in with eight lookups and no carried dependency
// between them. Table 0 is the classic byte-at-a-time table.
inline constexpr std::array<std::array<uint32_t, 256>, 8> make_crc32_tables() {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
        for (int k = 1; k < 8; ++k)
            t[k][i] = t[0][t[k - 1][i] & 0xFF] ^ (t[k - 1][i] >> 8);
    return t;
}

inline constexpr std::array<std::array<uint32_t, 256>, 8> kCrc32Tables = make_crc32_tables();

} // namespace detail

inline uint32_t crc32(const char* data, size_t n) {
    const auto& t = detail::kCrc32Tables;
    const auto* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    // Eight bytes at a time (a byte at a time was 45% of loading a catalog,
    // 2026-09-19); the bytes are assembled explicitly, so any endianness.
    for (; n >= 8; p += 8, n -= 8) {
        const uint32_t lo = c ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
                                 uint32_t(p[3]) << 24);
        c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
            t[3][p[4]] ^ t[2][p[5]] ^ t[1][p[6]] ^ t[0][p[7]];
    }
    for (; n > 0; ++p, --n)
        c = t[0][(c ^ *p) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace prometheia

#endif // PROMETHEIA_CRC32_HPP
