// SPDX-License-Identifier: GPL-2.0-or-later
//
// JPL DE binary reader. See de.hpp for the format summary and docs/DE.md
// for the byte map, derivation and provenance.
#include "prometheia/de.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "prometheia/chebyshev.hpp"

namespace prometheia::de {
namespace {

// Header record 1 byte map (verified against DE200 and DE440): 3 x 84
// title bytes, 400 six-character name slots, then SS/FF/NN (start, end,
// interval) and the fields below.
constexpr size_t kTitleBytes = 3 * 84;
constexpr size_t kNameSlots = 400;
constexpr size_t kEpochOffset = kTitleBytes + kNameSlots * 6; // 2652

constexpr size_t kNconOffset = 2676;     // int32 NCON
constexpr size_t kAuOffset = 2680;       // f64 AU (km)
constexpr size_t kEmratOffset = 2688;    // f64 EMRAT
constexpr size_t kIptOffset = 2696;      // int32 x 36: columns 1-12
constexpr size_t kNumdeOffset = 2840;    // int32 NUMDE
constexpr size_t kLptOffset = 2844;      // int32 x 3: column 13
constexpr size_t kExtNamesOffset = 2856; // names 401..NCON, then columns 14, 15

constexpr int kMaxConstants = 4000;
constexpr size_t kPrefixBytes = kExtNamesOffset + (kMaxConstants - kNameSlots) * 6 + 24;

constexpr uint64_t bswap64(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
        r |= ((v >> (8 * i)) & 0xFF) << (8 * (7 - i));
    return r;
}

constexpr uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

struct ByteReader {
    const std::string& buf;
    bool swap;

    bool has(size_t off, size_t n) const { return off <= buf.size() && n <= buf.size() - off; }
    uint64_t u64(size_t off) const {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64_t(uint8_t(buf[off + size_t(i)])) << (8 * i);
        return swap ? bswap64(v) : v;
    }
    double f64(size_t off) const { return std::bit_cast<double>(u64(off)); }
    int32_t i32(size_t off) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(uint8_t(buf[off + size_t(i)])) << (8 * i);
        return int32_t(swap ? bswap32(v) : v);
    }
};

std::string trimmed(const char* p, size_t n) {
    size_t end = n;
    while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == '\0'))
        --end;
    return std::string(p, end);
}

uint64_t file_size(std::ifstream& f) {
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0);
    return size < 0 ? 0 : uint64_t(size);
}

bool read_at(std::ifstream& f, uint64_t off, std::string& buf, size_t n) {
    buf.assign(n, '\0');
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(buf.data(), std::streamsize(n));
    return uint64_t(f.gcount()) == n;
}

// SS/FF/NN sanity: returns the data-record count, or 0 when implausible.
uint64_t epoch_records(double ss, double ff, double nn) {
    if (!std::isfinite(ss) || !std::isfinite(ff) || !std::isfinite(nn))
        return 0;
    if (!(ff > ss) || !(nn > 0.0) || nn > 64.0)
        return 0;
    const double span = (ff - ss) / nn;
    if (span < 1.0 || span > 1e8 || std::fabs(span - std::round(span)) > 1e-6)
        return 0;
    return uint64_t(std::round(span));
}

bool present(const BodyLayout& b) {
    return b.offset > 0 && b.ncoeff > 0 && b.nsubint > 0;
}

// Last 1-based word a column occupies (0 when absent).
int64_t column_end(const BodyLayout& b, int column) {
    if (!present(b))
        return 0;
    return int64_t(b.offset) - 1 + int64_t(b.ncoeff) * b.nsubint * component_count(Body(column));
}

enum class Probe { NoMatch, Match, Truncated };

