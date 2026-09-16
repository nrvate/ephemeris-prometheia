// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal CBOR (RFC 8949) codec for EPM1 container metadata.
//
// Supports the subset the container actually needs: unsigned/negative
// integers, text strings, 64-bit floats, bool, null, arrays and maps. The
// decoder also accepts 16/32-bit floats and byte strings (surfaced as text
// with a lossy flag is NOT done — byte strings decode as Type::Text with
// their raw bytes, which is fine for metadata keys we control).
#ifndef PROMETHEIA_CBOR_HPP
#define PROMETHEIA_CBOR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <prometheia/error.hpp>

namespace prometheia {

struct CborValue {
  enum class Type : uint8_t {
    Null,
    Bool,
    Unsigned,  // stored in u (>= 0)
    Negative,  // stored in i (always < 0)
    Text,      // stored in text
    Double,    // stored in d
    Array,     // elements in items
    Map,       // items holds key,value,key,value,... (keys are Text here)
  };

  Type type = Type::Null;
  uint64_t u = 0;
  int64_t i = 0;
  std::string text;
  double d = 0.0;
  bool b = false;
  std::vector<CborValue> items;

  static CborValue make_unsigned(uint64_t v);
  static CborValue make_text(std::string_view s);
  static CborValue make_double(double v);
  static CborValue make_bool(bool v);
  static CborValue make_array();
  static CborValue make_map();

  // Map lookup by text key; nullptr when this is not a map or key absent.
  const CborValue* find(std::string_view key) const;
};

void cbor_encode(std::string& out, const CborValue& v);
Result<CborValue> cbor_decode(const char* p, size_t n);

// Renders metadata as compact JSON-ish text for display (strings escaped,
// doubles via %g). Not a full JSON encoder — debugging/`prometheia-info` use.
std::string cbor_to_debug_string(const CborValue& v, int indent = 0);

}  // namespace prometheia

#endif  // PROMETHEIA_CBOR_HPP
