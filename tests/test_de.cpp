// SPDX-License-Identifier: GPL-2.0-or-later
//
// DE binary reader tests. Part A builds a synthetic old-format file in
// the DE200 layout with a known analytic Chebyshev field and checks the
// reader exactly (runs everywhere, CI included). Part B validates the
// real JPL DE200 binary when it is available on this machine (path from
// PROMETHEIA_DE200, default /shares/swisseph/ephe/de200.eph); those
// cases print SKIP and pass when the file is absent.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <prometheia/de.hpp>
#include <prometheia/varint.hpp>

#include "test_main.hpp"

using namespace prometheia;
using prometheia::de::Body;
using prometheia::de::DeFile;

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Synthetic file writer: DE200 layout, analytic degree-3 Chebyshev field.
// ---------------------------------------------------------------------------

constexpr double kSynSS = 2451545.0;
constexpr double kSynNN = 32.0;
constexpr int kSynRecords = 3;

// f(tau) = A + B*tau + C*(2*tau^2-1) + D*(4*tau^3-3*tau)
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

// Writes a synthetic old-format file. denum overrides the DENUM constant
// (used to exercise unsupported-DENUM handling).
std::string write_synthetic(const fs::path& path, double denum = 200.0) {
    const de::OldFormatSpec* spec = de::old_format_spec(200);
    CHECK(spec != nullptr);
    const int nrec = spec->record_doubles;

    std::string out;
    // Header record 1: 3x84 title, 400 name slots, SS/FF/NN, padding.
    std::string title = "JPL Planetary Ephemeris DE200/DE200 (prometheia synthetic)";
    title.resize(3 * 84, ' ');
    out += title;
    for (int i = 0; i < spec->name_slot_count; ++i) {
        std::string field = i == 0 ? "DENUM" : "K" + std::to_string(i + 1);
        field.resize(6, ' ');
        out += field;
    }
    put_f64(out, kSynSS);
    put_f64(out, kSynSS + kSynNN * kSynRecords);
    put_f64(out, kSynNN);
    out.resize(size_t(nrec) * 8, '\0');

    // Header record 2: constants, DENUM first.
    std::string rec2;
    for (int i = 0; i < spec->constant_count; ++i) {
        put_f64(rec2, i == 0 ? denum : 1001.0 + i);
    }
    rec2.resize(size_t(nrec) * 8, '\0');
    out += rec2;

    // Data records.
    std::vector<double> rec(size_t(nrec), 0.0);
    for (int j = 0; j < kSynRecords; ++j) {
        std::fill(rec.begin(), rec.end(), 0.0);
        rec[0] = kSynSS + kSynNN * j;
        rec[1] = kSynSS + kSynNN * (j + 1);
        for (int body = 1; body <= 11; ++body) {
            const de::BodyLayout& lay = spec->bodies[body - 1];
            if (lay.offset <= 0)
                continue;
            for (int s = 0; s < lay.nsubint; ++s) {
                for (int c = 0; c < 3; ++c) {
                    for (int k = 0; k < lay.ncoeff; ++k) {
                        const size_t idx = size_t(lay.offset - 1) +
                                           size_t(s) * size_t(3 * lay.ncoeff) +
                                           size_t(c) * size_t(lay.ncoeff) + size_t(k);
                        rec[idx] = coeff(body, c, k);
                    }
                }
            }
        }
        std::string rs;
        for (double v : rec)
            put_f64(rs, v);
        out += rs;
    }

    fs::create_directories(path.parent_path());
    FILE* fp = std::fopen(path.string().c_str(), "wb");
    CHECK(fp != nullptr);
    const size_t written = std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    CHECK(written == out.size());
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

// Expected synthetic state, computed independently of the reader's
// internals: same record/subinterval geometry, explicit formula.
void expected_state(int body, double jed, double out[6]) {
    const de::OldFormatSpec* spec = de::old_format_spec(200);
    const de::BodyLayout& lay = spec->bodies[body - 1];
    double j = std::floor((jed - kSynSS) / kSynNN);
    if (j >= kSynRecords)
        j = kSynRecords - 1; // end epoch lands on the last record
    const double rec_start = kSynSS + kSynNN * j;
    const double width = kSynNN / lay.nsubint;
    double s = std::floor((jed - rec_start) / width);
    if (s > lay.nsubint - 1)
        s = lay.nsubint - 1;
    const double tau = 2.0 * (jed - (rec_start + s * width)) / width - 1.0;
    for (int c = 0; c < 3; ++c) {
        out[c] = expected_value(body, c, tau);
        out[3 + c] = expected_deriv(body, c, tau) * 2.0 / width;
    }
}

double max_abs_diff(const double a[6], const double b[6]) {
    double m = 0.0;
    for (int i = 0; i < 6; ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

// ---------------------------------------------------------------------------
// Part A: synthetic file (always runs).
// ---------------------------------------------------------------------------

TEST(de_synthetic_header_and_constants) {
    const fs::path dir = temp_dir();
    auto opened = DeFile::open(write_synthetic(dir / "syn.eph"));
    CHECK(opened.ok());
    DeFile f = std::move(opened.value());
    const de::Header& h = f.header();
    CHECK(h.denum == 200);
    CHECK(h.start_jed == kSynSS);
    CHECK(h.end_jed == kSynSS + 96.0);
    CHECK(h.interval_days == 32.0);
    CHECK(h.record_count == 3);
    CHECK(h.title.find("DE200") != std::string::npos);
    CHECK(f.constant("DENUM").value_or(0.0) == 200.0);
    CHECK(f.constant("K42").value_or(0.0) == 1042.0);
    CHECK(!f.constant("NOPE").has_value());
    CHECK(f.spec() != nullptr);
    CHECK(f.spec()->denum == 200);
    CHECK(f.has_body(Body::Mercury));
    CHECK(f.has_body(Body::Sun));
    CHECK(!f.has_body(Body::Libration));
}

TEST(de_synthetic_states_exact) {
    const fs::path dir = temp_dir();
    auto opened = DeFile::open(write_synthetic(dir / "syn.eph"));
    CHECK(opened.ok());
    DeFile f = std::move(opened.value());

    // Grid crosses subinterval boundaries (Mercury every 8d, Moon every
    // 4d, EMB every 16d), record boundaries (32d, 64d) and the end epoch.
    const std::vector<double> times = {0.5,  3.99, 4.0,  4.01, 7.99,  8.0,  8.01, 15.99, 16.0,
                                       16.5, 31.9, 32.0, 32.5, 63.99, 64.0, 64.1, 95.9,  96.0};
    double worst = 0.0;
    for (int body = 1; body <= 11; ++body) {
        if (!f.has_body(Body(body)))
            continue;
        for (double dt : times) {
            const double jed = kSynSS + dt;
            double got[6], want[6];
            auto r = f.state(Body(body), jed, got);
            CHECK(r.ok());
            expected_state(body, jed, want);
            worst = std::max(worst, max_abs_diff(got, want));
            for (int i = 0; i < 6; ++i) {
                CHECK(std::isfinite(got[i]));
            }
        }
    }
    CHECK(worst < 1e-9);
    std::printf("  worst synthetic |delta| = %.3e\n", worst);
}

TEST(de_synthetic_errors) {
    const fs::path dir = temp_dir();
    const std::string good = write_synthetic(dir / "syn.eph");

    { // missing file
        auto r = DeFile::open(dir / "nope.eph");
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::IoError);
    }
    { // unsupported DENUM
        write_synthetic(dir / "other.eph", 999.0);
        auto r = DeFile::open((dir / "other.eph").string());
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::FormatError);
    }
    { // truncated mid-record: open must fail with a corruption report
        const std::string full = write_synthetic(dir / "full.eph");
        std::string bytes;
        {
            FILE* fp = std::fopen(full.c_str(), "rb");
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0)
                bytes.append(buf, n);
            std::fclose(fp);
        }
        bytes.resize(bytes.size() - 10);
        {
            FILE* fp = std::fopen((dir / "trunc.eph").string().c_str(), "wb");
            std::fwrite(bytes.data(), 1, bytes.size(), fp);
            std::fclose(fp);
        }
        auto r = DeFile::open((dir / "trunc.eph").string());
        CHECK(!r.ok());
        CHECK(r.error().code == ErrorCode::CorruptionError);
    }
    { // out-of-range and absent-body queries
        auto opened = DeFile::open(good);
        CHECK(opened.ok());
        DeFile f = std::move(opened.value());
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
        auto lib = f.state(Body::Libration, kSynSS + 1.0, out);
        CHECK(!lib.ok());
        CHECK(lib.error().code == ErrorCode::NotFound);
    }
    { // the end epoch itself is in coverage
        auto opened = DeFile::open(good);
        CHECK(opened.ok());
        DeFile f = std::move(opened.value());
        double out[6], want[6];
        auto r = f.state(Body::Moon, kSynSS + 96.0, out);
        CHECK(r.ok());
        expected_state(10, kSynSS + 96.0, want);
        CHECK(max_abs_diff(out, want) < 1e-9);
    }
    { // moved-from file stays usable via the move target
        auto opened = DeFile::open(good);
        CHECK(opened.ok());
        DeFile a = std::move(opened.value());
        DeFile b = std::move(a);
        double out[6], want[6];
        auto r = b.state(Body::Sun, kSynSS + 33.0, out);
        CHECK(r.ok());
        expected_state(11, kSynSS + 33.0, want);
        CHECK(max_abs_diff(out, want) < 1e-9);
    }
}

