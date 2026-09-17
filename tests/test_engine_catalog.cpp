// SPDX-License-Identifier: GPL-2.0-or-later
//
// Catalog overlay tests: small bodies answered by the engine through
// on-demand integration of the catalog's osculating elements.
//
// Part A (runs everywhere, CI included): the barycentric force model
// against the closed-form two-body solution, and the whole overlay
// pipeline on the synthetic linear SPK kernel against an independent
// integration of the same force model (the oracle reads the kernel
// directly, no spline tables).
//
// Part B (PROMETHEIA_DE440; SKIP when absent): Ceres from
// tests/data/sample-100.epm against fixtures generated from the Swiss
// Ephemeris swetest CLI running on the same DE440 with its own asteroid
// file seas_18.se1 (output-only oracle), so the residuals measure the
// integration, the seeding and the frame conventions.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <prometheia/catalog.hpp>
#include <prometheia/engine.hpp>
#include <prometheia/forces.hpp>
#include <prometheia/frames.hpp>
#include <prometheia/integrator.hpp>
#include <prometheia/kepler.hpp>
#include <prometheia/memo.hpp>
#include <prometheia/spk.hpp>

#include "synthetic_spk.hpp"
#include "test_main.hpp"

using namespace prometheia;
using synth::open_synthetic;
using synth::TempFile;

namespace {

constexpr double kAuKm = 149597870.7;
constexpr double kJ2000 = 2451545.0;
constexpr uint64_t kSpkid = 20000001;
constexpr double kEpoch = 2460000.5; // catalog epoch, inside the kernel span

// Ceres-like elements (the sample catalog's record for 20000001).
const Elements kEls{2.765552595034094, 0.07969229514816586, 0.1848, 1.4006, 1.2792, 1.0};

// ---------------------------------------------------------------------------
// Part A1: the force model.
// ---------------------------------------------------------------------------

// One "Sun" parked at the origin: the barycentric force degenerates to
// the two-body problem, which has a closed-form solution.
struct StaticSun : PerturberStates {
    void ensure(double) override {}
    void state(size_t, double, double out[6]) override {
        for (int i = 0; i < 6; ++i)
            out[i] = 0.0;
    }
    size_t count() const override { return 1; }
    const double* mus() const override { return &mu; }
    bool ok() const override { return true; }
    double mu = gm::kSun;
};

void to_array(const State& s, double y[6]) {
    y[0] = s.pos.x;
    y[1] = s.pos.y;
    y[2] = s.pos.z;
    y[3] = s.vel.x;
    y[4] = s.vel.y;
    y[5] = s.vel.z;
}

double distance_states_au(const State& a, const State& b) {
    double d = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double t = (&a.pos.x)[i] - (&b.pos.x)[i];
        d += t * t;
    }
    return std::sqrt(d);
}

double distance_au(const State& a, const double b[6]) {
    double d = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double t = (&a.pos.x)[i] - b[i];
        d += t * t;
    }
    return std::sqrt(d);
}

TEST(barycentric_force_two_body_limit) {
    StaticSun sun;
    BarycentricForce force{&sun};
    auto s0 = elements_to_state(gm::kSun, kEls);
    CHECK(s0.ok());

    double y[6];
    to_array(s0.value(), y);
    IntegrateStats stats;
    auto r = integrate_dp54(y, kJ2000, kJ2000 + 1000.0, force, IntegrateOptions{}, &stats);
    CHECK(r.ok());

    auto exact = kepler_propagate(gm::kSun, s0.value(), kJ2000, kJ2000 + 1000.0);
    CHECK(exact.ok());
    CHECK(distance_au(exact.value(), y) < 1e-8);

    // Backward, too: the memo marches both ways.
    to_array(s0.value(), y);
    r = integrate_dp54(y, kJ2000, kJ2000 - 1000.0, force, IntegrateOptions{}, &stats);
    CHECK(r.ok());
    exact = kepler_propagate(gm::kSun, s0.value(), kJ2000, kJ2000 - 1000.0);
    CHECK(exact.ok());
    CHECK(distance_au(exact.value(), y) < 1e-8);
}