// Parses header record 1. Fills h (except the constant values, which
// need record 2) and returns Match on success.
Probe probe_header(const std::string& prefix, uint64_t size, bool swap, Header& h) {
    const ByteReader r{prefix, swap};
    if (!r.has(kExtNamesOffset, 24))
        return Probe::NoMatch;
    const uint64_t records =
        epoch_records(r.f64(kEpochOffset), r.f64(kEpochOffset + 8), r.f64(kEpochOffset + 16));
    if (records == 0)
        return Probe::NoMatch;
    const int32_t ncon = r.i32(kNconOffset);
    const int32_t numde = r.i32(kNumdeOffset);
    const double au = r.f64(kAuOffset);
    const double emrat = r.f64(kEmratOffset);
    if (ncon < 1 || ncon > kMaxConstants || numde < 1 || numde > 100000)
        return Probe::NoMatch;
    if (!(au > 1.0e8 && au < 2.0e8) || !(emrat > 70.0 && emrat < 90.0))
        return Probe::NoMatch;

    BodyLayout bodies[kColumnCount] = {};
    for (int c = 0; c < 12; ++c) {
        const size_t o = kIptOffset + size_t(c) * 12;
        bodies[c] = {r.i32(o), r.i32(o + 4), r.i32(o + 8)};
    }
    bodies[12] = {r.i32(kLptOffset), r.i32(kLptOffset + 4), r.i32(kLptOffset + 8)};
    const size_t ext = ncon > int32_t(kNameSlots) ? size_t(ncon) - kNameSlots : 0;
    const size_t tail = kExtNamesOffset + ext * 6;
    if (r.has(tail, 24)) {
        bodies[13] = {r.i32(tail), r.i32(tail + 4), r.i32(tail + 8)};
        bodies[14] = {r.i32(tail + 12), r.i32(tail + 16), r.i32(tail + 20)};
    }

    int64_t record_doubles = 0;
    for (int c = 0; c < kColumnCount; ++c) {
        BodyLayout& b = bodies[c];
        if (!present(b)) {
            b = {};
            continue;
        }
        if (b.offset < 3 || b.ncoeff > 64 || b.nsubint > 64)
            return Probe::NoMatch;
        record_doubles = std::max(record_doubles, column_end(b, c + 1));
    }
    // A planetary ephemeris without the Sun or the Earth-Moon barycentre
    // is not one we recognise.
    if (!present(bodies[2]) || !present(bodies[10]) || record_doubles > 100000)
        return Probe::NoMatch;
    const uint64_t rec_len = uint64_t(record_doubles) * 8;
    if (tail + 24 > rec_len || uint64_t(ncon) * 8 > rec_len)
        return Probe::NoMatch;
    if (size < 2 * rec_len)
        return Probe::NoMatch;
    if (size < (records + 2) * rec_len)
        return Probe::Truncated;

    h.byte_swapped = swap;
    h.title = trimmed(prefix.data(), kTitleBytes);
    h.start_jed = r.f64(kEpochOffset);
    h.end_jed = r.f64(kEpochOffset + 8);
    h.interval_days = r.f64(kEpochOffset + 16);
    h.denum = numde;
    h.record_doubles = int(record_doubles);
    h.record_count = records;
    h.au_km = au;
    h.emrat = emrat;
    std::copy(std::begin(bodies), std::end(bodies), std::begin(h.bodies));
    h.constant_names.clear();
    h.constant_names.reserve(size_t(ncon));
    for (int32_t i = 0; i < ncon; ++i) {
        const size_t off = size_t(i) < kNameSlots ? kTitleBytes + size_t(i) * 6
                                                  : kExtNamesOffset + (size_t(i) - kNameSlots) * 6;
        h.constant_names.push_back(trimmed(prefix.data() + off, 6));
    }
    return Probe::Match;
}

void subtract(double a[6], const double b[6]) {
    for (int i = 0; i < 6; ++i)
        a[i] -= b[i];
}

} // namespace

int component_count(Body body) {
    switch (body) {
    case Body::Nutations:
        return 2;
    case Body::TTminusTDB:
        return 1;
    default:
        return 3;
    }
}

bool DeFile::has_body(Body body) const {
    const int b = int(body);
    if (b < 1 || b > kColumnCount || header_.record_doubles == 0)
        return false;
    return present(header_.bodies[b - 1]);
}

std::optional<double> DeFile::constant(std::string_view name) const {
    for (size_t i = 0; i < header_.constant_names.size(); ++i) {
        if (header_.constant_names[i] == name)
            return header_.constant_values[i];
    }
    return std::nullopt;
}

