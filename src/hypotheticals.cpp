// SPDX-License-Identifier: GPL-2.0-or-later
//
// Element files for named hypothetical bodies: JSON Lines, read strictly.
// docs/HYPOTHETICALS.md, "Element files", defines the schema.
#include "prometheia/hypotheticals.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <utility>

namespace prometheia::hypotheticals {

namespace {

// Just enough JSON for one element line (RFC 8259 grammar, read strictly):
// the schema needs objects, arrays, strings and numbers, and the rest of the
// grammar is parsed only so that it can be refused with a clear message.
struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;
};

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    bool value(Value& out, int depth) {
        ws();
        if (depth > 8)
            return fail("nested too deeply");
        if (i_ >= s_.size())
            return fail("expected a value");
        switch (s_[i_]) {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out.kind = Value::Kind::String;
            return string(out.str);
        case 't':
            out.kind = Value::Kind::Bool;
            return literal("true");
        case 'f':
            out.kind = Value::Kind::Bool;
            return literal("false");
        case 'n':
            out.kind = Value::Kind::Null;
            return literal("null");
        default:
            out.kind = Value::Kind::Number;
            return number(out.number);
        }
    }

    bool at_end() {
        ws();
        return i_ == s_.size();
    }
    const std::string& why() const { return why_; }

private:
    bool fail(std::string why) {
        if (why_.empty())
            why_ = std::move(why);
        return false;
    }
    void ws() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
            ++i_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word)
            return fail("unexpected character");
        i_ += word.size();
        return true;
    }
    bool object(Value& out, int depth) {
        out.kind = Value::Kind::Object;
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"')
                return fail("expected a field name in quotes");
            if (!string(key))
                return false;
            for (const auto& m : out.members)
                if (m.first == key)
                    return fail("field \"" + key + "\" appears twice");
            ws();
            if (i_ >= s_.size() || s_[i_] != ':')
                return fail("expected ':' after a field name");
            ++i_;
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.members.emplace_back(std::move(key), std::move(v));
            ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected ',' or '}' in an object");
        }
    }
    bool array(Value& out, int depth) {
        out.kind = Value::Kind::Array;
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.items.push_back(std::move(v));
            ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected ',' or ']' in a list");
        }
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& cp) {
        if (i_ + 4 > s_.size())
            return fail("truncated \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9')
                cp |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp |= uint32_t(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i_; // the opening quote
        for (;;) {
            if (i_ >= s_.size())
                return fail("unterminated string");
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return fail("unterminated string");
            const char e = s_[i_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo = 0;
                    if (s_.substr(i_, 2) != "\\u")
                        return fail("unpaired surrogate");
                    i_ += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000)
                        return fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("unpaired surrogate");
                }
                utf8(out, cp);
                break;
            }
            default:
                return fail("bad escape in a string");
            }
        }
    }
    // The strict JSON number grammar is checked here, so that from_chars --
    // which would also take "inf", "nan" and hexadecimal -- only ever sees
    // what JSON allows.
    bool number(double& out) {
        const size_t start = i_;
        const auto digits = [&] {
            const size_t from = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                ++i_;
            return i_ > from;
        };
        if (i_ < s_.size() && s_[i_] == '-')
            ++i_;
        if (i_ < s_.size() && s_[i_] == '0')
            ++i_;
        else if (!digits())
            return fail("unexpected character");
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits())
                return fail("a number needs digits after '.'");
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            if (!digits())
                return fail("a number needs digits in its exponent");
        }
        const char* first = s_.data() + start;
        const char* last = s_.data() + i_;
        const auto r = std::from_chars(first, last, out);
        if (r.ec != std::errc() || r.ptr != last || !std::isfinite(out))
            return fail("number out of range");
        return true;
    }

    std::string_view s_;
    size_t i_ = 0;
    std::string why_;
};

bool is_token(const std::string& t) {
    if (t.empty() || t.size() > 64)
        return false;
    for (size_t k = 0; k < t.size(); ++k) {
        const char c = t[k];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (k > 0 && c == '-');
        if (!ok)
            return false;
    }
    return true;
}

