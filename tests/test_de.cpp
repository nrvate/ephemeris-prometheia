// SPDX-License-Identifier: GPL-2.0-or-later
//
// DE binary reader tests.
//
// Part A builds synthetic files with known analytic Chebyshev fields and
// checks the reader exactly (runs everywhere, CI included): both byte
// orders, more than 400 constants and a TT-TDB column.
//
// Part B validates real JPL binaries when present on this machine; those
// cases print SKIP and pass when a file is absent:
//  - DE200 (PROMETHEIA_DE200, default /shares/swisseph/ephe/de200.eph);
//  - DE440 (PROMETHEIA_DE440, default ephe/linux_p1550p2650.440) against
//    JPL's own test points (PROMETHEIA_TESTPO440, default ephe/testpo.440).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <prometheia/de.hpp>
#include <prometheia/frames.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
using prometheia::de::Body;
using prometheia::de::DeFile;
using prometheia::de::Target;

namespace {

namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;
constexpr double kArcsec = kPi / (180.0 * 3600.0);

// ---------------------------------------------------------------------------
// Synthetic fields and writers.
// ---------------------------------------------------------------------------

constexpr double kSynSS = 2451545.0;
constexpr double kSynNN = 32.0;
constexpr int kSynRecords = 3;
constexpr double kSynAU = 149597870.7;
constexpr double kSynEmrat = 81.3;

// f(tau) = A + B*tau + C*(2*tau^2-1) + D*(4*tau^3-3*tau); higher k zero.
double coeff(int body, int comp, int k) {
    switch (k) {
    case 0:
        return 1e5 * body + 1e3 * comp + 7.0;
    case 1:
        return 30.0 * body + 2.0 * comp + 1.0;
    case 2:
        return 0.5 * body + 0.05 * comp;
    case 3:
        return 0.01 * body + 0.001 * comp;
    default:
        return 0.0;
    }
}

double expected_value(int body, int comp, double tau) {
    const double a = coeff(body, comp, 0), b = coeff(body, comp, 1);
    const double c = coeff(body, comp, 2), d = coeff(body, comp, 3);
    return a + b * tau + c * (2.0 * tau * tau - 1.0) + d * (4.0 * tau * tau * tau - 3.0 * tau);
}

double expected_deriv(int body, int comp, double tau) {
    const double b = coeff(body, comp, 1);
    const double c = coeff(body, comp, 2), d = coeff(body, comp, 3);
    return b + 4.0 * c * tau + d * (12.0 * tau * tau - 3.0);
}

struct Writer {
    bool swap = false;
    std::string out;

    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            const int shift = swap ? 8 * (7 - i) : 8 * i;
            out.push_back(char((v >> shift) & 0xFF));
        }
    }
    void f64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }
    void i32(int32_t v) {
        const uint32_t u = uint32_t(v);
        for (int i = 0; i < 4; ++i) {
            const int shift = swap ? 8 * (3 - i) : 8 * i;
            out.push_back(char((u >> shift) & 0xFF));
        }
    }
    void text(std::string s, size_t n) {
        s.resize(n, ' ');
        out += s;
    }
    void pad_to(size_t n) { out.resize(n, '\0'); }
};

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    FILE* fp = std::fopen(path.string().c_str(), "wb");
    CHECK(fp != nullptr);
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    CHECK(written == bytes.size());
}

// Data records for a layout: every present column carries coeff().
void write_data(Writer& w, const de::BodyLayout* bodies, int record_doubles) {
    std::vector<double> rec(size_t(record_doubles), 0.0);
    for (int j = 0; j < kSynRecords; ++j) {
        std::fill(rec.begin(), rec.end(), 0.0);
        rec[0] = kSynSS + kSynNN * j;
        rec[1] = kSynSS + kSynNN * (j + 1);
        for (int body = 1; body <= de::kColumnCount; ++body) {
            const de::BodyLayout& lay = bodies[body - 1];
            if (lay.offset <= 0 || lay.ncoeff <= 0)
                continue;
            const int ncomp = de::component_count(Body(body));
            for (int s = 0; s < lay.nsubint; ++s) {
                for (int c = 0; c < ncomp; ++c) {
                    for (int k = 0; k < lay.ncoeff; ++k) {
                        const size_t idx = size_t(lay.offset - 1) +
                                           size_t(s) * size_t(ncomp * lay.ncoeff) +
                                           size_t(c) * size_t(lay.ncoeff) + size_t(k);
                        rec[idx] = coeff(body, c, k);
                    }
                }
            }
        }
        for (double v : rec)
            w.f64(v);
    }
}

