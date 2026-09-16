// SPDX-License-Identifier: GPL-2.0-or-later
//
// Old-format JPL DE binary reader. See de.hpp for the format summary
// and docs/DE.md for the full derivation and provenance.
#include "prometheia/de.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "prometheia/varint.hpp"

namespace prometheia::de {
namespace {

// DE200 pointer table from JPL's public ASCII header.200 (GROUP 1050),
// fetched from ssd.jpl.nasa.gov/ftp/eph/planets/ascii/de200/. Rows in
// order: Mercury, Venus, Earth-Moon barycentre, Mars, Jupiter, Saturn,
// Uranus, Neptune, Pluto, Moon (geocentric), Sun. The republished Linux
// binary has no lunar-libration block: its records are NCOEFF=826 f64
// words and the Sun block ends at slot 792, the rest is padding.
constexpr OldFormatSpec kDe200 = {
    200, // denum
    826, // record_doubles (KSIZE 1652 words of 4 bytes)
    200, // constant_count (NVALS)
    400, // name_slot_count
    {
        {3, 12, 4},   // Mercury
        {147, 12, 1}, // Venus
        {183, 15, 2}, // Earth-Moon barycentre
        {273, 10, 1}, // Mars
        {303, 9, 1},  // Jupiter
        {330, 8, 1},  // Saturn
        {354, 8, 1},  // Uranus
        {378, 6, 1},  // Neptune
        {396, 6, 1},  // Pluto
        {414, 12, 8}, // Moon (geocentric)
        {702, 15, 1}, // Sun
        {},           // Libration: absent from this binary
    },
    "JPL DE200 header.200 GROUP 1050 (ssd.jpl.nasa.gov, public domain)",
};

constexpr OldFormatSpec kSpecs[] = {kDe200};

constexpr int kTitleBytes = 3 * 84; // three 84-char header lines

// Chebyshev value and d/dtau derivative of sum(cs[k] * T_k(tau)).
// T_0 = 1, T_1 = tau, T_k = 2*tau*T_{k-1} - T_{k-2};
// T'_k = k * U_{k-1} with the same recurrence for U.
void chebyshev_eval(const double* cs, int n, double tau, double& value, double& deriv) {
    if (n <= 0) {
        value = 0.0;
        deriv = 0.0;
        return;
    }
    if (n == 1) {
        value = cs[0];
        deriv = 0.0;
        return;
    }
    double t_km2 = 1.0, t_km1 = tau;
    double u_km2 = 1.0, u_km1 = 2.0 * tau;
    value = cs[0] + cs[1] * tau;
    deriv = cs[1];
    for (int k = 2; k < n; ++k) {
        const double t_k = 2.0 * tau * t_km1 - t_km2;
        const double u_k = 2.0 * tau * u_km1 - u_km2;
        value += cs[k] * t_k;
        deriv += cs[k] * double(k) * u_km1;
        t_km2 = t_km1;
        t_km1 = t_k;
        u_km2 = u_km1;
        u_km1 = u_k;
    }
}

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

} // namespace

const OldFormatSpec* old_format_spec(int denum) {
    for (const OldFormatSpec& s : kSpecs) {
        if (s.denum == denum)
            return &s;
    }
    return nullptr;
}

bool DeFile::has_body(Body body) const {
    const int b = int(body);
    if (b < 1 || b > 12 || spec_ == nullptr)
        return false;
    return spec_->bodies[b - 1].offset > 0;
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
    if (size < 4 * 8) {
        return make_error(ErrorCode::FormatError,
                          "file too small to be a DE binary: '" + path + "'");
    }

    // The record length is not self-described in the old format: try each
    // built-in spec and accept the one whose geometry checks out.
    for (const OldFormatSpec& spec : kSpecs) {
        const uint64_t rec_len = uint64_t(spec.record_doubles) * 8;
        if (2 * rec_len > size)
            continue;

        // Header record 1: title, constant-name slots, then SS/FF/NN.
        std::string rec1(rec_len, '\0');
        out.file_.seekg(0);
        out.file_.read(rec1.data(), std::streamsize(rec_len));
        if (uint64_t(out.file_.gcount()) != rec_len)
            continue;
        const size_t epoch_off = size_t(kTitleBytes) + size_t(spec.name_slot_count) * 6;
        if (epoch_off + 24 > rec_len)
            continue;
        double ss = 0.0, ff = 0.0, nn = 0.0;
        const char* rec1_end = rec1.data() + rec_len;
        const char* q = get_f64(rec1.data() + epoch_off, rec1_end, ss);
        if (q == nullptr)
            continue;
        q = get_f64(q, rec1_end, ff);
        if (q == nullptr)
            continue;
        q = get_f64(q, rec1_end, nn);
        if (q == nullptr)
            continue;
        if (!std::isfinite(ss) || !std::isfinite(ff) || !std::isfinite(nn))
            continue;
        if (!(ff > ss) || !(nn > 0.0) || nn > 64.0)
            continue;
        const double span = (ff - ss) / nn;
        if (span < 1.0 || std::fabs(span - std::round(span)) > 1e-6)
            continue;
        const uint64_t records = uint64_t(std::round(span));
        if (size < (records + 2) * rec_len) {
            return make_error(ErrorCode::CorruptionError,
                              "DE binary truncated: header promises " + std::to_string(records) +
                                  " data records of " + std::to_string(rec_len) +
                                  " bytes, file is " + std::to_string(size) + " bytes: '" + path +
                                  "'");
        }

        // Header record 2: constant values, DENUM first.
        std::string rec2(rec_len, '\0');
        out.file_.seekg(std::streamoff(rec_len));
        out.file_.read(rec2.data(), std::streamsize(rec_len));
        if (uint64_t(out.file_.gcount()) != rec_len)
            continue;
        double denum_val = 0.0;
        if (!get_f64(rec2.data(), rec2.data() + 8, denum_val))
            continue;
        if (std::fabs(denum_val - std::round(denum_val)) > 1e-9)
            continue;
        if (int(std::round(denum_val)) != spec.denum)
            continue;

        // Geometry and identity match this spec: fill in the header.
        out.spec_ = &spec;
        out.header_.title = trimmed(rec1.data(), kTitleBytes);
        out.header_.start_jed = ss;
        out.header_.end_jed = ff;
        out.header_.interval_days = nn;
        out.header_.denum = spec.denum;
        out.header_.record_count = records;
        out.header_.constant_names.clear();
        out.header_.constant_values.clear();
        out.header_.constant_names.reserve(size_t(spec.constant_count));
        const char* names = rec1.data() + kTitleBytes;
        for (int i = 0; i < spec.constant_count; ++i) {
            out.header_.constant_names.push_back(trimmed(names + size_t(i) * 6, 6));
        }
        out.header_.constant_values.resize(size_t(spec.constant_count));
        const char* vp = rec2.data();
        const char* vend = rec2.data() + rec_len;
        for (int i = 0; i < spec.constant_count; ++i) {
            if (!get_f64(vp, vend, out.header_.constant_values[size_t(i)])) {
                return make_error(ErrorCode::FormatError,
                                  "constant values truncated in '" + path + "'");
            }
            vp += 8;
        }
        out.cache_.clear();
        out.cached_record_ = ~uint64_t{0};
        return out;
    }

    return make_error(ErrorCode::FormatError,
                      "not a supported old-format DE binary: '" + path +
                          "' (known old-format DENUMs: 200; modern-format DE405+ reader pending)");
}

Result<void> DeFile::load_record(uint64_t index) const {
    if (cached_record_ == index)
        return {};

    const uint64_t rec_len = uint64_t(spec_->record_doubles) * 8;
    std::string buf(rec_len, '\0');
    file_.seekg(std::streamoff((index + 2) * rec_len));
    file_.read(buf.data(), std::streamsize(rec_len));
    if (uint64_t(file_.gcount()) != rec_len) {
        cached_record_ = ~uint64_t{0};
        return make_error(ErrorCode::CorruptionError,
                          "truncated data record " + std::to_string(index) + " in '" + path_ + "'");
    }
    cache_.resize(size_t(spec_->record_doubles));
    const char* p = buf.data();
    const char* end = buf.data() + rec_len;
    for (int i = 0; i < spec_->record_doubles; ++i) {
        if (!get_f64(p, end, cache_[size_t(i)])) {
            return make_error(ErrorCode::CorruptionError,
                              "short decode of record " + std::to_string(index));
        }
        p += 8;
    }
    cached_record_ = index;
    return {};
}

Result<void> DeFile::state(Body body, double jed, double out[6]) const {
    if (spec_ == nullptr) {
        return make_error(ErrorCode::ArgumentError, "DeFile is not open");
    }
    const int b = int(body);
    if (b < 1 || b > 12) {
        return make_error(ErrorCode::ArgumentError,
                          "body index out of range: " + std::to_string(b));
    }
    const BodyLayout& layout = spec_->bodies[b - 1];
    if (layout.offset <= 0) {
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
    // (offset - 1) + sub*3*ncoeff + c*ncoeff (0-based, after the epochs).
    const int n = layout.ncoeff;
    const size_t base = size_t(layout.offset - 1) + size_t(sub) * size_t(3 * n);
    const double scale = 2.0 / width; // dtau/dt
    for (int c = 0; c < 3; ++c) {
        double value = 0.0, deriv = 0.0;
        chebyshev_eval(&cache_[base + size_t(c) * size_t(n)], n, tau, value, deriv);
        out[c] = value;
        out[3 + c] = deriv * scale;
    }
    return {};
}

} // namespace prometheia::de