// One parsed line -> a Body, or the reason it is not one.
std::string body_of(const Value& v, Body& b) {
    if (v.kind != Value::Kind::Object)
        return "each line must be one JSON object";
    PolynomialElements& el = b.elements;
    el = PolynomialElements{};
    bool seen_epoch = false, seen_equinox = false;
    int seen_elements = 0;
    const auto text = [](const Value& x, std::string& out) {
        if (x.kind != Value::Kind::String)
            return false;
        out = x.str;
        return true;
    };
    // Each element is a list of 1..5 finite coefficients; the body's term
    // count is the longest, the rest stay zero-padded.
    const auto coefficients = [&](const Value& x, double out[5]) -> std::string {
        if (x.kind != Value::Kind::Array || x.items.empty() || x.items.size() > 5)
            return "must be a list of 1 to 5 numbers";
        for (size_t k = 0; k < x.items.size(); ++k) {
            if (x.items[k].kind != Value::Kind::Number)
                return "must be a list of 1 to 5 numbers";
            out[k] = x.items[k].number;
        }
        el.n_terms = std::max(el.n_terms, int(x.items.size()));
        ++seen_elements;
        return {};
    };
    for (const auto& [key, x] : v.members) {
        std::string why;
        if (key == "token") {
            if (!text(x, b.token) || !is_token(b.token))
                why = "must be a lowercase identifier (a-z, 0-9, '-')";
        } else if (key == "name") {
            if (!text(x, b.name))
                why = "must be a string";
        } else if (key == "set") {
            if (!text(x, b.set) || b.set.empty())
                why = "must be a non-empty string";
        } else if (key == "citation") {
            if (!text(x, b.citation) || b.citation.empty())
                why = "must be a non-empty string";
        } else if (key == "epoch") {
            if (x.kind != Value::Kind::Number)
                why = "must be a Julian date (TT)";
            el.epoch_jd_tt = x.number;
            seen_epoch = true;
        } else if (key == "equinox") {
            seen_equinox = true;
            if (x.kind == Value::Kind::Number) {
                el.equinox = ElementEquinox::Explicit;
                el.equinox_jd_tt = x.number;
            } else if (x.kind == Value::Kind::String && x.str == "J2000") {
                el.equinox = ElementEquinox::J2000;
            } else if (x.kind == Value::Kind::String && x.str == "B1950") {
                el.equinox = ElementEquinox::B1950;
            } else if (x.kind == Value::Kind::String && x.str == "J1900") {
                el.equinox = ElementEquinox::J1900;
            } else if (x.kind == Value::Kind::String && x.str == "of date") {
                el.equinox = ElementEquinox::OfDate;
            } else {
                why = "must be \"J2000\", \"B1950\", \"J1900\", \"of date\" or a Julian date";
            }
        } else if (key == "centre") {
            if (x.kind == Value::Kind::String && x.str == "sun")
                el.centre = ElementCentre::Sun;
            else if (x.kind == Value::Kind::String && x.str == "earth")
                el.centre = ElementCentre::Earth;
            else
                why = "must be \"sun\" or \"earth\"";
        } else if (key == "M") {
            why = coefficients(x, el.mean_anomaly);
        } else if (key == "a") {
            why = coefficients(x, el.semi_major_axis);
        } else if (key == "e") {
            why = coefficients(x, el.eccentricity);
        } else if (key == "w") {
            why = coefficients(x, el.arg_perihelion);
        } else if (key == "node") {
            why = coefficients(x, el.ascending_node);
        } else if (key == "i") {
            why = coefficients(x, el.inclination);
        } else {
            return "unknown field \"" + key + "\"";
        }
        if (!why.empty())
            return "\"" + key + "\" " + why;
    }
    if (b.token.empty())
        return "missing \"token\"";
    if (b.set.empty())
        return "missing \"set\"";
    if (b.citation.empty())
        return "missing \"citation\"";
    if (!seen_epoch)
        return "missing \"epoch\"";
    if (!seen_equinox)
        return "missing \"equinox\"";
    if (seen_elements != 6)
        return "needs all six elements: \"M\", \"a\", \"e\", \"w\", \"node\", \"i\"";
    // At its own epoch the orbit must be a bound ellipse; later instants are
    // the engine's to judge, since T-terms can carry an element anywhere.
    if (!(el.semi_major_axis[0] > 0.0))
        return "\"a\" must be positive at the epoch";
    if (!(el.eccentricity[0] >= 0.0 && el.eccentricity[0] < 1.0))
        return "\"e\" must be in [0, 1) at the epoch";
    return {};
}

} // namespace

Result<std::vector<Body>> parse(std::string_view text, std::string_view origin) {
    std::vector<Body> out;
    size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.find_first_not_of(" \t\r") == std::string_view::npos)
            continue;
        const auto where = [&](const std::string& why) {
            return make_error(ErrorCode::FormatError,
                              std::string(origin) + ":" + std::to_string(line_no) + ": " + why);
        };
        Reader r(line);
        Value v;
        if (!r.value(v, 0))
            return where(r.why());
        if (!r.at_end())
            return where("one JSON object per line");
        Body b;
        if (std::string why = body_of(v, b); !why.empty())
            return where(why);
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace prometheia::hypotheticals