// Synthetic ephemeris: 450 constants (names past slot 400), a
// geocentric Moon with 8 subintervals, nutations, librations, TT-TDB,
// and an absent lunar-mantle column.
constexpr int kModernNcon = 450;
constexpr int kModernDenum = 999;

struct ModernLayout {
    de::BodyLayout bodies[de::kColumnCount] = {};
    int record_doubles = 0;
};

ModernLayout modern_layout() {
    const int shape[de::kColumnCount][2] = {
        {6, 4}, {5, 1}, {7, 2}, {5, 1}, {4, 1}, {4, 1}, {4, 1}, {4, 1},
        {4, 1}, {6, 8}, {6, 2}, {5, 4}, {5, 4}, {0, 0}, {4, 1},
    };
    ModernLayout m;
    int next = 3;
    for (int c = 0; c < de::kColumnCount; ++c) {
        if (shape[c][0] == 0)
            continue;
        m.bodies[c] = {next, shape[c][0], shape[c][1]};
        next += shape[c][0] * shape[c][1] * de::component_count(Body(c + 1));
    }
    m.record_doubles = next - 1;
    return m;
}

std::string write_modern(const fs::path& path, bool swap, double denum_constant = kModernDenum) {
    const ModernLayout m = modern_layout();
    const size_t rec_len = size_t(m.record_doubles) * 8;
    Writer w;
    w.swap = swap;
    w.text("JPL Planetary Ephemeris DE999/LE999 (prometheia synthetic)", 3 * 84);
    const auto name = [](int i) { return i == 0 ? std::string("DENUM") : "C" + std::to_string(i); };
    for (int i = 0; i < 400; ++i)
        w.text(name(i), 6);
    w.f64(kSynSS);
    w.f64(kSynSS + kSynNN * kSynRecords);
    w.f64(kSynNN);
    w.i32(kModernNcon);
    w.f64(kSynAU);
    w.f64(kSynEmrat);
    for (int c = 0; c < 12; ++c) {
        w.i32(m.bodies[c].offset);
        w.i32(m.bodies[c].ncoeff);
        w.i32(m.bodies[c].nsubint);
    }
    w.i32(kModernDenum);
    for (int v : {m.bodies[12].offset, m.bodies[12].ncoeff, m.bodies[12].nsubint})
        w.i32(v);
    for (int i = 400; i < kModernNcon; ++i)
        w.text(name(i), 6);
    // Column 14 absent the way JPL writes it: next free offset, zero counts.
    w.i32(m.record_doubles + 1);
    w.i32(0);
    w.i32(0);
    for (int v : {m.bodies[14].offset, m.bodies[14].ncoeff, m.bodies[14].nsubint})
        w.i32(v);
    CHECK(w.out.size() <= rec_len);
    w.pad_to(rec_len);
    for (int i = 0; i < kModernNcon; ++i)
        w.f64(i == 0 ? denum_constant : 5000.0 + i);
    w.pad_to(2 * rec_len);
    write_data(w, m.bodies, m.record_doubles);
    write_file(path, w.out);
    return path.string();
}

