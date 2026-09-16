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

inline constexpr std::array<uint32_t, 256> make_crc32_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}

inline constexpr std::array<uint32_t, 256> kCrc32Table = make_crc32_table();

}  // namespace detail

inline uint32_t crc32(const char* data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = detail::kCrc32Table[(c ^ uint8_t(data[i])) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace prometheia

#endif  // PROMETHEIA_CRC32_HPP
