// SPDX-License-Identifier: GPL-2.0-or-later
#include <prometheia/cbor.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace prometheia {

namespace {

constexpr uint8_t kMajorUnsigned = 0;
constexpr uint8_t kMajorNegative = 1;
constexpr uint8_t kMajorText = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorSimple = 7;

void put_head(std::string& out, uint8_t major, uint64_t value) {
    uint8_t info;
    if (value < 24) {
        info = uint8_t(value);
    } else if (value <= 0xFF) {
        info = 24;
    } else if (value <= 0xFFFF) {
        info = 25;
    } else if (value <= 0xFFFFFFFF) {
        info = 26;
    } else {
        info = 27;
    }
    out.push_back(char((major << 5) | info));
    const int nbytes = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
    for (int i = nbytes - 1; i >= 0; --i)
        out.push_back(char((value >> (8 * i)) & 0xFF));
}

void put_float_head(std::string& out, uint8_t float_code) {
    out.push_back(char((kMajorSimple << 5) | float_code));
}

Result<double> decode_double_from_head(uint8_t byte, const char* p, const char* end,
                                       size_t& consumed) {
    const uint8_t info = byte & 0x1F;
    if (info == 25) { // half precision
        if (end - p < 2)
            return make_error(ErrorCode::FormatError, "CBOR: truncated half float");
        uint16_t h = uint16_t(uint8_t(p[0]) << 8) | uint8_t(p[1]);
        consumed = 3;
        // IEEE 754 half -> double without relying on _Float16.
        const uint16_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, frac = h & 0x3FF;
        double v;
        if (exp == 0) {
            v = std::ldexp(double(frac), -24);
        } else if (exp == 0x1F) {
            v = frac ? std::nan("") : HUGE_VAL;
        } else {
            // Normal: implicit leading 1 in the 10-bit fraction, exponent bias 15.
            v = std::ldexp(double(frac | 0x400), exp - 25);
        }
        return (sign ? -v : v);
    }
    if (info == 26) { // single precision
        if (end - p < 4)
            return make_error(ErrorCode::FormatError, "CBOR: truncated float");
        uint32_t bits = 0;
        for (int k = 0; k < 4; ++k)
            bits = (bits << 8) | uint8_t(p[k]);
        float f;
        std::memcpy(&f, &bits, 4);
        consumed = 5;
        return double(f);
    }
    if (info == 27) { // double precision
        if (end - p < 8)
            return make_error(ErrorCode::FormatError, "CBOR: truncated double");
        uint64_t bits = 0;
        for (int k = 0; k < 8; ++k)
            bits = (bits << 8) | uint8_t(p[k]);
        double d;
        std::memcpy(&d, &bits, 8);
        consumed = 9;
        return d;
    }
    return make_error(ErrorCode::FormatError, "CBOR: unsupported simple value");
}

struct DecodeState {
    const char* p;
    const char* end;
    int depth = 0;
};

constexpr int kMaxDepth = 64; // metadata is shallow; reject hostile nesting

Result<CborValue> decode_one(DecodeState& st) {
    if (st.p == st.end)
        return make_error(ErrorCode::FormatError, "CBOR: unexpected end");
    if (++st.depth > kMaxDepth)
        return make_error(ErrorCode::FormatError, "CBOR: nesting too deep");

    const uint8_t byte = uint8_t(*st.p++);
    const uint8_t major = byte >> 5;
    const uint8_t info = byte & 0x1F;

    auto read_arg = [&](uint64_t& arg) -> bool {
        if (info < 24) {
            arg = info;
            return true;
        }
        const int nbytes = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : -1;
        if (nbytes < 0 || st.end - st.p < nbytes)
            return false;
        arg = 0;
        for (int k = 0; k < nbytes; ++k)
            arg = (arg << 8) | uint8_t(*st.p++);
        return true;
    };

    CborValue v;
    switch (major) {
    case kMajorUnsigned: {
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated integer");
        v.type = CborValue::Type::Unsigned;
        v.u = arg;
        break;
    }
    case kMajorNegative: {
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated integer");
        v.type = CborValue::Type::Negative;
        v.i = -1 - int64_t(arg <= uint64_t(INT64_MAX) ? int64_t(arg) : INT64_MAX);
        break;
    }
    case 2: { // byte string — surfaced as Text with raw bytes (metadata only)
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated bytes");
        if (uint64_t(st.end - st.p) < arg)
            return make_error(ErrorCode::FormatError, "CBOR: truncated byte string");
        v.type = CborValue::Type::Text;
        v.text.assign(st.p, size_t(arg));
        st.p += arg;
        break;
    }
    case kMajorText: {
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated text");
        if (uint64_t(st.end - st.p) < arg)
            return make_error(ErrorCode::FormatError, "CBOR: truncated text string");
        v.type = CborValue::Type::Text;
        v.text.assign(st.p, size_t(arg));
        st.p += arg;
        break;
    }
    case kMajorArray: {
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated array");
        v.type = CborValue::Type::Array;
        v.items.reserve(size_t(arg));
        for (uint64_t k = 0; k < arg; ++k) {
            auto elem = decode_one(st);
            if (!elem)
                return elem.error();
            v.items.push_back(std::move(elem.value()));
        }
        break;
    }
    case kMajorMap: {
        uint64_t arg;
        if (!read_arg(arg))
            return make_error(ErrorCode::FormatError, "CBOR: truncated map");
        v.type = CborValue::Type::Map;
        v.items.reserve(2 * size_t(arg));
        for (uint64_t k = 0; k < 2 * arg; ++k) {
            auto elem = decode_one(st);
            if (!elem)
                return elem.error();
            v.items.push_back(std::move(elem.value()));
        }
        break;
    }
    case kMajorSimple: {
        if (info == 20 || info == 21) {
            v.type = CborValue::Type::Bool;
            v.b = info == 21;
            break;
        }
        if (info == 22) {
            v.type = CborValue::Type::Null;
            break;
        }
        if (info == 25 || info == 26 || info == 27) {
            size_t consumed = 0;
            auto d = decode_double_from_head(byte, st.p, st.end, consumed);
            if (!d)
                return d.error();
            st.p += consumed - 1; // the head byte was already consumed
            v.type = CborValue::Type::Double;
            v.d = d.value();
            break;
        }
        return make_error(ErrorCode::FormatError, "CBOR: unsupported simple value");
    }
    default:
        return make_error(ErrorCode::FormatError, "CBOR: unsupported major type");
    }

    --st.depth;
    return v;
}

void debug_render(std::string& out, const CborValue& v, int indent) {
    char buf[64];
    switch (v.type) {
    case CborValue::Type::Null:
        out += "null";
        break;
    case CborValue::Type::Bool:
        out += v.b ? "true" : "false";
        break;
    case CborValue::Type::Unsigned:
        std::snprintf(buf, sizeof buf, "%llu", (unsigned long long)v.u);
        out += buf;
        break;
    case CborValue::Type::Negative:
        std::snprintf(buf, sizeof buf, "%lld", (long long)v.i);
        out += buf;
        break;
    case CborValue::Type::Text:
        out += '"';
        for (char c : v.text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        out += '"';
        break;
    case CborValue::Type::Double:
        std::snprintf(buf, sizeof buf, "%.17g", v.d);
        out += buf;
        break;
    case CborValue::Type::Array: {
        out += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            out.append(size_t(indent + 2), ' ');
            debug_render(out, v.items[k], indent + 2);
            if (k + 1 < v.items.size())
                out += ',';
            out += '\n';
        }
        out.append(size_t(indent), ' ');
        out += ']';
        break;
    }
    case CborValue::Type::Map: {
        out += "{\n";
        for (size_t k = 0; k + 1 < v.items.size(); k += 2) {
            out.append(size_t(indent + 2), ' ');
            debug_render(out, v.items[k], indent + 2);
            out += ": ";
            debug_render(out, v.items[k + 1], indent + 2);
            if (k + 2 < v.items.size())
                out += ',';
            out += '\n';
        }
        out.append(size_t(indent), ' ');
        out += '}';
        break;
    }
    }
}

} // namespace