Result<DeFile> DeFile::open(const std::string& path) {
    DeFile out;
    out.path_ = path;
    out.file_.open(path, std::ios::binary);
    if (!out.file_.is_open()) {
        return make_error(ErrorCode::IoError, "cannot open '" + path + "'");
    }
    const uint64_t size = file_size(out.file_);
    if (size < kEpochOffset + 24) {
        return make_error(ErrorCode::FormatError,
                          "file too small to be a DE binary: '" + path + "'");
    }
    std::string prefix;
    if (!read_at(out.file_, 0, prefix, size_t(std::min<uint64_t>(size, kPrefixBytes)))) {
        return make_error(ErrorCode::IoError, "cannot read header of '" + path + "'");
    }

    // Little-endian first, then byte-swapped.
    for (bool swap : {false, true}) {
        Header h;
        const Probe p = probe_header(prefix, size, swap, h);
        if (p == Probe::NoMatch)
            continue;
        const uint64_t rec_len = uint64_t(h.record_doubles) * 8;
        const size_t ncon = h.constant_names.size();

        // Header record 2: constant values. DENUM (when named) must agree
        // with the header's NUMDE.
        std::string rec2;
        if (!read_at(out.file_, rec_len, rec2, size_t(rec_len)))
            continue;
        const ByteReader r2{rec2, swap};
        h.constant_values.resize(ncon);
        for (size_t i = 0; i < ncon; ++i)
            h.constant_values[i] = r2.f64(i * 8);
        if (ncon > 0 && h.constant_names[0] == "DENUM" && h.constant_values[0] != double(h.denum)) {
            continue;
        }
        if (p == Probe::Truncated) {
            return make_error(ErrorCode::CorruptionError,
                              "DE binary truncated: header promises " +
                                  std::to_string(h.record_count) + " data records of " +
                                  std::to_string(rec_len) + " bytes, file is " +
                                  std::to_string(size) + " bytes: '" + path + "'");
        }

        // First data record must start at SS and span one interval.
        std::string first;
        if (!read_at(out.file_, 2 * rec_len, first, 16))
            continue;
        const ByteReader r3{first, swap};
        if (r3.f64(0) != h.start_jed || r3.f64(8) != h.start_jed + h.interval_days)
            continue;

        out.header_ = std::move(h);
        out.cache_.clear();
        out.cached_record_ = ~uint64_t{0};
        return out;
    }

    return make_error(ErrorCode::FormatError, "not a recognised JPL DE binary: '" + path + "'");
}

Result<void> DeFile::load_record(uint64_t index) const {
    if (cached_record_ == index)
        return {};

    const uint64_t rec_len = uint64_t(header_.record_doubles) * 8;
    // Straight into the cache, then swapped in place only when the file's
    // byte order is not the machine's: the same doubles the byte-by-byte
    // reader assembles, without the copy (a fresh instant loads a record).
    cache_.resize(size_t(header_.record_doubles));
    file_.clear();
    file_.seekg(std::streamoff((index + 2) * rec_len));
    file_.read(reinterpret_cast<char*>(cache_.data()), std::streamsize(rec_len));
    if (uint64_t(file_.gcount()) != rec_len) {
        cached_record_ = ~uint64_t{0};
        return make_error(ErrorCode::CorruptionError,
                          "truncated data record " + std::to_string(index) + " in '" + path_ + "'");
    }
    // The file is little-endian unless byte_swapped says it is big-endian.
    if ((std::endian::native == std::endian::little) == header_.byte_swapped) {
        for (double& d : cache_)
            d = std::bit_cast<double>(bswap64(std::bit_cast<uint64_t>(d)));
    }
    cached_record_ = index;
    return {};
}