TEST(barycentric_force_memo_matches_integration) {
    StaticSun sun;
    BarycentricForce force{&sun};
    auto s0 = elements_to_state(gm::kSun, kEls);
    CHECK(s0.ok());

    WindowMemo<BarycentricForce> memo(&force, IntegrateOptions{});
    memo.set_seed(s0.value(), kJ2000);

    // Both the memo and a free-running dp54 pass are compared against the
    // closed-form solution; at rtol 1e-12 two step sequences disagree by
    // ~1e-9 AU over 1000 days, so the exact solution is the only common
    // reference. The memo's fixed sub-stepping must not cost more than
    // that same order.
    double worst_memo = 0.0, worst_direct = 0.0;
    for (double t : {kJ2000 + 400.0, kJ2000 + 1000.0, kJ2000 - 800.0, kJ2000 + 0.25}) {
        auto exact = kepler_propagate(gm::kSun, s0.value(), kJ2000, t);
        CHECK(exact.ok());
        double y[6];
        to_array(s0.value(), y);
        IntegrateStats stats;
        CHECK(integrate_dp54(y, kJ2000, t, force, IntegrateOptions{}, &stats).ok());
        const State s = memo.at(t);
        CHECK(memo.coverage_lo() <= t && t <= memo.coverage_hi());
        worst_memo = std::max(worst_memo, distance_states_au(s, exact.value()));
        worst_direct = std::max(worst_direct, distance_au(exact.value(), y));
    }
    std::printf("  memo %.3g AU, direct dp54 %.3g AU vs closed form\n", worst_memo, worst_direct);
    CHECK(worst_memo < 1e-8);
    CHECK(worst_direct < 1e-8);
}

// ---------------------------------------------------------------------------
// Part A2: the engine overlay on the synthetic kernel, against an
// independent integration of the same force model.
// ---------------------------------------------------------------------------

catalog::Record make_record(double a_au) {
    catalog::Record r;
    r.spkid = kSpkid;
    r.body_class = catalog::BodyClass::Asteroid;
    r.epoch_jtdb = kEpoch;
    r.a_au = a_au;
    r.e = kEls.e;
    r.inc_rad = kEls.inc;
    r.node_rad = kEls.node;
    r.argp_rad = kEls.argp;
    r.mean_anom_rad = kEls.mean_anom;
    r.flags = catalog::RecordFlags::kSigmas;
    for (double& s : r.sigmas)
        s = 1e-6;
    return r;
}

std::string write_catalog(const TempFile& tf, double a_au, uint64_t spkid = kSpkid) {
    catalog::Record r = make_record(a_au);
    r.spkid = spkid;
    auto w = catalog::Writer::create(tf.path.string(), catalog::WriterOptions{});
    CHECK(w.ok());
    CHECK(w.value().add(r, std::to_string(spkid), "Testbody").ok());
    CHECK(w.value().finish(CborValue::make_map()).ok());
    return tf.path.string();
}

// Reads the kernel itself, per force evaluation, for the three bodies it
// carries — the same masses the engine picks (Sun, Earth, Jupiter
// barycentre) with the same built-in GMs.
struct KernelPerturbers : PerturberStates {
    explicit KernelPerturbers(spk::SpkFile f) : file_(std::move(f)) {}
    void ensure(double) override {}
    void state(size_t i, double t, double out[6]) override {
        auto r = file_.state(ids_[i], 0, t, out);
        if (!r) {
            bad = true;
            for (int k = 0; k < 6; ++k)
                out[k] = 0.0;
            return;
        }
        for (int k = 0; k < 6; ++k)
            out[k] /= kAuKm;
    }
    size_t count() const override { return ids_.size(); }
    const double* mus() const override { return mus_.data(); }
    bool ok() const override { return !bad; }

    spk::SpkFile file_;
    std::vector<int> ids_{10, 399, 5};
    std::vector<double> mus_{gm::kSun, gm::kEarth, gm::kJupiter};
    bool bad = false;
};

// The engine's seed construction, rebuilt independently: elements ->
// heliocentric state in the ecliptic of J2000, rotated to ICRF by the
// transpose of R1(eps0)*B, then translated by the Sun's barycentric state.
State oracle_seed(spk::SpkFile& file) {
    auto helio = elements_to_state(gm::kSun, kEls);
    CHECK(helio.ok());
    double b[9];
    frames::frame_bias_matrix(b);
    const double eps0 = frames::mean_obliquity(kJ2000);
    const double c = std::cos(eps0), s = std::sin(eps0);
    const double r1[9] = {1, 0, 0, 0, c, s, 0, -s, c};
    double m[9]; // r1 * b
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[3 * i + j] = r1[3 * i] * b[j] + r1[3 * i + 1] * b[3 + j] + r1[3 * i + 2] * b[6 + j];

    double sun[6];
    CHECK(file.state(10, 0, kEpoch, sun).ok());
    State out;
    const double rh[3] = {helio.value().pos.x, helio.value().pos.y, helio.value().pos.z};
    const double vh[3] = {helio.value().vel.x, helio.value().vel.y, helio.value().vel.z};
    for (int i = 0; i < 3; ++i) {
        (&out.pos.x)[i] = m[i] * rh[0] + m[3 + i] * rh[1] + m[6 + i] * rh[2] + sun[i] / kAuKm;
        (&out.vel.x)[i] = m[i] * vh[0] + m[3 + i] * vh[1] + m[6 + i] * vh[2] + sun[3 + i] / kAuKm;
    }
    return out;
}