fs::path temp_dir() {
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() /
        ("prometheia-test-de-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    fs::create_directories(dir);
    return dir;
}

// Expected synthetic column value, computed independently of the reader:
// same record/subinterval geometry, explicit formula.
void expected_state(const de::BodyLayout& lay, int body, double jed, double out[6]) {
    double j = std::floor((jed - kSynSS) / kSynNN);
    if (j >= kSynRecords)
        j = kSynRecords - 1; // end epoch lands on the last record
    const double rec_start = kSynSS + kSynNN * j;
    const double width = kSynNN / lay.nsubint;
    double s = std::floor((jed - rec_start) / width);
    if (s > lay.nsubint - 1)
        s = lay.nsubint - 1;
    const double tau = 2.0 * (jed - (rec_start + s * width)) / width - 1.0;
    const int ncomp = de::component_count(Body(body));
    for (int c = 0; c < 3; ++c) {
        out[c] = c < ncomp ? expected_value(body, c, tau) : 0.0;
        out[3 + c] = c < ncomp ? expected_deriv(body, c, tau) * 2.0 / width : 0.0;
    }
}

double max_abs_diff(const double a[6], const double b[6]) {
    double m = 0.0;
    for (int i = 0; i < 6; ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

DeFile open_ok(const std::string& path) {
    auto opened = DeFile::open(path);
    CHECK(opened.ok());
    if (!opened.ok())
        std::printf("  open failed: %s\n", opened.error().message.c_str());
    return std::move(opened.value());
}

// Every present column at a grid crossing subinterval and record
// boundaries and the end epoch; returns the worst |delta|.
double worst_synthetic_delta(const DeFile& f) {
    const std::vector<double> times = {0.5,  3.99, 4.0,  4.01, 7.99,  8.0,  8.01, 15.99, 16.0,
                                       16.5, 31.9, 32.0, 32.5, 63.99, 64.0, 64.1, 95.9,  96.0};
    double worst = 0.0;
    for (int body = 1; body <= de::kColumnCount; ++body) {
        if (!f.has_body(Body(body)))
            continue;
        for (double dt : times) {
            const double jed = kSynSS + dt;
            double got[6], want[6];
            auto r = f.state(Body(body), jed, got);
            CHECK(r.ok());
            expected_state(f.header().bodies[body - 1], body, jed, want);
            worst = std::max(worst, max_abs_diff(got, want));
        }
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Part A: synthetic files (always run).
// ---------------------------------------------------------------------------

TEST_CASE("de_synthetic_errors") {
    const fs::path dir = temp_dir();
    const std::string good = write_modern(dir / "syn.eph", false);

    { // missing file
        auto r = DeFile::open((dir / "nope.eph").string());
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::IoError);
    }
    { // not a DE binary
        write_file(dir / "junk.eph", std::string(20000, 'x'));
        auto r = DeFile::open((dir / "junk.eph").string());
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::FormatError);
    }
    { // DENUM constant disagreeing with the header's NUMDE
        auto r = DeFile::open(write_modern(dir / "denum.eph", false, 440.0));
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::FormatError);
    }
    { // truncated mid-record: open must fail with a corruption report
        std::string bytes;
        {
            std::ifstream in(good, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            bytes = ss.str();
        }
        bytes.resize(bytes.size() - 10);
        write_file(dir / "trunc.eph", bytes);
        auto r = DeFile::open((dir / "trunc.eph").string());
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::CorruptionError);
    }
    { // out-of-range and absent-body queries
        DeFile f = open_ok(good);
        double out[6];
        auto below = f.state(Body::Mercury, kSynSS - 0.5, out);
        CHECK(!below.ok());
        CHECK(below.error().code == ErrorCode::ArgumentError);
        auto above = f.state(Body::Mercury, kSynSS + 96.0 + 0.5, out);
        CHECK(!above.ok());
        CHECK(above.error().code == ErrorCode::ArgumentError);
        auto nonfinite = f.state(Body::Mercury, std::nan(""), out);
        CHECK(!nonfinite.ok());
        CHECK(nonfinite.error().code == ErrorCode::ArgumentError);
        auto mantle = f.state(Body::LunarMantleOmega, kSynSS + 1.0, out);
        CHECK(!mantle.ok());
        CHECK(mantle.error().code == ErrorCode::NotFound);
        auto bad = f.state(Body(16), kSynSS + 1.0, out);
        CHECK(!bad.ok());
        CHECK(bad.error().code == ErrorCode::ArgumentError);
    }
    { // the end epoch itself is in coverage
        DeFile f = open_ok(good);
        double out[6], want[6];
        CHECK(f.state(Body::Moon, kSynSS + 96.0, out).ok());
        expected_state(f.header().bodies[9], 10, kSynSS + 96.0, want);
        CHECK(max_abs_diff(out, want) < 1e-9);
    }
    { // moved-from file stays usable via the move target
        DeFile a = open_ok(good);
        DeFile b = std::move(a);
        double out[6], want[6];
        CHECK(b.state(Body::Sun, kSynSS + 33.0, out).ok());
        expected_state(b.header().bodies[10], 11, kSynSS + 33.0, want);
        CHECK(max_abs_diff(out, want) < 1e-9);
    }
}

void check_modern_header(const DeFile& f, bool swapped) {
    const de::Header& h = f.header();
    const ModernLayout m = modern_layout();
    CHECK(h.byte_swapped == swapped);
    CHECK(h.denum == kModernDenum);
    CHECK(h.record_doubles == m.record_doubles);
    CHECK(h.record_count == 3);
    CHECK(h.start_jed == kSynSS);
    CHECK(h.end_jed == kSynSS + 96.0);
    CHECK(h.interval_days == 32.0);
    CHECK(h.au_km == kSynAU);
    CHECK(h.emrat == kSynEmrat);
    CHECK(h.title.find("DE999") != std::string::npos);
    CHECK(h.constant_names.size() == size_t(kModernNcon));
    CHECK(f.constant("DENUM").value_or(0.0) == double(kModernDenum));
    CHECK(f.constant("C399").value_or(0.0) == 5399.0);
    CHECK(f.constant("C400").value_or(0.0) == 5400.0); // first name past slot 400
    CHECK(f.constant("C449").value_or(0.0) == 5449.0);
    for (int c = 0; c < de::kColumnCount; ++c) {
        CHECK(h.bodies[c].offset == m.bodies[c].offset);
        CHECK(h.bodies[c].ncoeff == m.bodies[c].ncoeff);
        CHECK(h.bodies[c].nsubint == m.bodies[c].nsubint);
    }
    CHECK(f.has_body(Body::Librations));
    CHECK(f.has_body(Body::TTminusTDB));
    CHECK(!f.has_body(Body::LunarMantleOmega));
}

TEST_CASE("de_synthetic_little_endian") {
    const fs::path dir = temp_dir();
    DeFile f = open_ok(write_modern(dir / "modern.eph", false));
    check_modern_header(f, false);
    const double worst = worst_synthetic_delta(f);
    CHECK(worst < 1e-9);
    std::printf("  worst synthetic |delta| = %.3e\n", worst);
    double out[6];
    CHECK(f.state(Body::TTminusTDB, kSynSS + 5.0, out).ok());
    CHECK((out[1] == 0.0 && out[2] == 0.0 && out[4] == 0.0 && out[5] == 0.0));
    auto mantle = f.state(Body::LunarMantleOmega, kSynSS + 5.0, out);
    CHECK(!mantle.ok());
    CHECK(mantle.error().code == ErrorCode::NotFound);
}

TEST_CASE("de_synthetic_big_endian") {
    const fs::path dir = temp_dir();
    DeFile f = open_ok(write_modern(dir / "modern-be.eph", true));
    check_modern_header(f, true);
    CHECK(worst_synthetic_delta(f) < 1e-9);
}

TEST_CASE("de_synthetic_truncated_by_a_record") {
    const fs::path dir = temp_dir();
    const std::string path = write_modern(dir / "full.eph", false);
    fs::resize_file(path, fs::file_size(path) - size_t(modern_layout().record_doubles) * 8);
    auto r = DeFile::open(path);
    CHECK(!r.ok());
    CHECK(r.error().code == ErrorCode::CorruptionError);
}

TEST_CASE("de_relative_state_composition") {
    const fs::path dir = temp_dir();
    DeFile f = open_ok(write_modern(dir / "modern.eph", false));
    const double jed = kSynSS + 20.25;
    double emb[6], moon[6], mars[6], sun[6];
    CHECK(f.state(Body::EarthMoonBary, jed, emb).ok());
    CHECK(f.state(Body::Moon, jed, moon).ok());
    CHECK(f.state(Body::Mars, jed, mars).ok());
    CHECK(f.state(Body::Sun, jed, sun).ok());
    const double k = 1.0 / (1.0 + kSynEmrat);
    double earth[6], moon_b[6];
    for (int i = 0; i < 6; ++i) {
        earth[i] = emb[i] - moon[i] * k;
        moon_b[i] = earth[i] + moon[i];
    }
    const auto check_rel = [&](Target t, Target c, const double* want, double tol) {
        double got[6];
        CHECK(f.relative_state(t, c, jed, got).ok());
        CHECK(max_abs_diff(got, want) <= tol);
    };
    double want[6];
    for (int i = 0; i < 6; ++i)
        want[i] = mars[i] - earth[i];
    check_rel(Target::Mars, Target::Earth, want, 1e-6);
    for (int i = 0; i < 6; ++i)
        want[i] = moon_b[i] - sun[i];
    check_rel(Target::Moon, Target::Sun, want, 1e-6);
    check_rel(Target::Moon, Target::Earth, moon, 0.0); // exact: the column itself
    for (int i = 0; i < 6; ++i)
        want[i] = -moon[i] * k;
    check_rel(Target::Earth, Target::EarthMoonBary, want, 1e-9);
    for (int i = 0; i < 6; ++i)
        want[i] = emb[i];
    check_rel(Target::EarthMoonBary, Target::SolarSystemBary, want, 0.0);
    const double zeros[6] = {0, 0, 0, 0, 0, 0};
    check_rel(Target::Jupiter, Target::Jupiter, zeros, 0.0);

    double out[6];
    auto bad = f.relative_state(Target(14), Target::Sun, jed, out);
    CHECK(!bad.ok());
    CHECK(bad.error().code == ErrorCode::ArgumentError);
    auto outside = f.relative_state(Target::Sun, Target::Sun, kSynSS - 1.0, out);
    CHECK(!outside.ok());
    CHECK(outside.error().code == ErrorCode::ArgumentError);
}

// ---------------------------------------------------------------------------
// Part B: real JPL binaries (skipped when not present).
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

const std::string kDe200Path = env_or("PROMETHEIA_DE200", "/shares/swisseph/ephe/de200.eph");
const std::string kDe440Path =
    env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");
const std::string kTestpoPath =
    env_or("PROMETHEIA_TESTPO440", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/testpo.440");

const double kJ2000 = 2451545.0;

double norm3(const double v[6]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Position jump (km) across every subinterval boundary of one data record
// for bodies 1-11. Each side is evaluated just inside its own polynomial
// and carried to the boundary with its own velocity, so the body's motion
// during the probe offset cancels (residual ~ acceleration * eps^2). The
// offsets actually applied are recomputed from the rounded epochs: near
// JD 2.4e6 a double resolves ~40 us, worth ~2 m at Mercury's speed.
double worst_join_km(const DeFile& f, uint64_t record) {
    const de::Header& h = f.header();
    const double rec_start = h.start_jed + h.interval_days * double(record);
    constexpr double eps = 1e-6; // days
    double worst = 0.0;
    for (int body = 1; body <= 11; ++body) {
        const de::BodyLayout& lay = h.bodies[body - 1];
        for (int s = 1; s <= lay.nsubint; ++s) {
            const double t = rec_start + s * h.interval_days / lay.nsubint;
            const double ta = t - eps, tb = t + eps;
            double a[6], b[6], d[6] = {};
            CHECK(f.state(Body(body), ta, a).ok());
            CHECK(f.state(Body(body), tb, b).ok());
            for (int i = 0; i < 3; ++i)
                d[i] = (a[i] + a[3 + i] * (t - ta)) - (b[i] - b[3 + i] * (tb - t));
            worst = std::max(worst, norm3(d));
        }
    }
    return worst;
}

TEST_CASE("de200_real_header") {
    if (!available(kDe200Path, "PROMETHEIA_DE200"))
        return;
    DeFile f = open_ok(kDe200Path);
    const de::Header& h = f.header();
    CHECK(!h.byte_swapped);
    CHECK(h.denum == 200);
    CHECK(h.record_doubles == 826); // header.200: NCOEFF = 826
    CHECK(h.title.find("DE200") != std::string::npos);
    CHECK(h.start_jed == 2305424.5);
    CHECK(h.end_jed == 2513392.5);
    CHECK(h.interval_days == 32.0);
    CHECK(h.record_count == 6499);
    CHECK(std::fabs(f.constant("CLIGHT").value_or(0.0) - 299792.458) < 1e-6);
    CHECK(std::fabs(h.au_km - 149597870.66) < 0.01);
    CHECK(std::fabs(h.emrat - 81.300587) < 1e-5);
    CHECK(std::fabs(f.constant("GMS").value_or(0.0) - 2.95912208285591095e-4) < 1e-15);
    // The embedded pointer table matches GROUP 1050 of the published
    // header.200: nutations present, no librations.
    const int expected[de::kColumnCount][3] = {
        {3, 12, 4},   {147, 12, 1}, {183, 15, 2}, {273, 10, 1}, {303, 9, 1},
        {330, 8, 1},  {354, 8, 1},  {378, 6, 1},  {396, 6, 1},  {414, 12, 8},
        {702, 15, 1}, {747, 10, 4}, {0, 0, 0},    {0, 0, 0},    {0, 0, 0},
    };
    for (int c = 0; c < de::kColumnCount; ++c) {
        CHECK(h.bodies[c].offset == expected[c][0]);
        CHECK(h.bodies[c].ncoeff == expected[c][1]);
        CHECK(h.bodies[c].nsubint == expected[c][2]);
    }
}

TEST_CASE("de200_real_earth_and_moon_at_j2000") {
    if (!available(kDe200Path, "PROMETHEIA_DE200"))
        return;
    DeFile f = open_ok(kDe200Path);
    const double au = f.header().au_km;

    double moon[6];
    CHECK(f.relative_state(Target::Moon, Target::Earth, kJ2000, moon).ok());
    // Moon geocentric distance: 402448.6 km from this file; SWE (DE441
    // data) inverts to 402448.9 km at the same epoch.
    const double moon_dist = norm3(moon);
    CHECK(moon_dist > 402300.0);
    CHECK(moon_dist < 402600.0);
    const double moon_speed =
        std::sqrt(moon[3] * moon[3] + moon[4] * moon[4] + moon[5] * moon[5]) / 86400.0;
    CHECK(moon_speed > 0.90);
    CHECK(moon_speed < 1.10);

    // Analytic velocity agrees with a finite difference of the position.
    double p1[6], p2[6];
    CHECK(f.state(Body::Moon, kJ2000 - 0.001, p1).ok());
    CHECK(f.state(Body::Moon, kJ2000 + 0.001, p2).ok());
    double rel = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double fd = (p2[i] - p1[i]) / 0.002;
        rel = std::max(rel, std::fabs(fd - moon[3 + i]) / std::max(1.0, std::fabs(moon[3 + i])));
    }
    CHECK(rel < 1e-5);

    double earth_sun[6];
    CHECK(f.relative_state(Target::Earth, Target::Sun, kJ2000, earth_sun).ok());
    const double es = norm3(earth_sun) / au;
    CHECK(es > 0.98320);
    CHECK(es < 0.98340);

    // Heliocentric distances at J2000 (planets are barycentric in the
    // file; relative_state removes the Sun).
    const struct {
        Target t;
        double lo, hi;
    } ranges[] = {{Target::Mercury, 0.46645, 0.46650},
                  {Target::Mars, 1.39118, 1.39123},
                  {Target::Jupiter, 4.96536, 4.96541},
                  {Target::Saturn, 9.18382, 9.18387},
                  {Target::Neptune, 30.12050, 30.12055}};
    for (const auto& r : ranges) {
        double s[6];
        CHECK(f.relative_state(r.t, Target::Sun, kJ2000, s).ok());
        const double dist = norm3(s) / au;
        CHECK(dist > r.lo);
        CHECK(dist < r.hi);
        if (!(dist > r.lo && dist < r.hi))
            std::printf("  target %d heliocentric distance %.6f AU\n", int(r.t), dist);
    }
}

// Nutation columns hold the IAU 1980 series; our frames module uses the
// full IAU 2000A series. The two models differ by tens of mas.
void check_nutation_vs_frames(const DeFile& f, double jed, double tol_arcsec) {
    double nut[6];
    CHECK(f.state(Body::Nutations, jed, nut).ok());
    double dpsi = 0.0, deps = 0.0;
    frames::nutation(jed, dpsi, deps);
    const double d1 = std::fabs(nut[0] - dpsi) / kArcsec;
    const double d2 = std::fabs(nut[1] - deps) / kArcsec;
    CHECK(d1 < tol_arcsec);
    CHECK(d2 < tol_arcsec);
    std::printf("  nutation at JD %.1f: file dpsi %.4f\" deps %.4f\", vs IAU 2000A "
                "|d| = %.4f\" / %.4f\"\n",
                jed, nut[0] / kArcsec, nut[1] / kArcsec, d1, d2);
}

TEST_CASE("de200_real_nutations") {
    if (!available(kDe200Path, "PROMETHEIA_DE200"))
        return;
    DeFile f = open_ok(kDe200Path);
    check_nutation_vs_frames(f, kJ2000, 0.1);
    check_nutation_vs_frames(f, 2461000.5, 0.1);
}

TEST_CASE("de200_real_continuity") {
    if (!available(kDe200Path, "PROMETHEIA_DE200"))
        return;
    DeFile f = open_ok(kDe200Path);
    const double worst = worst_join_km(f, 4566);
    std::printf("  DE200 worst segment join: %.3e km\n", worst);
    CHECK(worst < 1e-5); // measured: ~1-2 mm
}

TEST_CASE("de200_real_obliquity_and_last_record") {
    if (!available(kDe200Path, "PROMETHEIA_DE200"))
        return;
    DeFile f = open_ok(kDe200Path);
    double s[6];
    CHECK(f.relative_state(Target::Earth, Target::Sun, kJ2000, s).ok());
    const double hx = s[1] * s[5] - s[2] * s[4];
    const double hy = s[2] * s[3] - s[0] * s[5];
    const double hz = s[0] * s[4] - s[1] * s[3];
    const double obl = std::acos(hz / std::sqrt(hx * hx + hy * hy + hz * hz)) * 180.0 / kPi;
    CHECK(obl > 23.43);
    CHECK(obl < 23.45);
    std::printf("  obliquity from Earth orbital pole: %.5f deg\n", obl);

    const de::Header& h = f.header();
    double out[6];
    CHECK(f.state(Body::Sun, h.end_jed, out).ok());
    for (int i = 0; i < 6; ++i)
        CHECK(std::isfinite(out[i]));
}

TEST_CASE("de440_real_header") {
    if (!available(kDe440Path, "PROMETHEIA_DE440"))
        return;
    DeFile f = open_ok(kDe440Path);
    const de::Header& h = f.header();
    CHECK(!h.byte_swapped);
    CHECK(h.denum == 440);
    CHECK(h.title.find("DE440") != std::string::npos);
    CHECK(h.start_jed == 2287184.5);
    CHECK(h.end_jed == 2688976.5);
    CHECK(h.interval_days == 32.0);
    CHECK(h.record_count == 12556);
    CHECK(h.record_doubles == 1018); // header.440: NCOEFF = 1018
    CHECK(h.au_km == 149597870.7);
    CHECK(std::fabs(h.emrat - 81.30056822149722) < 1e-12);
    CHECK(h.constant_names.size() == 645);
    CHECK(h.constant_names.back() == "MA8236");
    CHECK(f.constant("DENUM").value_or(0.0) == 440.0);
    CHECK(std::fabs(f.constant("CLIGHT").value_or(0.0) - 299792.458) < 1e-9);
    // GROUP 1050 from the published header.440.
    const int expected[de::kColumnCount][3] = {
        {3, 14, 4},   {171, 10, 2}, {231, 13, 2}, {309, 11, 1}, {342, 8, 1},
        {366, 7, 1},  {387, 6, 1},  {405, 6, 1},  {423, 6, 1},  {441, 13, 8},
        {753, 11, 2}, {819, 10, 4}, {899, 10, 4}, {0, 0, 0},    {0, 0, 0},
    };
    for (int c = 0; c < de::kColumnCount; ++c) {
        CHECK(h.bodies[c].offset == expected[c][0]);
        CHECK(h.bodies[c].ncoeff == expected[c][1]);
        CHECK(h.bodies[c].nsubint == expected[c][2]);
    }
}

// JPL's testpo.440: "denum date jed target center coordinate value", in
// AU and AU/day (nutations and librations in radians and rad/day).
// Targets: 1-11 bodies (3 = Earth, 10 = Moon), 12 = SSB, 13 = EMB,
// 14 = nutations, 15 = librations. Every point inside the file's
// coverage is checked.
TEST_CASE("de440_real_matches_jpl_testpo") {
    if (!available(kDe440Path, "PROMETHEIA_DE440") ||
        !available(kTestpoPath, "PROMETHEIA_TESTPO440"))
        return;
    DeFile f = open_ok(kDe440Path);
    const de::Header& h = f.header();
    std::ifstream in(kTestpoPath);
    CHECK(in.is_open());
    std::string line;
    bool in_body = false;
    long checked = 0;
    double worst_pos = 0.0, worst_nut = 0.0, worst_lib = 0.0;
    while (std::getline(in, line)) {
        if (!in_body) {
            in_body = line.rfind("EOT", 0) == 0;
            continue;
        }
        std::istringstream ls(line);
        int denum = 0, target = 0, center = 0, coord = 0;
        std::string date;
        double jed = 0.0, value = 0.0;
        if (!(ls >> denum >> date >> jed >> target >> center >> coord >> value))
            continue;
        CHECK(denum == 440);
        if (jed < h.start_jed || jed > h.end_jed)
            continue;
        double out[6];
        double got = 0.0;
        if (target == 14) {
            CHECK(f.state(Body::Nutations, jed, out).ok());
            static constexpr int map[4] = {0, 1, 3, 4};
            got = out[map[coord - 1]];
            worst_nut = std::max(worst_nut, std::fabs(got - value));
        } else if (target == 15) {
            CHECK(f.state(Body::Librations, jed, out).ok());
            got = out[coord - 1];
            worst_lib = std::max(worst_lib, std::fabs(got - value));
        } else {
            CHECK(f.relative_state(Target(target), Target(center), jed, out).ok());
            got = out[coord - 1] / h.au_km;
            worst_pos = std::max(worst_pos, std::fabs(got - value));
        }
        ++checked;
    }
    std::printf("  testpo.440: %ld points; worst |delta| bodies %.2e AU(/day), "
                "nutations %.2e rad, librations %.2e rad\n",
                checked, worst_pos, worst_nut, worst_lib);
    CHECK(checked > 13000);
    // testpo prints 20 digits after the decimal point; the bodies and
    // nutations reproduce to the double-precision noise of that print.
    // Libration angles grow to thousands of radians, so their absolute
    // noise floor is correspondingly larger.
    CHECK(worst_pos < 1e-13);
    CHECK(worst_nut < 1e-15);
    CHECK(worst_lib < 1e-10);
}

TEST_CASE("de440_real_nutations_and_continuity") {
    if (!available(kDe440Path, "PROMETHEIA_DE440"))
        return;
    DeFile f = open_ok(kDe440Path);
    check_nutation_vs_frames(f, kJ2000, 0.1);
    check_nutation_vs_frames(f, 2461000.5, 0.1);

    const double worst = worst_join_km(f, 7000);
    std::printf("  DE440 worst segment join: %.3e km\n", worst);
    CHECK(worst < 1e-5); // measured: ~1-2 mm
}

// DE200 (1981 fit, FK5-era frame) against DE440 at 1900, 2000, 2026 and
// 2100: geocentric directions agree to about 2" for the Sun, Moon and
// planets (measured worst: Neptune 2.14", Moon 1.49"); Pluto's 1981
// orbit is off by 18".
TEST_CASE("de200_vs_de440_geocentric_directions") {
    if (!available(kDe200Path, "PROMETHEIA_DE200") || !available(kDe440Path, "PROMETHEIA_DE440"))
        return;
    DeFile a = open_ok(kDe200Path);
    DeFile b = open_ok(kDe440Path);
    double worst = 0.0, worst_pluto = 0.0;
    for (double jed : {2415020.5, kJ2000, 2461000.5, 2488069.5}) {
        for (int t = 1; t <= 11; ++t) {
            if (t == 3)
                continue;
            double pa[6], pb[6];
            CHECK(a.relative_state(Target(t), Target::Earth, jed, pa).ok());
            CHECK(b.relative_state(Target(t), Target::Earth, jed, pb).ok());
            const double dot = pa[0] * pb[0] + pa[1] * pb[1] + pa[2] * pb[2];
            const double cross = std::sqrt(std::pow(pa[1] * pb[2] - pa[2] * pb[1], 2) +
                                           std::pow(pa[2] * pb[0] - pa[0] * pb[2], 2) +
                                           std::pow(pa[0] * pb[1] - pa[1] * pb[0], 2));
            double& w = t == int(Target::Pluto) ? worst_pluto : worst;
            w = std::max(w, std::atan2(cross, dot) / kArcsec);
        }
    }
    std::printf("  DE200 vs DE440 worst geocentric direction difference: %.3f\" "
                "(Pluto %.3f\")\n",
                worst, worst_pluto);
    CHECK(worst < 2.5);
    CHECK(worst_pluto < 20.0);
}

} // namespace