// ---------------------------------------------------------------------------
// Part B: real JPL DE200 binary (skipped when not present).
// ---------------------------------------------------------------------------

const char* real_path() {
    const char* env = std::getenv("PROMETHEIA_DE200");
    if (env && *env)
        return env;
    return "/shares/swisseph/ephe/de200.eph";
}

bool real_available() {
    static const bool have = std::filesystem::exists(real_path());
    if (!have) {
        static const bool announced = [] {
            std::printf("[SKIP] real-file DE200 tests: '%s' not found "
                        "(set PROMETHEIA_DE200 to enable)\n",
                        real_path());
            return true;
        }();
        (void)announced;
    }
    return have;
}

const double kJ2000 = 2451545.0;

double au_km(const DeFile& f) {
    return f.constant("AU").value();
}

TEST(de200_real_header) {
    if (!real_available())
        return;
    auto opened = DeFile::open(real_path());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    DeFile f = std::move(opened.value());
    const de::Header& h = f.header();
    CHECK(h.denum == 200);
    CHECK(h.title.find("DE200") != std::string::npos);
    CHECK(h.start_jed == 2305424.5);
    CHECK(h.end_jed == 2513392.5);
    CHECK(h.interval_days == 32.0);
    CHECK(h.record_count == 6499);
    CHECK(std::fabs(f.constant("CLIGHT").value_or(0.0) - 299792.458) < 1e-6);
    CHECK(std::fabs(f.constant("AU").value_or(0.0) - 149597870.66) < 0.01);
    CHECK(std::fabs(f.constant("EMRAT").value_or(0.0) - 81.300587) < 1e-5);
    CHECK(std::fabs(f.constant("GMS").value_or(0.0) - 2.95912208285591095e-4) < 1e-15);
    CHECK(f.has_body(Body::Moon));
    CHECK(f.has_body(Body::Sun));
    CHECK(!f.has_body(Body::Libration));
}