Result<void> DeFile::state(Body body, double jed, double out[6]) const {
    if (header_.record_doubles == 0) {
        return make_error(ErrorCode::ArgumentError, "DeFile is not open");
    }
    const int b = int(body);
    if (b < 1 || b > kColumnCount) {
        return make_error(ErrorCode::ArgumentError,
                          "body index out of range: " + std::to_string(b));
    }
    const BodyLayout& layout = header_.bodies[b - 1];
    if (!present(layout)) {
        return make_error(ErrorCode::NotFound,
                          "body " + std::to_string(b) + " is not in this ephemeris");
    }
    if (!std::isfinite(jed)) {
        return make_error(ErrorCode::ArgumentError, "jed is not finite");
    }
    if (jed < header_.start_jed || jed > header_.end_jed) {
        return make_error(ErrorCode::ArgumentError, "jed " + std::to_string(jed) +
                                                        " outside coverage [" +
                                                        std::to_string(header_.start_jed) + ", " +
                                                        std::to_string(header_.end_jed) + "]");
    }

    // Record and subinterval selection. jed == end_jed lands on the final
    // record's last subinterval boundary.
    uint64_t j = uint64_t((jed - header_.start_jed) / header_.interval_days);
    if (j >= header_.record_count)
        j = header_.record_count - 1;
    Result<void> loaded = load_record(j);
    if (!loaded.ok())
        return loaded;

    const double rec_start = cache_[0];
    const double width = header_.interval_days / layout.nsubint;
    double s = std::floor((jed - rec_start) / width);
    if (s < 0.0)
        s = 0.0;
    if (s > double(layout.nsubint - 1))
        s = double(layout.nsubint - 1);
    const int sub = int(s);

    double tau = 2.0 * (jed - (rec_start + s * width)) / width - 1.0;
    tau = std::max(-1.0, std::min(1.0, tau));

    // Component c of subinterval sub occupies ncoeff doubles at
    // (offset - 1) + sub*ncomp*ncoeff + c*ncoeff (0-based, after the epochs).
    const int n = layout.ncoeff;
    const int ncomp = component_count(body);
    const size_t base = size_t(layout.offset - 1) + size_t(sub) * size_t(ncomp * n);
    const double scale = 2.0 / width; // dtau/dt
    if (ncomp == 3) {
        // A position: one set of recurrences for x, y, z.
        double value[3], deriv[3];
        chebyshev_eval3(&cache_[base], n, tau, value, deriv);
        for (int c = 0; c < 3; ++c) {
            out[c] = value[c];
            out[3 + c] = deriv[c] * scale;
        }
        return {};
    }
    for (int c = 0; c < 3; ++c) {
        if (c >= ncomp) {
            out[c] = 0.0;
            out[3 + c] = 0.0;
            continue;
        }
        double value = 0.0, deriv = 0.0;
        chebyshev_eval(&cache_[base + size_t(c) * size_t(n)], n, tau, value, deriv);
        out[c] = value;
        out[3 + c] = deriv * scale;
    }
    return {};
}

Result<void> DeFile::barycentric(Target target, double jed, double out[6]) const {
    switch (target) {
    case Target::SolarSystemBary:
        std::fill(out, out + 6, 0.0);
        return {};
    case Target::EarthMoonBary:
        return state(Body::EarthMoonBary, jed, out);
    case Target::Earth:
    case Target::Moon: {
        double moon[6];
        Result<void> r = state(Body::EarthMoonBary, jed, out);
        if (!r.ok())
            return r;
        r = state(Body::Moon, jed, moon);
        if (!r.ok())
            return r;
        const double k = 1.0 / (1.0 + header_.emrat);
        for (int i = 0; i < 6; ++i) {
            out[i] -= moon[i] * k; // Earth
            if (target == Target::Moon)
                out[i] += moon[i];
        }
        return {};
    }
    default: {
        const int t = int(target);
        if (t < 1 || t > 11) {
            return make_error(ErrorCode::ArgumentError,
                              "target out of range: " + std::to_string(t));
        }
        return state(Body(t), jed, out);
    }
    }
}

Result<void> DeFile::relative_state(Target target, Target center, double jed, double out[6]) const {
    if (target == center) {
        // Still validate the query (coverage, bodies present).
        Result<void> r = barycentric(target, jed, out);
        if (!r.ok())
            return r;
        std::fill(out, out + 6, 0.0);
        return {};
    }
    // Earth-Moon pairs straight from the geocentric Moon column: no
    // cancellation through barycentric magnitudes.
    const bool em_pair =
        (target == Target::Moon || target == Target::Earth || target == Target::EarthMoonBary) &&
        (center == Target::Moon || center == Target::Earth || center == Target::EarthMoonBary);
    if (em_pair) {
        double moon[6];
        Result<void> r = state(Body::Moon, jed, moon);
        if (!r.ok())
            return r;
        const double k = 1.0 / (1.0 + header_.emrat);
        // Offsets from the Earth-Moon barycentre along the geocentric Moon.
        const auto from_emb = [&](Target t) {
            return t == Target::Moon ? 1.0 - k : (t == Target::Earth ? -k : 0.0);
        };
        const double f = from_emb(target) - from_emb(center);
        for (int i = 0; i < 6; ++i)
            out[i] = moon[i] * f;
        return {};
    }
    double c[6];
    Result<void> r = barycentric(target, jed, out);
    if (!r.ok())
        return r;
    r = barycentric(center, jed, c);
    if (!r.ok())
        return r;
    subtract(out, c);
    return {};
}

} // namespace prometheia::de
