// SPDX-License-Identifier: GPL-2.0-or-later
//
// SPK (DAF) reader tests.
//
// Part A writes synthetic DAF/SPK files in both byte orders with type 2
// and type 3 segments, an overriding later segment and an unsupported
// segment type, and checks evaluation, segment chaining and precedence
// exactly (runs everywhere, CI included).
//
// Part B opens JPL's de440s.bsp (PROMETHEIA_DE440S, default
// ephe/de440s.bsp) and cross-checks it against the DE440 binary
// (PROMETHEIA_DE440): the same ephemeris in two containers must agree.
// Those cases print SKIP and pass when a file is absent.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <prometheia/de.hpp>
#include <prometheia/spk.hpp>

#include "test_main.hpp"

using namespace prometheia;
using prometheia::spk::SpkFile;
namespace naif = prometheia::spk::naif;

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Synthetic DAF/SPK writer.
// ---------------------------------------------------------------------------

constexpr double kDay = 86400.0;
constexpr double kInit = 1.0e8; // TDB seconds past J2000 (~2003)
constexpr double kInterval = 4.0 * kDay;
constexpr int kRecords = 3;

struct SynSegment {
    int target, center, type, degree;
    double init, interval;
    int records;
    double offset; // added to every constant term, distinguishes segments
    std::string name;
};

// Coefficient k of component c in record i of segment s.
double syn_coeff(const SynSegment& s, int i, int c, int k) {
    const double base = s.offset + 1000.0 * s.target + 100.0 * i + 10.0 * c;
    switch (k) {
    case 0:
        return base;
    case 1:
        return 50.0 + 3.0 * c + i;
    case 2:
        return 0.25 * (c + 1);
    case 3:
        return -0.125 * (i + 1);
    default:
        return 0.0;
    }
}

double cheb_value(const double cs[4], int n, double t) {
    const double T[4] = {1.0, t, 2.0 * t * t - 1.0, 4.0 * t * t * t - 3.0 * t};
    double v = 0.0;
    for (int k = 0; k < n; ++k)
        v += cs[k] * T[k];
    return v;
}

double cheb_deriv(const double cs[4], int n, double t) {
    const double dT[4] = {0.0, 1.0, 4.0 * t, 12.0 * t * t - 3.0};
    double v = 0.0;
    for (int k = 0; k < n; ++k)
        v += cs[k] * dT[k];
    return v;
}

// Expected state (km, km/day), independent of the reader's internals.
void syn_expected(const SynSegment& s, double et, double out[6]) {
    int i = int(std::floor((et - s.init) / s.interval));
    i = std::max(0, std::min(s.records - 1, i));
    const double mid = s.init + (i + 0.5) * s.interval;
    const double radius = 0.5 * s.interval;
    const double t = (et - mid) / radius;
    const int n = s.degree + 1;
    for (int c = 0; c < 3; ++c) {
        double cs[4] = {};
        for (int k = 0; k < n; ++k)
            cs[k] = syn_coeff(s, i, c, k);
        out[c] = cheb_value(cs, n, t);
        if (s.type == 2) {
            out[3 + c] = cheb_deriv(cs, n, t) / radius * kDay;
        } else {
            double vs[4] = {};
            for (int k = 0; k < n; ++k)
                vs[k] = syn_coeff(s, i, 3 + c, k) * 1e-3; // km/s-scale velocities
            out[3 + c] = cheb_value(vs, n, t) * kDay;
        }
    }
}

struct Bytes {
    bool swap = false;
    std::string out;

    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            out.push_back(char((v >> (swap ? 8 * (7 - i) : 8 * i)) & 0xFF));
    }
    void f64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }
    void i32(int32_t v) {
        const uint32_t u = uint32_t(v);
        for (int i = 0; i < 4; ++i)
            out.push_back(char((u >> (swap ? 8 * (3 - i) : 8 * i)) & 0xFF));
    }
    void text(std::string s, size_t n, char fill = ' ') {
        s.resize(n, fill);
        out += s;
    }
    void pad_to(size_t n) { out.resize(n, '\0'); }
};