TEST(de200_real_earth_and_moon_at_j2000) {
    if (!real_available())
        return;
    auto opened = DeFile::open(real_path());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    DeFile f = std::move(opened.value());
    const double emrat = f.constant("EMRAT").value();
    const double au = au_km(f);

    double emb[6], moon[6], sun[6];
    CHECK(f.state(Body::EarthMoonBary, kJ2000, emb).ok());
    CHECK(f.state(Body::Moon, kJ2000, moon).ok());
    CHECK(f.state(Body::Sun, kJ2000, sun).ok());

    // Moon geocentric distance: 402448.6 km from this file; SWE (DE441
    // data) inverts to 402448.9 km at the same epoch. Keep a small window.
    const double moon_dist = std::sqrt(moon[0] * moon[0] + moon[1] * moon[1] + moon[2] * moon[2]);
    CHECK(moon_dist > 402300.0 && moon_dist < 402600.0);

    const double moon_speed =
        std::sqrt(moon[3] * moon[3] + moon[4] * moon[4] + moon[5] * moon[5]) / 86400.0;
    CHECK(moon_speed > 0.90 && moon_speed < 1.10);

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

    // Earth = EMB - Moon/(1+EMRAT); Earth-Sun distance at J2000.
    double earth[3];
    for (int i = 0; i < 3; ++i)
        earth[i] = emb[i] - moon[i] / (1.0 + emrat);
    double d2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = earth[i] - sun[i];
        d2 += d * d;
    }
    const double earth_sun = std::sqrt(d2) / au;
    CHECK(earth_sun > 0.98320 && earth_sun < 0.98340);

    // Heliocentric distances of the planets (Sun-centred entries).
    const struct {
        Body b;
        double lo, hi;
    } ranges[] = {{Body::Mercury, 0.470, 0.473},
                  {Body::Mars, 1.383, 1.386},
                  {Body::Jupiter, 4.956, 4.960},
                  {Body::Saturn, 9.175, 9.180},
                  {Body::Neptune, 30.115, 30.123}};
    for (const auto& r : ranges) {
        double s[6];
        CHECK(f.state(r.b, kJ2000, s).ok());
        const double dist = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) / au;
        CHECK(dist > r.lo && dist < r.hi);
    }
}