CborValue CborValue::make_unsigned(uint64_t v) {
    CborValue c;
    c.type = Type::Unsigned;
    c.u = v;
    return c;
}
CborValue CborValue::make_text(std::string_view s) {
    CborValue c;
    c.type = Type::Text;
    c.text.assign(s);
    return c;
}
CborValue CborValue::make_double(double v) {
    CborValue c;
    c.type = Type::Double;
    c.d = v;
    return c;
}
CborValue CborValue::make_bool(bool v) {
    CborValue c;
    c.type = Type::Bool;
    c.b = v;
    return c;
}
CborValue CborValue::make_array() {
    CborValue c;
    c.type = Type::Array;
    return c;
}
CborValue CborValue::make_map() {
    CborValue c;
    c.type = Type::Map;
    return c;
}

const CborValue* CborValue::find(std::string_view key) const {
    if (type != Type::Map)
        return nullptr;
    for (size_t k = 0; k + 1 < items.size(); k += 2) {
        if (items[k].type == Type::Text && items[k].text == key)
            return &items[k + 1];
    }
    return nullptr;
}

void cbor_encode(std::string& out, const CborValue& v) {
    switch (v.type) {
    case CborValue::Type::Null:
        out.push_back(char((kMajorSimple << 5) | 22));
        break;
    case CborValue::Type::Bool:
        out.push_back(char((kMajorSimple << 5) | (v.b ? 21 : 20)));
        break;
    case CborValue::Type::Unsigned:
        put_head(out, kMajorUnsigned, v.u);
        break;
    case CborValue::Type::Negative:
        put_head(out, kMajorNegative, uint64_t(-1 - v.i));
        break;
    case CborValue::Type::Text:
        put_head(out, kMajorText, v.text.size());
        out += v.text;
        break;
    case CborValue::Type::Double: {
        put_float_head(out, 27);
        uint64_t bits;
        std::memcpy(&bits, &v.d, 8);
        for (int k = 7; k >= 0; --k)
            out.push_back(char((bits >> (8 * k)) & 0xFF));
        break;
    }
    case CborValue::Type::Array:
        put_head(out, kMajorArray, v.items.size());
        for (const auto& item : v.items)
            cbor_encode(out, item);
        break;
    case CborValue::Type::Map:
        put_head(out, kMajorMap, v.items.size() / 2);
        for (const auto& item : v.items)
            cbor_encode(out, item);
        break;
    }
}

Result<CborValue> cbor_decode(const char* p, size_t n) {
    DecodeState st{p, p + n, 0};
    auto v = decode_one(st);
    if (!v)
        return v.error();
    if (st.p != st.end)
        return make_error(ErrorCode::FormatError, "CBOR: trailing bytes after value");
    return v;
}

std::string cbor_to_debug_string(const CborValue& v, int indent) {
    std::string out;
    debug_render(out, v, indent);
    return out;
}

} // namespace prometheia