std::vector<SynSegment> syn_segments() {
    return {
        {naif::kEarthMoonBary, naif::kSolarSystemBary, 2, 3, kInit, kInterval, kRecords, 0.0,
         "EMB wrt SSB"},
        {naif::kMoon, naif::kEarthMoonBary, 3, 2, kInit, kInterval, kRecords, 0.0,
         "MOON wrt EMB (type 3)"},
        {naif::kEarth, naif::kEarthMoonBary, 2, 3, kInit, kInterval, kRecords, 0.0,
         "EARTH wrt EMB"},
        // Later segment for the same body over the middle record only: it
        // must take precedence there.
        {naif::kEarthMoonBary, naif::kSolarSystemBary, 2, 1, kInit + kInterval, kInterval, 1, 1.0e6,
         "EMB override"},
        // Unsupported type (1): listed, not evaluable.
        {499, 4, 1, 0, kInit, kInterval, 1, 0.0, "MARS type 1"},
    };
}

std::string write_spk(const fs::path& path, bool swap, bool damage_ftp = false) {
    const std::vector<SynSegment> segs = syn_segments();
    Bytes data; // segment arrays, starting at record 4 (word 385)
    data.swap = swap;
    std::vector<std::pair<int, int>> addresses;
    constexpr int kFirstWord = 3 * 1024 / 8 + 1;
    for (const SynSegment& s : segs) {
        const int begin = kFirstWord + int(data.out.size() / 8);
        if (s.type == 1) {
            for (int i = 0; i < 5; ++i)
                data.f64(0.0);
        } else {
            const int comps = s.type == 2 ? 3 : 6;
            const int n = s.degree + 1;
            for (int i = 0; i < s.records; ++i) {
                data.f64(s.init + (i + 0.5) * s.interval);
                data.f64(0.5 * s.interval);
                for (int c = 0; c < comps; ++c) {
                    for (int k = 0; k < n; ++k)
                        data.f64(syn_coeff(s, i, c, k) * (c >= 3 ? 1e-3 : 1.0));
                }
            }
            data.f64(s.init);
            data.f64(s.interval);
            data.f64(2.0 + comps * n);
            data.f64(s.records);
        }
        const int end = kFirstWord + int(data.out.size() / 8) - 1;
        addresses.push_back({begin, end});
    }

    Bytes f;
    f.swap = swap;
    // Record 1: file record.
    f.text("DAF/SPK ", 8);
    f.i32(2);
    f.i32(6);
    f.text("PROMETHEIA SYNTHETIC SPK", 60);
    f.i32(2);                                     // FWARD
    f.i32(2);                                     // BWARD
    f.i32(kFirstWord + int(data.out.size() / 8)); // FREE
    f.text(swap ? "BIG-IEEE" : "LTL-IEEE", 8);
    f.pad_to(699);
    const char ftp[] = "FTPSTR:\r:\n:\r\n:\r\0:\x81:\x10\xce:ENDFTP";
    f.out.append(ftp, sizeof(ftp) - 1);
    if (damage_ftp)
        f.out[699 + 8] = '\n'; // what an ASCII-mode transfer does to "\r"
    f.pad_to(1024);
    // Record 2: one summary record.
    f.f64(0.0);
    f.f64(0.0);
    f.f64(double(segs.size()));
    for (size_t k = 0; k < segs.size(); ++k) {
        const SynSegment& s = segs[k];
        f.f64(s.init);
        f.f64(s.init + s.records * s.interval);
        f.i32(s.target);
        f.i32(s.center);
        f.i32(1);
        f.i32(s.type);
        f.i32(addresses[k].first);
        f.i32(addresses[k].second);
    }
    f.pad_to(2048);
    // Record 3: segment names, 40 characters each.
    for (const SynSegment& s : segs)
        f.text(s.name, 40);
    f.pad_to(3072);
    f.out += data.out;
    f.pad_to((f.out.size() + 1023) / 1024 * 1024);

    fs::create_directories(path.parent_path());
    FILE* fp = std::fopen(path.string().c_str(), "wb");
    CHECK(fp != nullptr);
    std::fwrite(f.out.data(), 1, f.out.size(), fp);
    std::fclose(fp);
    return path.string();
}