TEST(engine_overlay_matches_independent_integration) {
    TempFile tf_kernel("cat-kernel");
    TempFile tf_cat("cat-overlay");
    Engine e = open_synthetic(tf_kernel);
    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());

    auto spk = spk::SpkFile::open(tf_kernel.path.string());
    CHECK(spk.ok());
    KernelPerturbers pert(std::move(spk).value());
    BarycentricForce force{&pert};
    const State seed = oracle_seed(pert.file_);

    CalcOptions o = CalcOptions::geometric();
    o.center = Center::Barycentric;
    o.frame = Frame::ICRF;
    o.coords = Coords::Equatorial;
    o.speed = false;

    // Forward, backward, and across several memo windows.
    for (double dt : {400.5, -800.25, 365.25 * 2 + 100.0, -0.75}) {
        const double t = kEpoch + dt;
        double y[6];
        to_array(seed, y);
        IntegrateStats stats;
        CHECK(integrate_dp54(y, kEpoch, t, force, IntegrateOptions{}, &stats).ok());
        CHECK(pert.ok());

        auto res = e.calc(int(kSpkid), t, o);
        CHECK(res.ok());
        if (!res.ok()) {
            std::printf("  calc: %s\n", res.error().message.c_str());
            continue;
        }
        double d = 0.0;
        for (int i = 0; i < 3; ++i)
            d += (res.value().pos.xyz_au[i] - y[i]) * (res.value().pos.xyz_au[i] - y[i]);
        d = std::sqrt(d);
        if (d > 1e-8)
            std::printf("  dt=%g: %.3g AU off\n", dt, d);
        CHECK(d < 1e-8);
    }
}

TEST(engine_overlay_speeds_and_consistency) {
    TempFile tf_kernel("cat-speed-k");
    TempFile tf_cat("cat-speed-c");
    Engine e = open_synthetic(tf_kernel);
    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());

    // Geocentric = barycentric minus the Earth, through the engine itself.
    const double t = kEpoch + 300.125;
    CalcOptions obary = CalcOptions::geometric();
    obary.center = Center::Barycentric;
    auto body_bary = e.calc(int(kSpkid), t, obary);
    CHECK(body_bary.ok());
    auto earth = e.calc(body::kEarth, t, obary);
    CHECK(earth.ok());

    auto body_geo = e.calc(int(kSpkid), t, CalcOptions::geometric());
    CHECK(body_geo.ok());

    for (int i = 0; i < 3; ++i) {
        const double expect = body_bary.value().pos.xyz_au[i] - earth.value().pos.xyz_au[i];
        CHECK(std::fabs(body_geo.value().pos.xyz_au[i] - expect) < 1e-12);
    }

    // Rates are central differences of the apparent coordinates: differencing
    // positions by the same step must reproduce the speed columns.
    const double h = 0.001;
    auto tp = e.calc(int(kSpkid), t + h, CalcOptions::geometric());
    auto tm = e.calc(int(kSpkid), t - h, CalcOptions::geometric());
    CHECK(tp.ok() && tm.ok());
    CHECK(std::fabs((tp.value().pos.lon_deg - tm.value().pos.lon_deg) / (2 * h) -
                    body_geo.value().pos.lon_speed) < 1e-6);
    CHECK(std::fabs((tp.value().pos.dist_au - tm.value().pos.dist_au) / (2 * h) -
                    body_geo.value().pos.dist_speed) < 1e-8);

    // Warm cache: identical epoch, identical answer.
    auto again = e.calc(int(kSpkid), t, CalcOptions::geometric());
    CHECK(again.ok());
    CHECK(again.value().pos.lon_deg == body_geo.value().pos.lon_deg);
    CHECK(again.value().pos.dist_au == body_geo.value().pos.dist_au);
}

TEST(engine_overlay_provenance_and_errors) {
    TempFile tf_kernel("cat-err-k");
    TempFile tf_cat("cat-err-c");
    Engine e = open_synthetic(tf_kernel);

    // Before any catalog: the body is simply not known.
    auto res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    CHECK(!res.ok() && res.error().code == ErrorCode::NotFound);

    // Catalog file problems.
    CHECK(!e.add_catalog("/nonexistent/sb.epm").ok());
    {
        TempFile tf_junk("cat-err-j");
        FILE* f = std::fopen(tf_junk.path.c_str(), "wb");
        std::fwrite("not an epm1 container", 1, 20, f);
        std::fclose(f);
        auto bad = e.add_catalog(tf_junk.path.string());
        CHECK(!bad.ok());
    }

    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());
    res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    CHECK(res.ok());
    if (res.ok()) {
        // Provenance names the catalog overlay, and the source string
        // outlives the CalcResult only while the Engine does (contract).
        const std::string src(res.value().provenance.source);
        CHECK(src.find("EPM1") != std::string::npos);
        CHECK(res.value().provenance.denum == 0); // synthetic kernel: no DE
    }

    // A body in no catalog and no ephemeris.
    auto missing = e.calc(20009999, kEpoch + 10.0, CalcOptions::geometric());
    CHECK(!missing.ok() && missing.error().code == ErrorCode::NotFound);
}