TEST(de200_real_continuity) {
    if (!real_available())
        return;
    auto opened = DeFile::open(real_path());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    DeFile f = std::move(opened.value());
    const de::Header& h = f.header();
    // Probe one record in the middle of the file: every subinterval
    // boundary plus the record boundary must join to well under a km
    // (measured worst case on this file: Mercury 0.80 km).
    const double rec_start = h.start_jed + h.interval_days * 4566;
    for (int body = 1; body <= 11; ++body) {
        if (!f.has_body(Body(body)))
            continue;
        const de::BodyLayout& lay = f.spec()->bodies[body - 1];
        double worst = 0.0;
        auto jump = [&](double t) {
            double a[6], b[6];
            CHECK(f.state(Body(body), t - 1e-7, a).ok());
            CHECK(f.state(Body(body), t + 1e-7, b).ok());
            double d2 = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double d = a[i] - b[i];
                d2 += d * d;
            }
            worst = std::max(worst, std::sqrt(d2));
        };
        for (int s = 1; s <= lay.nsubint; ++s) {
            jump(rec_start + s * h.interval_days / lay.nsubint);
        }
        CHECK(worst < 1.5);
    }
}

TEST(de200_real_obliquity_from_earth_pole) {
    if (!real_available())
        return;
    auto opened = DeFile::open(real_path());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    DeFile f = std::move(opened.value());
    const double emrat = f.constant("EMRAT").value();

    double emb[6], moon[6], sun[6];
    CHECK(f.state(Body::EarthMoonBary, kJ2000, emb).ok());
    CHECK(f.state(Body::Moon, kJ2000, moon).ok());
    CHECK(f.state(Body::Sun, kJ2000, sun).ok());

    double r[3], v[3];
    for (int i = 0; i < 3; ++i) {
        r[i] = (emb[i] - moon[i] / (1.0 + emrat)) - sun[i];
        v[i] = (emb[3 + i] - moon[3 + i] / (1.0 + emrat)) - sun[3 + i];
    }
    const double hx = r[1] * v[2] - r[2] * v[1];
    const double hy = r[2] * v[0] - r[0] * v[2];
    const double hz = r[0] * v[1] - r[1] * v[0];
    const double hn = std::sqrt(hx * hx + hy * hy + hz * hz);
    const double obl = std::acos(hz / hn) * (180.0 / 3.14159265358979323846);
    // DE200's frame is its own 1981 J2000-equatorial realisation; the
    // ecliptic-obliquity test is a coarse frame sanity check.
    CHECK(obl > 23.43 && obl < 23.45);
    std::printf("  obliquity from Earth orbital pole: %.5f deg\n", obl);
}

TEST(de200_real_last_record) {
    if (!real_available())
        return;
    auto opened = DeFile::open(real_path());
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    DeFile f = std::move(opened.value());
    const de::Header& h = f.header();
    // The final epoch is in coverage and the last record is readable.
    double out[6];
    auto r = f.state(Body::Sun, h.end_jed, out);
    CHECK(r.ok());
    if (!r.ok())
        return;
    double before[6];
    CHECK(f.state(Body::Sun, h.end_jed - 0.5, before).ok());
    for (int i = 0; i < 6; ++i) {
        CHECK(std::isfinite(out[i]));
    }
}

} // namespace

int main() {
    return ptest::run_all();
}