fs::path temp_dir() {
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() /
        ("prometheia-test-spk-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    fs::create_directories(dir);
    return dir;
}

double jd_of(double et) {
    return 2451545.0 + et / kDay;
}

double max_abs_diff(const double a[6], const double b[6]) {
    double m = 0.0;
    for (int i = 0; i < 6; ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

SpkFile open_ok(const std::string& path) {
    auto opened = SpkFile::open(path);
    CHECK(opened.ok());
    if (!opened.ok())
        std::printf("  open failed: %s\n", opened.error().message.c_str());
    return std::move(opened.value());
}

// ---------------------------------------------------------------------------
// Part A: synthetic files.
// ---------------------------------------------------------------------------

void check_synthetic(bool swap) {
    const fs::path dir = temp_dir();
    SpkFile f = open_ok(write_spk(dir / "syn.bsp", swap));
    const std::vector<SynSegment> segs = syn_segments();
    CHECK(f.byte_swapped() == swap);
    CHECK(f.internal_name() == "PROMETHEIA SYNTHETIC SPK");
    CHECK(f.segments().size() == segs.size());
    for (size_t k = 0; k < segs.size() && k < f.segments().size(); ++k) {
        const spk::Segment& g = f.segments()[k];
        CHECK(g.name == segs[k].name);
        CHECK(g.target == segs[k].target && g.center == segs[k].center);
        CHECK(g.type == segs[k].type && g.frame == 1);
        CHECK(g.start_et == segs[k].init);
        CHECK(g.end_et == segs[k].init + segs[k].records * segs[k].interval);
        if (g.type != 1) {
            CHECK(g.degree == segs[k].degree);
            CHECK(g.record_count == uint64_t(segs[k].records));
            CHECK(g.interval_s == segs[k].interval);
        }
    }

    // Direct segment evaluation across record boundaries and the end.
    double worst = 0.0;
    for (size_t k = 0; k < 4; ++k) {
        const SynSegment& s = segs[k];
        for (double frac : {0.0, 0.1, 0.9999, 1.0, 1.5, 2.0, 2.75, 3.0}) {
            if (frac > s.records)
                continue;
            const double et = s.init + frac * s.interval;
            double got[6], want[6];
            CHECK(f.segment_state_et(k, et, got).ok());
            syn_expected(s, et, want);
            worst = std::max(worst, max_abs_diff(got, want));
        }
    }
    std::printf("  worst synthetic segment |delta| = %.3e\n", worst);
    CHECK(worst < 1e-6);

    // Chaining through the common ancestor, and precedence.
    const auto seg_state = [&](size_t k, double jd, double out[6]) {
        syn_expected(segs[k], SpkFile::et_from_jd(jd), out);
    };
    for (double frac : {0.5, 1.5, 2.5}) {
        const double jd = jd_of(kInit + frac * kInterval);
        double moon_emb[6], earth_emb[6], emb[6], got[6], want[6];
        seg_state(1, jd, moon_emb);
        seg_state(2, jd, earth_emb);
        const bool overridden = frac > 1.0 && frac < 2.0;
        seg_state(overridden ? 3 : 0, jd, emb);

        CHECK(f.state(naif::kMoon, naif::kEarth, jd, got).ok());
        for (int i = 0; i < 6; ++i)
            want[i] = moon_emb[i] - earth_emb[i];
        CHECK(max_abs_diff(got, want) < 1e-6);

        CHECK(f.state(naif::kEarth, naif::kSolarSystemBary, jd, got).ok());
        for (int i = 0; i < 6; ++i)
            want[i] = earth_emb[i] + emb[i];
        CHECK(max_abs_diff(got, want) < 1e-6);

        CHECK(f.state(naif::kSolarSystemBary, naif::kMoon, jd, got).ok());
        for (int i = 0; i < 6; ++i)
            want[i] = -(moon_emb[i] + emb[i]);
        CHECK(max_abs_diff(got, want) < 1e-6);

        CHECK(f.state(naif::kEarth, naif::kEarth, jd, got).ok());
        for (int i = 0; i < 6; ++i)
            CHECK(got[i] == 0.0);
    }
}

TEST(spk_synthetic_little_endian) {
    check_synthetic(false);
}

TEST(spk_synthetic_big_endian) {
    check_synthetic(true);
}

TEST(spk_synthetic_errors) {
    const fs::path dir = temp_dir();
    const std::string good = write_spk(dir / "syn.bsp", false);
    { // missing file
        auto r = SpkFile::open((dir / "nope.bsp").string());
        CHECK(!r.ok() && r.error().code == ErrorCode::IoError);
    }
    { // not a DAF/SPK
        FILE* fp = std::fopen((dir / "junk.bsp").string().c_str(), "wb");
        const std::string junk(2048, 'x');
        std::fwrite(junk.data(), 1, junk.size(), fp);
        std::fclose(fp);
        auto r = SpkFile::open((dir / "junk.bsp").string());
        CHECK(!r.ok() && r.error().code == ErrorCode::FormatError);
    }
    { // ASCII-mode transfer damage
        auto r = SpkFile::open(write_spk(dir / "ftp.bsp", false, true));
        CHECK(!r.ok() && r.error().code == ErrorCode::CorruptionError);
    }
    { // truncated: segment data cut off
        const std::string path = (dir / "trunc.bsp").string();
        fs::copy_file(good, path);
        fs::resize_file(path, 3072 + 64);
        auto r = SpkFile::open(path);
        CHECK(!r.ok() && r.error().code == ErrorCode::CorruptionError);
    }
    SpkFile f = open_ok(good);
    double out[6];
    const double inside = jd_of(kInit + 0.5 * kInterval);
    { // unsupported type
        auto r = f.segment_state(4, inside, out);
        CHECK(!r.ok() && r.error().code == ErrorCode::FormatError);
        auto chained = f.state(499, naif::kSolarSystemBary, inside, out);
        CHECK(!chained.ok() && chained.error().code == ErrorCode::FormatError);
    }
    { // coverage
        auto r = f.segment_state(0, jd_of(kInit - kDay), out);
        CHECK(!r.ok() && r.error().code == ErrorCode::ArgumentError);
        auto chained = f.state(naif::kMoon, naif::kSolarSystemBary, jd_of(kInit - kDay), out);
        CHECK(!chained.ok() && chained.error().code == ErrorCode::ArgumentError);
        auto nan = f.state(naif::kMoon, naif::kEarth, std::nan(""), out);
        CHECK(!nan.ok() && nan.error().code == ErrorCode::ArgumentError);
    }
    { // the exact start epoch is in coverage through the et entry point
        CHECK(f.segment_state_et(0, kInit, out).ok());
        CHECK(f.state_et(naif::kMoon, naif::kSolarSystemBary, kInit, out).ok());
    }
    { // no path between bodies
        auto r = f.state(12345, naif::kEarth, inside, out);
        CHECK(!r.ok() && r.error().code == ErrorCode::NotFound);
        auto bad = f.segment_state(99, inside, out);
        CHECK(!bad.ok() && bad.error().code == ErrorCode::ArgumentError);
    }
}

// ---------------------------------------------------------------------------
// Part B: de440s.bsp against the DE440 binary.
// ---------------------------------------------------------------------------

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return env && *env ? std::string(env) : fallback;
}

bool available(const std::string& path, const char* var) {
    if (fs::exists(path))
        return true;
    std::printf("  [SKIP] '%s' not found (set %s to enable)\n", path.c_str(), var);
    return false;
}

const std::string kDe440sPath =
    env_or("PROMETHEIA_DE440S", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/de440s.bsp");
const std::string kDe440Path =
    env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");

TEST(de440s_real_segments) {
    if (!available(kDe440sPath, "PROMETHEIA_DE440S"))
        return;
    SpkFile f = open_ok(kDe440sPath);
    CHECK(!f.byte_swapped());
    CHECK(f.internal_name() == "NIO2SPK");
    const auto& segs = f.segments();
    CHECK(segs.size() == 14);
    const int targets[14][2] = {{1, 0}, {2, 0}, {3, 0},  {4, 0},   {5, 0},   {6, 0},   {7, 0},
                                {8, 0}, {9, 0}, {10, 0}, {301, 3}, {399, 3}, {199, 1}, {299, 2}};
    for (size_t k = 0; k < segs.size() && k < 14; ++k) {
        CHECK(segs[k].target == targets[k][0] && segs[k].center == targets[k][1]);
        CHECK(segs[k].type == 2 && segs[k].frame == 1);
        CHECK(segs[k].name == "DE-0440LE-0440");
        CHECK(segs[k].start_et == -4734072000.0 && segs[k].end_et == 4735368000.0);
    }
    // Mercury barycentre: degree 13 over 8 days; EMB 12 over 16; Moon 12 over 4.
    CHECK(segs[0].degree == 13 && segs[0].interval_s == 8 * 86400.0);
    CHECK(segs[2].degree == 12 && segs[2].interval_s == 16 * 86400.0);
    CHECK(segs[10].degree == 12 && segs[10].interval_s == 4 * 86400.0);
}

// The same DE440 data in two containers: positions agree to the fitting
// noise of JPL's re-packaging, far below a metre.
TEST(de440s_real_matches_de440_binary) {
    if (!available(kDe440sPath, "PROMETHEIA_DE440S") || !available(kDe440Path, "PROMETHEIA_DE440"))
        return;
    SpkFile s = open_ok(kDe440sPath);
    auto opened = de::DeFile::open(kDe440Path);
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    const de::DeFile d = std::move(opened.value());

    using de::Target;
    const struct {
        Target de_target, de_center;
        int naif_target, naif_center;
    } pairs[] = {
        {Target::Moon, Target::Earth, naif::kMoon, naif::kEarth},
        {Target::Earth, Target::Sun, naif::kEarth, naif::kSun},
        {Target::Mercury, Target::Earth, naif::kMercuryBary, naif::kEarth},
        {Target::Venus, Target::SolarSystemBary, naif::kVenusBary, naif::kSolarSystemBary},
        {Target::Mars, Target::Earth, naif::kMarsBary, naif::kEarth},
        {Target::Jupiter, Target::Sun, naif::kJupiterBary, naif::kSun},
        {Target::Pluto, Target::EarthMoonBary, naif::kPlutoBary, naif::kEarthMoonBary},
        {Target::Sun, Target::SolarSystemBary, naif::kSun, naif::kSolarSystemBary},
    };
    double worst_pos = 0.0, worst_vel = 0.0;
    long n = 0;
    // 1850..2149, a prime step so epochs land at arbitrary record phases.
    for (double jd = 2396770.5; jd < 2506300.0; jd += 97.3137) {
        for (const auto& p : pairs) {
            double a[6], b[6];
            CHECK(d.relative_state(p.de_target, p.de_center, jd, a).ok());
            CHECK(s.state(p.naif_target, p.naif_center, jd, b).ok());
            for (int i = 0; i < 3; ++i) {
                worst_pos = std::max(worst_pos, std::fabs(a[i] - b[i]));
                worst_vel = std::max(worst_vel, std::fabs(a[3 + i] - b[3 + i]));
            }
            ++n;
        }
    }
    std::printf("  de440s.bsp vs DE440 binary: %ld states, worst |dpos| %.3e km, "
                "|dvel| %.3e km/day\n",
                n, worst_pos, worst_vel);
    CHECK(n > 3000);
    CHECK(worst_pos < 1e-3);
    CHECK(worst_vel < 1e-3);
}

} // namespace

int main() {
    return ptest::run_all();
}
