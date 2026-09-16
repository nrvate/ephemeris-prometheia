// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <prometheia/cbor.hpp>

#include "test_main.hpp"

using prometheia::CborValue;
using prometheia::cbor_decode;
using prometheia::cbor_encode;
using prometheia::ErrorCode;

namespace {

CborValue roundtrip(const CborValue& v) {
  std::string out;
  cbor_encode(out, v);
  auto decoded = cbor_decode(out.data(), out.size());
  CHECK(decoded.ok());
  return decoded.ok() ? std::move(decoded.value()) : CborValue{};
}

bool doubles_equal_exact(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

}  // namespace

TEST(unsigned_roundtrip) {
  for (uint64_t v : {0ull, 23ull, 24ull, 255ull, 256ull, 65535ull, 65536ull,
                     0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull}) {
    auto r = roundtrip(CborValue::make_unsigned(v));
    CHECK(r.type == CborValue::Type::Unsigned);
    CHECK(r.u == v);
  }
}

TEST(negative_roundtrip) {
  const int64_t values[] = {-1, -24, -25, -256, -65536, INT64_MIN / 2};
  for (int64_t v : values) {
    auto r = roundtrip([&] {
      CborValue c;
      c.type = CborValue::Type::Negative;
      c.i = v;
      return c;
    }());
    CHECK(r.type == CborValue::Type::Negative);
    CHECK(r.i == v);
  }
}

TEST(text_roundtrip) {
  for (const char* s : {"", "Ceres", "2003 SB220", "quote\"and\\slash", "new\nline"}) {
    auto r = roundtrip(CborValue::make_text(s));
    CHECK(r.type == CborValue::Type::Text);
    CHECK(r.text == s);
  }
}

TEST(double_roundtrip) {
  for (double v : {0.0, -0.0, 0.5, 3.141592653589793, 1e300, -1e-300,
                   2461200.5}) {
    auto r = roundtrip(CborValue::make_double(v));
    CHECK(r.type == CborValue::Type::Double);
    CHECK(doubles_equal_exact(r.d, v));
  }
  // Infinity encodes/decodes through the f64 path.
  auto r = roundtrip(CborValue::make_double(HUGE_VAL));
  CHECK(r.type == CborValue::Type::Double);
  CHECK(std::isinf(r.d) && r.d > 0);
}

TEST(bool_null_roundtrip) {
  auto t = roundtrip(CborValue::make_bool(true));
  CHECK(t.type == CborValue::Type::Bool && t.b);
  auto n = roundtrip(CborValue{});
  CHECK(n.type == CborValue::Type::Null);
}

TEST(map_array_roundtrip) {
  CborValue meta = CborValue::make_map();
  meta.items.push_back(CborValue::make_text("frame"));
  meta.items.push_back(CborValue::make_text("ICRF"));
  CborValue counts = CborValue::make_array();
  counts.items.push_back(CborValue::make_unsigned(1564460));
  counts.items.push_back(CborValue::make_unsigned(3743));
  meta.items.push_back(CborValue::make_text("counts"));
  meta.items.push_back(std::move(counts));
  meta.items.push_back(CborValue::make_text("epoch"));
  meta.items.push_back(CborValue::make_double(2461200.5));

  auto r = roundtrip(meta);
  CHECK(r.type == CborValue::Type::Map);
  const CborValue* frame = r.find("frame");
  CHECK(frame != nullptr && frame->text == "ICRF");
  const CborValue* counts2 = r.find("counts");
  CHECK(counts2 != nullptr && counts2->items.size() == 2);
  CHECK(counts2->items[1].u == 3743);
  const CborValue* missing = r.find("nope");
  CHECK(missing == nullptr);
}

TEST(decode_errors) {
  // Truncated buffer.
  std::string out;
  cbor_encode(out, CborValue::make_text("hello world"));
  auto t = cbor_decode(out.data(), out.size() - 3);
  CHECK(!t.ok() && t.error().code == ErrorCode::FormatError);

  // Trailing bytes rejected.
  out.push_back(char(0x00));
  auto x = cbor_decode(out.data(), out.size());
  CHECK(!x.ok() && x.error().code == ErrorCode::FormatError);

  // Empty input.
  auto e = cbor_decode(nullptr, 0);
  CHECK(!e.ok());

  // Half-float decode (0x3C00 = 1.0) accepted into Double.
  const unsigned char half[] = {0xF9, 0x3C, 0x00};
  auto h = cbor_decode(reinterpret_cast<const char*>(half), sizeof half);
  CHECK(h.ok() && h.value().type == CborValue::Type::Double &&
        h.value().d == 1.0);
}

TEST(debug_string) {
  CborValue m = CborValue::make_map();
  m.items.push_back(CborValue::make_text("a"));
  m.items.push_back(CborValue::make_unsigned(1));
  const std::string s = prometheia::cbor_to_debug_string(m);
  CHECK(s.find("\"a\"") != std::string::npos);
  CHECK(s.find(": 1") != std::string::npos);
}

int main() { return ptest::run_all(); }