TEST(engine_overlay_newest_catalog_wins) {
    TempFile tf_kernel("cat-new-k");
    TempFile tf_a("cat-new-a");
    TempFile tf_b("cat-new-b");
    Engine e = open_synthetic(tf_kernel);

    CHECK(e.add_catalog(write_catalog(tf_a, kEls.a)).ok());
    const double t = kEpoch + 200.5;
    auto first = e.calc(int(kSpkid), t, CalcOptions::geometric());
    CHECK(first.ok());

    // Same body, larger semimajor axis: a genuinely different orbit.
    CHECK(e.add_catalog(write_catalog(tf_b, kEls.a + 0.01)).ok());
    auto second = e.calc(int(kSpkid), t, CalcOptions::geometric());
    CHECK(second.ok());
    CHECK(std::fabs(second.value().pos.dist_au - first.value().pos.dist_au) > 1e-4);

    // ... and it is exactly what a fresh engine sees with that catalog alone.
    Engine fresh = open_synthetic(tf_kernel);
    CHECK(fresh.add_catalog(tf_b.path.string()).ok());
    auto ref = fresh.calc(int(kSpkid), t, CalcOptions::geometric());
    CHECK(ref.ok());
    CHECK(ref.value().pos.dist_au == second.value().pos.dist_au);
    CHECK(ref.value().pos.lon_deg == second.value().pos.lon_deg);
}

} // namespace

// ---------------------------------------------------------------------------
// Part B: DE440 + sample-100.epm vs the Swiss Ephemeris asteroid files.
// ---------------------------------------------------------------------------

namespace {

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

struct CatalogFixture {
    double jd_tt;
    int spkid;
    double lon_deg, lat_deg, dist_au;    // apparent, ecliptic of date
    double jlon_deg, jlat_deg, jdist_au; // geometric, ecliptic J2000
};

#include "catalog_fixtures.inc"

} // namespace

namespace {

TEST(de440_ceres_vs_swetest) {
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto e = Engine::open(de);
    CHECK(e.ok());
    if (!e.ok())
        return;
    CHECK(e.value()
              .add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm")
              .ok());

    double worst_apparent = 0.0, worst_geometric = 0.0, worst_dist = 0.0;
    for (const CatalogFixture& f : kCatalogFixtures) {
        auto app = e.value().calc(f.spkid, f.jd_tt, CalcOptions::apparent());
        CHECK(app.ok());
        CalcOptions oj2000 = CalcOptions::geometric();
        oj2000.frame = Frame::J2000;
        auto geo = e.value().calc(f.spkid, f.jd_tt, oj2000);
        CHECK(geo.ok());
        if (!app.ok() || !geo.ok())
            continue;

        // Great-circle separation in arcsec.
        auto sep = [](double lon1, double lat1, double lon2, double lat2) {
            const double dr = 3.14159265358979323846 / 180.0;
            const double c1 = std::cos((90 - lat1) * dr), c2 = std::cos((90 - lat2) * dr);
            const double a[3] = {c1 * std::cos(lon1 * dr), c1 * std::sin(lon1 * dr),
                                 std::sin(lat1 * dr)};
            const double b[3] = {c2 * std::cos(lon2 * dr), c2 * std::sin(lon2 * dr),
                                 std::sin(lat2 * dr)};
            const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2],
                         cz = a[0] * b[1] - a[1] * b[0];
            return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), d) / dr * 3600.0;
        };

        worst_apparent =
            std::max(worst_apparent,
                     sep(app.value().pos.lon_deg, app.value().pos.lat_deg, f.lon_deg, f.lat_deg));
        worst_geometric =
            std::max(worst_geometric,
                     sep(geo.value().pos.lon_deg, geo.value().pos.lat_deg, f.jlon_deg, f.jlat_deg));
        worst_dist = std::max(worst_dist, std::fabs(geo.value().pos.dist_au - f.jdist_au));
        CHECK(worst_apparent < kCatalogGateArcsec);
        CHECK(worst_dist < 1e-5);
    }
    std::printf("  Ceres vs swetest/DE440: apparent %.4f\", geometric J2000 %.4f\", dist %.2e AU\n",
                worst_apparent, worst_geometric, worst_dist);
}

} // namespace

int main() {
    return ptest::run_all();
}