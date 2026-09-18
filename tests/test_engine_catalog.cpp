// SPDX-License-Identifier: GPL-2.0-or-later
//
// Catalog overlay tests: small bodies answered by the engine through
// on-demand integration of the catalog's osculating elements.
//
// Part A (runs everywhere, no data files needed): the barycentric force model
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
#include <array>
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
#include <doctest/doctest.h>

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

TEST_CASE("barycentric_force_two_body_limit") {
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

TEST_CASE("barycentric_force_memo_matches_integration") {
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
        CHECK(memo.coverage_lo() <= t);
        CHECK(t <= memo.coverage_hi());
        worst_memo = std::max(worst_memo, distance_states_au(s, exact.value()));
        worst_direct = std::max(worst_direct, distance_au(exact.value(), y));
    }
    std::printf("  memo %.3g AU, direct dp54 %.3g AU vs closed form\n", worst_memo, worst_direct);
    CHECK(worst_memo < 1e-8);
    CHECK(worst_direct < 1e-8);
}

// The Sun's post-Newtonian term against its closed form: the perihelion of
// a tight, eccentric orbit about a static Sun advances by
// 6 pi mu / (c^2 a (1 - e^2)) per revolution. The Laplace-Runge-Lenz
// vector is read after whole periods, where the osculating wobble repeats.
TEST_CASE("barycentric_force_relativistic_perihelion_advance") {
    struct RelativisticSun : StaticSun {
        long sun_index() const override { return 0; }
    } sun;
    const double a = 0.05, e = 0.5;
    const Elements els{a, e, 0.0, 0.0, 0.0, 0.0};
    auto s0 = elements_to_state(gm::kSun, els);
    REQUIRE(s0.ok());
    const double period = 2.0 * 3.14159265358979323846 * std::sqrt(a * a * a / gm::kSun);
    const int orbits = 100;

    auto perihelion_angle = [](const double y[6]) {
        const double h[3] = {y[1] * y[5] - y[2] * y[4], y[2] * y[3] - y[0] * y[5],
                             y[0] * y[4] - y[1] * y[3]};
        const double r = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        const double ax = (y[4] * h[2] - y[5] * h[1]) / gm::kSun - y[0] / r;
        const double ay = (y[5] * h[0] - y[3] * h[2]) / gm::kSun - y[1] / r;
        return std::atan2(ay, ax);
    };
    double y[6];
    to_array(s0.value(), y);
    const double w0 = perihelion_angle(y);

    for (bool relativity : {false, true}) {
        BarycentricForce force{&sun, relativity};
        to_array(s0.value(), y);
        IntegrateStats stats;
        REQUIRE(
            integrate_dp54(y, kJ2000, kJ2000 + orbits * period, force, IntegrateOptions{}, &stats)
                .ok());
        const double advance =
            std::remainder(perihelion_angle(y) - w0, 2.0 * 3.14159265358979323846);
        const double expected = relativity
                                    ? orbits * 6.0 * 3.14159265358979323846 * gm::kSun /
                                          (kLightAuPerDay * kLightAuPerDay * a * (1.0 - e * e))
                                    : 0.0;
        std::printf("  relativity %d: perihelion advance %.6e rad (closed form %.6e)\n",
                    int(relativity), advance, expected);
        CHECK(std::fabs(advance - expected) < 0.01 * 4.96e-4);
    }
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

std::string write_record(const TempFile& tf, const catalog::Record& r) {
    auto w = catalog::Writer::create(tf.path.string(), catalog::WriterOptions{});
    CHECK(w.ok());
    CHECK(w.value().add(r, std::to_string(r.spkid), "Testbody").ok());
    CHECK(w.value().finish(CborValue::make_map()).ok());
    return tf.path.string();
}

std::string write_catalog(const TempFile& tf, double a_au, uint64_t spkid = kSpkid) {
    catalog::Record r = make_record(a_au);
    r.spkid = spkid;
    return write_record(tf, r);
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
    long sun_index() const override { return 0; }

    spk::SpkFile file_;
    std::vector<int> ids_{10, 399, 5};
    std::vector<double> mus_{gm::kSun, gm::kEarth, gm::kJupiter};
    bool bad = false;
};

// JPL's J2000 ecliptic frame (SBDB elements, Horizons, SPICE ECLIPJ2000):
// the ICRF rotated about x by the IAU 1976 obliquity, no frame bias.
constexpr double kJplEclipticObliquity = 84381.448 / 3600.0 * 3.14159265358979323846 / 180.0;

// The engine's seed construction, rebuilt independently: elements ->
// heliocentric state in JPL's J2000 ecliptic, rotated to ICRF by the
// transpose of R1(84381.448"), then translated by the Sun's barycentric
// state.
State oracle_seed(spk::SpkFile& file, const Elements& els, double epoch = kEpoch) {
    auto helio = elements_to_state(gm::kSun, els);
    CHECK(helio.ok());
    const double c = std::cos(kJplEclipticObliquity), s = std::sin(kJplEclipticObliquity);
    const double m[9] = {1, 0, 0, 0, c, s, 0, -s, c};

    double sun[6];
    CHECK(file.state(10, 0, epoch, sun).ok());
    State out;
    const double rh[3] = {helio.value().pos.x, helio.value().pos.y, helio.value().pos.z};
    const double vh[3] = {helio.value().vel.x, helio.value().vel.y, helio.value().vel.z};
    for (int i = 0; i < 3; ++i) {
        (&out.pos.x)[i] = m[i] * rh[0] + m[3 + i] * rh[1] + m[6 + i] * rh[2] + sun[i] / kAuKm;
        (&out.vel.x)[i] = m[i] * vh[0] + m[3 + i] * vh[1] + m[6 + i] * vh[2] + sun[3 + i] / kAuKm;
    }
    return out;
}

TEST_CASE("engine_overlay_matches_independent_integration") {
    TempFile tf_kernel("cat-kernel");
    TempFile tf_cat("cat-overlay");
    Engine e = open_synthetic(tf_kernel);
    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());

    auto spk = spk::SpkFile::open(tf_kernel.path.string());
    CHECK(spk.ok());
    KernelPerturbers pert(std::move(spk).value());
    BarycentricForce force{&pert};
    const State seed = oracle_seed(pert.file_, kEls);

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

TEST_CASE("engine_overlay_speeds_and_consistency") {
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
    CHECK(tp.ok());
    CHECK(tm.ok());
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

TEST_CASE("engine_overlay_provenance_and_errors") {
    TempFile tf_kernel("cat-err-k");
    TempFile tf_cat("cat-err-c");
    Engine e = open_synthetic(tf_kernel);

    // Before any catalog: the body is simply not known.
    auto res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    CHECK(!res.ok());
    CHECK(res.error().code == ErrorCode::NotFound);

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
    CHECK(!missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("engine_overlay_newest_catalog_wins") {
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

// ---------------------------------------------------------------------------
// Part A3: sigma_arcsec — element sigmas propagated to a sky-plane
// uncertainty through the same integration that produces the position.
// ---------------------------------------------------------------------------

CalcOptions bary_geom_icrf() {
    CalcOptions o = CalcOptions::geometric();
    o.center = Center::Barycentric;
    o.frame = Frame::ICRF;
    o.coords = Coords::Equatorial;
    o.speed = false;
    return o;
}

// The engine's projection convention, rebuilt for the oracles: larger
// eigenvalue of the 2x2 tangent-plane covariance, square-rooted, divided
// by the observer->body distance, in arcsec. cov packed xx, xy, xz, yy,
// yz, zz; u_unit the unit line of sight.
double sky_sigma_arcsec(const double cov[6], const double u_unit[3], double dist_au) {
    const double au[3] = {std::fabs(u_unit[0]), std::fabs(u_unit[1]), std::fabs(u_unit[2])};
    double axis[3] = {0, 0, 0};
    axis[au[0] <= au[1] && au[0] <= au[2] ? 0 : (au[1] <= au[2] ? 1 : 2)] = 1.0;
    double e1[3] = {u_unit[1] * axis[2] - u_unit[2] * axis[1],
                    u_unit[2] * axis[0] - u_unit[0] * axis[2],
                    u_unit[0] * axis[1] - u_unit[1] * axis[0]};
    const double n1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (double& x : e1)
        x /= n1;
    const double e2[3] = {u_unit[1] * e1[2] - u_unit[2] * e1[1],
                          u_unit[2] * e1[0] - u_unit[0] * e1[2],
                          u_unit[0] * e1[1] - u_unit[1] * e1[0]};
    auto q = [&](const double a[3], const double b[3]) {
        return cov[0] * a[0] * b[0] + cov[3] * a[1] * b[1] + cov[5] * a[2] * b[2] +
               cov[1] * (a[0] * b[1] + a[1] * b[0]) + cov[2] * (a[0] * b[2] + a[2] * b[0]) +
               cov[4] * (a[1] * b[2] + a[2] * b[1]);
    };
    const double A = q(e1, e1), B = q(e1, e2), C = q(e2, e2);
    const double disc = std::sqrt(std::max(0.0, (A - C) * (A - C) + 4.0 * B * B));
    return std::sqrt(std::max(0.5 * (A + C + disc), 0.0)) / dist_au *
           (180.0 / 3.14159265358979323846) * 3600.0;
}

constexpr double kPi = 3.14159265358979323846;

// Osculating elements at TDB JD t -> JPL cometary {e, q, tp, node, peri, i}
// (elliptic; tp is the perihelion passage nearest before or after t).
void to_cometary(const Elements& el, double t, double out[6]) {
    const double n = std::sqrt(gm::kSun / (el.a * el.a * el.a));
    out[0] = el.e;
    out[1] = el.a * (1.0 - el.e);
    out[2] = t - std::remainder(el.mean_anom, 2.0 * kPi) / n;
    out[3] = el.node;
    out[4] = el.argp;
    out[5] = el.inc;
}

// Adds a covariance block to r: epoch t, cometary elements of `el` at t,
// and the full symmetric 6x6 matrix c.
void set_covariance(catalog::Record& r, const Elements& el, double t, const double c[6][6]) {
    r.flags |= catalog::RecordFlags::kCovariance;
    r.cov_epoch_jtdb = t;
    to_cometary(el, t, r.cov_elements);
    for (int i = 0; i < 6; ++i)
        for (int j = i; j < 6; ++j)
            r.covariance[catalog::packed_index(i, j)] = c[i][j];
}

// A covariance with the given standard deviations and correlations
// rho_ij = rho^|i-j| (sign alternating): positive definite for |rho| < 1.
void correlated(const double sd[6], double rho, double c[6][6]) {
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) {
            const int k = std::abs(i - j);
            c[i][j] = sd[i] * sd[j] * std::pow(rho, k) * ((i + j) % 2 ? -1.0 : 1.0) *
                      ((i - j) % 2 ? -1.0 : 1.0);
        }
}

TEST_CASE("sigma_absent_zero_and_planetary") {
    TempFile tf_kernel("sig-0-k");
    TempFile tf_cat("sig-0-c");
    Engine e = open_synthetic(tf_kernel);

    // Element sigmas without a covariance: uncorrelated summaries give no
    // calibrated answer, so sigma is absent; the position is unaffected.
    catalog::Record plain = make_record(kEls.a); // kSigmas set
    CHECK(e.add_catalog(write_record(tf_cat, plain)).ok());
    auto res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    REQUIRE(res.ok());
    CHECK(!res.value().sigma_arcsec.has_value());

    // A covariance of zeros: exactly zero, not absent.
    catalog::Record zero = make_record(kEls.a);
    const double c0[6][6] = {};
    set_covariance(zero, kEls, kEpoch, c0);
    CHECK(e.add_catalog(write_record(tf_cat, zero)).ok());
    res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    REQUIRE(res.ok());
    REQUIRE(res.value().sigma_arcsec.has_value());
    CHECK(*res.value().sigma_arcsec == 0.0);

    // The planetary ephemeris publishes no covariance: absent.
    auto earth = e.calc(body::kEarth, kEpoch + 10.0, bary_geom_icrf());
    REQUIRE(earth.ok());
    CHECK(!earth.value().sigma_arcsec.has_value());
}

TEST_CASE("sigma_tp_analytic_at_covariance_epoch") {
    TempFile tf_kernel("sig-tp-k");
    TempFile tf_cat("sig-tp-c");
    Engine e = open_synthetic(tf_kernel);

    // Only the perihelion time is uncertain: dM = -n dtp moves the body
    // along its velocity, dr = -v sigma_tp, so at the covariance epoch the
    // direction uncertainty is sigma_tp |u x v| / |r_bary| — closed form.
    const Elements el{2.2, 0.1, 0.2, 0.7, 1.1, 1.3};
    catalog::Record r = make_record(el.a);
    r.e = el.e;
    r.inc_rad = el.inc;
    r.node_rad = el.node;
    r.argp_rad = el.argp;
    r.mean_anom_rad = el.mean_anom;
    const double sigma_tp = 1e-3; // days
    double c[6][6] = {};
    c[2][2] = sigma_tp * sigma_tp;
    set_covariance(r, el, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    auto helio = elements_to_state(gm::kSun, el);
    REQUIRE(helio.ok());
    const double ce = std::cos(kJplEclipticObliquity), se = std::sin(kJplEclipticObliquity);
    const double m[9] = {1, 0, 0, 0, ce, se, 0, -se, ce};
    auto sunres = e.calc(body::kSun, kEpoch, bary_geom_icrf());
    REQUIRE(sunres.ok());
    const double* sun_au = sunres.value().pos.xyz_au;
    const double rh[3] = {helio.value().pos.x, helio.value().pos.y, helio.value().pos.z};
    const double vh[3] = {helio.value().vel.x, helio.value().vel.y, helio.value().vel.z};
    double v[3], rbar[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = m[i] * vh[0] + m[3 + i] * vh[1] + m[6 + i] * vh[2];
        rbar[i] = m[i] * rh[0] + m[3 + i] * rh[1] + m[6 + i] * rh[2] + sun_au[i];
    }
    const double d = std::sqrt(rbar[0] * rbar[0] + rbar[1] * rbar[1] + rbar[2] * rbar[2]);
    const double cx[3] = {rbar[1] * v[2] - rbar[2] * v[1], rbar[2] * v[0] - rbar[0] * v[2],
                          rbar[0] * v[1] - rbar[1] * v[0]};
    const double expect = sigma_tp * std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]) / d /
                          d * (180.0 / kPi) * 3600.0;

    auto res = e.calc(int(kSpkid), kEpoch, bary_geom_icrf());
    REQUIRE(res.ok());
    REQUIRE(res.value().sigma_arcsec.has_value());
    std::printf("  tp-only at the covariance epoch: %.6f\" (analytic %.6f\")\n",
                *res.value().sigma_arcsec, expect);
    CHECK(std::fabs(*res.value().sigma_arcsec - expect) < 1e-4 * expect);
}

TEST_CASE("sigma_matches_independent_jcjt") {
    TempFile tf_kernel("sig-ind-k");
    TempFile tf_cat("sig-ind-c");
    Engine e = open_synthetic(tf_kernel);

    // A fully correlated covariance published at its own epoch, 300 days
    // before the record's element epoch. The oracle does not decompose it:
    // it builds J column by column (a finite difference per cometary
    // element, each track integrated from the covariance epoch against the
    // kernel read per evaluation) and forms J C J^T explicitly.
    const double t_cov = kEpoch - 300.0;
    const double sd[6] = {2e-6, 3e-6, 2e-3, 1e-6, 4e-6, 1e-6};
    double c[6][6];
    correlated(sd, 0.6, c);
    catalog::Record r = make_record(kEls.a);
    set_covariance(r, kEls, t_cov, c);
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    auto spk = spk::SpkFile::open(tf_kernel.path.string());
    REQUIRE(spk.ok());
    KernelPerturbers pert(std::move(spk).value());
    BarycentricForce force{&pert};
    const double* x0 = r.cov_elements;

    auto elements_of = [&](const double x[6]) {
        const double a = x[1] / (1.0 - x[0]);
        const double n = std::sqrt(gm::kSun / (a * a * a));
        return Elements{a, x[0], x[5], x[3], x[4], std::remainder(n * (t_cov - x[2]), 2 * kPi)};
    };

    for (double dt : {0.0, 700.25, -900.5}) {
        const double t = kEpoch + dt;
        auto res = e.calc(int(kSpkid), t, bary_geom_icrf());
        REQUIRE(res.ok());
        REQUIRE(res.value().sigma_arcsec.has_value());

        double jac[3][6];
        for (int k = 0; k < 6; ++k) {
            const double h = 3.0 * sd[k];
            double rp[3], rm[3];
            for (int sign : {+1, -1}) {
                double x[6];
                for (int i = 0; i < 6; ++i)
                    x[i] = x0[i];
                x[k] += sign * h;
                const State seed = oracle_seed(pert.file_, elements_of(x), t_cov);
                double y[6];
                to_array(seed, y);
                IntegrateStats stats;
                CHECK(integrate_dp54(y, t_cov, t, force, IntegrateOptions{}, &stats).ok());
                for (int i = 0; i < 3; ++i)
                    (sign > 0 ? rp : rm)[i] = y[i];
            }
            for (int i = 0; i < 3; ++i)
                jac[i][k] = (rp[i] - rm[i]) / (2.0 * h);
        }
        double full[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int a = 0; a < 6; ++a)
                    for (int b = 0; b < 6; ++b)
                        full[i][j] += jac[i][a] * c[a][b] * jac[j][b];
        const double cov[6] = {full[0][0], full[0][1], full[0][2],
                               full[1][1], full[1][2], full[2][2]};
        double u[3] = {res.value().pos.xyz_au[0], res.value().pos.xyz_au[1],
                       res.value().pos.xyz_au[2]};
        const double n = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& x : u)
            x /= n;
        const double expect = sky_sigma_arcsec(cov, u, n);
        std::printf("  dt=%+g: engine %.6f\", oracle J C J^T %.6f\"\n", dt,
                    *res.value().sigma_arcsec, expect);
        CHECK(std::fabs(*res.value().sigma_arcsec - expect) < 2e-3 * expect);
    }
}

TEST_CASE("sigma_correlations_matter") {
    TempFile tf_kernel("sig-corr-k");
    TempFile tf_a("sig-corr-a");
    TempFile tf_b("sig-corr-b");
    Engine e = open_synthetic(tf_kernel);

    // Nearly coplanar orbit: node and argument of perihelion are each poorly
    // known, but their sum (the longitude of perihelion) is well known — the
    // variances alone overstate the uncertainty, the anticorrelation
    // removes it.
    const Elements el{2.5, 0.2, 0.01, 0.7, 1.1, 1.3};
    catalog::Record r = make_record(el.a);
    r.e = el.e;
    r.inc_rad = el.inc;
    r.node_rad = el.node;
    r.argp_rad = el.argp;
    r.mean_anom_rad = el.mean_anom;
    const double sd = 1e-4, t = kEpoch + 500.0;
    double c[6][6] = {};
    c[3][3] = c[4][4] = sd * sd;
    set_covariance(r, el, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_a, r)).ok());
    auto independent = e.calc(int(kSpkid), t, bary_geom_icrf());

    c[3][4] = c[4][3] = -0.99999 * sd * sd;
    set_covariance(r, el, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_b, r)).ok());
    auto correlated_res = e.calc(int(kSpkid), t, bary_geom_icrf());
    REQUIRE(independent.ok());
    REQUIRE(correlated_res.ok());
    REQUIRE(independent.value().sigma_arcsec.has_value());
    REQUIRE(correlated_res.value().sigma_arcsec.has_value());
    const double si = *independent.value().sigma_arcsec;
    const double sc = *correlated_res.value().sigma_arcsec;
    std::printf("  uncorrelated %.4f\", anticorrelated node/peri %.4f\"\n", si, sc);
    CHECK(sc < 0.05 * si);
}

TEST_CASE("sigma_growth_symmetry") {
    TempFile tf_kernel("sig-grw-k");
    TempFile tf_cat("sig-grw-c");
    Engine e = open_synthetic(tf_kernel);

    // Perihelion distance only (e fixed): the semimajor axis and so the
    // mean motion are uncertain, and the along-track spread grows ~linearly
    // in |t - epoch|, near-symmetrically in time.
    catalog::Record r = make_record(kEls.a);
    double c[6][6] = {};
    c[1][1] = 1e-6 * 1e-6;
    set_covariance(r, kEls, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    auto at = [&](double t) {
        auto res = e.calc(int(kSpkid), t, bary_geom_icrf());
        CHECK(res.ok());
        CHECK(res.value().sigma_arcsec.has_value());
        return res.value().sigma_arcsec.value_or(0.0);
    };
    const double s0 = at(kEpoch), sp = at(kEpoch + 2000.0), sm = at(kEpoch - 2000.0);
    std::printf("  epoch %.4f\", +2000d %.4f\", -2000d %.4f\"\n", s0, sp, sm);
    CHECK(s0 > 0.0);
    CHECK(sp > 3.0 * s0);
    CHECK(sm > 3.0 * s0);
    CHECK(std::fabs(sp - sm) < 0.4 * 0.5 * (sp + sm));
}

TEST_CASE("sigma_newest_catalog_rescales") {
    TempFile tf_kernel("sig-nc-k");
    TempFile tf_a("sig-nc-a");
    TempFile tf_b("sig-nc-b");
    Engine e = open_synthetic(tf_kernel);

    // Same elements, the covariance times 100: the cached tracks are
    // invalidated by add_catalog and rebuilt, and the (linear) answer
    // scales by ten.
    const double sd[6] = {1e-6, 1e-6, 1e-3, 1e-6, 1e-6, 1e-6};
    double c[6][6];
    correlated(sd, 0.3, c);
    catalog::Record r = make_record(kEls.a);
    set_covariance(r, kEls, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_a, r)).ok());
    const double t = kEpoch + 300.0;
    auto first = e.calc(int(kSpkid), t, bary_geom_icrf());
    REQUIRE(first.ok());
    REQUIRE(first.value().sigma_arcsec.has_value());

    for (auto& row : c)
        for (double& x : row)
            x *= 100.0;
    set_covariance(r, kEls, kEpoch, c);
    CHECK(e.add_catalog(write_record(tf_b, r)).ok());
    auto second = e.calc(int(kSpkid), t, bary_geom_icrf());
    REQUIRE(second.ok());
    REQUIRE(second.value().sigma_arcsec.has_value());

    const double ratio = *second.value().sigma_arcsec / *first.value().sigma_arcsec;
    std::printf("  sigma ratio after 100x covariance: %.6f\n", ratio);
    CHECK(ratio > 9.99);
    CHECK(ratio < 10.01);
    // The elements did not change: same position as before.
    CHECK(second.value().pos.dist_au == first.value().pos.dist_au);
}

// ---------------------------------------------------------------------------
// Asteroid perturber kernels (add_perturbers).
// ---------------------------------------------------------------------------

// The engine's masses plus an asteroid perturber kernel, read per evaluation
// for an independent integration: kernel asteroids are heliocentric, so the
// main kernel's Sun is added. `exclude` leaves one id out (the body itself).
struct KernelPlusAsteroids : PerturberStates {
    KernelPlusAsteroids(spk::SpkFile main, spk::SpkFile ast, int asteroid_kernel_id, double mu,
                        long exclude)
        : main_(std::move(main)), ast_(std::move(ast)), ast_id_(asteroid_kernel_id) {
        ids_ = {10, 399, 5};
        mus_ = {gm::kSun, gm::kEarth, gm::kJupiter};
        if (exclude != 20000000L + (asteroid_kernel_id - 2000000L)) {
            ids_.push_back(asteroid_kernel_id);
            mus_.push_back(mu);
        }
    }
    void ensure(double) override {}
    void state(size_t i, double t, double out[6]) override {
        if (ids_[i] == ast_id_) {
            double sun[6];
            bad = bad || !ast_.state(ast_id_, 10, t, out).ok() || !main_.state(10, 0, t, sun).ok();
            for (int k = 0; k < 6; ++k)
                out[k] = (out[k] + sun[k]) / kAuKm;
            return;
        }
        bad = bad || !main_.state(ids_[i], 0, t, out).ok();
        for (int k = 0; k < 6; ++k)
            out[k] /= kAuKm;
    }
    size_t count() const override { return ids_.size(); }
    const double* mus() const override { return mus_.data(); }
    bool ok() const override { return !bad; }
    long sun_index() const override { return 0; }

    spk::SpkFile main_, ast_;
    int ast_id_;
    std::vector<int> ids_;
    std::vector<double> mus_;
    bool bad = false;
};

TEST_CASE("perturber_kernel_matches_independent_integration") {
    TempFile tf_kernel("pert-k");
    TempFile tf_ast("pert-a");
    TempFile tf_cat("pert-c");
    Engine e = open_synthetic(tf_kernel);

    // Two catalog bodies on the same orbit: 20000002 (not in the perturber
    // kernel) and 20000001, which the kernel carries as 2000001 (Ceres'
    // mass): the second must leave itself out.
    const int kAstKernelId = 2000001;
    auto body_helio = elements_to_state(gm::kSun, kEls);
    REQUIRE(body_helio.ok());
    const double ce = std::cos(kJplEclipticObliquity), se = std::sin(kJplEclipticObliquity);
    const double m[9] = {1, 0, 0, 0, ce, se, 0, -se, ce};
    const double rh[3] = {body_helio.value().pos.x, body_helio.value().pos.y,
                          body_helio.value().pos.z};
    // The asteroid sits 0.02 AU from the bodies' epoch position (ICRF,
    // heliocentric), nearly at rest: km-scale pulls over a few hundred days.
    synth::LinearBody ast{kAstKernelId, {}, {0.001, -0.002, 0.0005}, 10};
    const double et0 = synth::et_of_tdb(kEpoch);
    for (int i = 0; i < 3; ++i) {
        const double icrf = m[i] * rh[0] + m[3 + i] * rh[1] + m[6 + i] * rh[2];
        ast.p[i] = (icrf + 0.02 * (i == 2 ? 1.0 : 0.0)) * kAuKm - ast.v[i] * et0;
    }
    synth::write_linear_spk(tf_ast.path, {ast});

    // Error paths first: nothing to add from a planetary kernel or a
    // missing file.
    auto not_asteroids = e.add_perturbers(tf_kernel.path.string());
    CHECK(!not_asteroids.ok());
    CHECK(not_asteroids.error().code == ErrorCode::FormatError);
    CHECK(e.add_perturbers("/nonexistent/sb.bsp").error().code == ErrorCode::IoError);

    {
        auto w = catalog::Writer::create(tf_cat.path.string(), catalog::WriterOptions{});
        REQUIRE(w.ok());
        catalog::Record r1 = make_record(kEls.a);
        r1.spkid = 20000001;
        catalog::Record r2 = make_record(kEls.a);
        r2.spkid = 20000002;
        CHECK(w.value().add(r1, "1").ok());
        CHECK(w.value().add(r2, "2").ok());
        CHECK(w.value().finish(CborValue::make_map()).ok());
    }
    CHECK(e.add_catalog(tf_cat.path.string()).ok());
    const double t = kEpoch + 400.0;
    auto before = e.calc(20000002, t, bary_geom_icrf());
    REQUIRE(before.ok());

    CHECK(e.add_perturbers(tf_ast.path.string()).ok());
    auto self = e.calc(20000001, t, bary_geom_icrf());
    auto other = e.calc(20000002, t, bary_geom_icrf());
    REQUIRE(self.ok());
    REQUIRE(other.ok());
    CHECK(std::string(other.value().provenance.source).find("asteroid perturbers") !=
          std::string::npos);

    auto oracle = [&](long body_id) {
        auto main = spk::SpkFile::open(tf_kernel.path.string());
        auto astf = spk::SpkFile::open(tf_ast.path.string());
        REQUIRE(main.ok());
        REQUIRE(astf.ok());
        KernelPlusAsteroids pert(std::move(main).value(), std::move(astf).value(), kAstKernelId,
                                 gm::asteroid(1), body_id);
        BarycentricForce force{&pert};
        State seed = oracle_seed(pert.main_, kEls);
        double y[6];
        to_array(seed, y);
        IntegrateStats stats;
        CHECK(integrate_dp54(y, kEpoch, t, force, IntegrateOptions{}, &stats).ok());
        CHECK(pert.ok());
        return std::array<double, 3>{y[0], y[1], y[2]};
    };
    const auto expect_other = oracle(20000002);
    const auto expect_self = oracle(20000001);
    double d_other = 0.0, d_self = 0.0, pull = 0.0;
    for (int i = 0; i < 3; ++i) {
        d_other += std::pow(other.value().pos.xyz_au[i] - expect_other[i], 2);
        d_self += std::pow(self.value().pos.xyz_au[i] - expect_self[i], 2);
        pull += std::pow(other.value().pos.xyz_au[i] - before.value().pos.xyz_au[i], 2);
    }
    std::printf("  asteroid pull %.3g AU; engine vs oracle: perturbed %.3g AU, "
                "self-excluded %.3g AU\n",
                std::sqrt(pull), std::sqrt(d_other), std::sqrt(d_self));
    CHECK(std::sqrt(pull) > 1e-7);    // the kernel's mass acts
    CHECK(std::sqrt(d_other) < 1e-8); // and exactly as the oracle integrates it
    CHECK(std::sqrt(d_self) < 1e-8);  // the body never perturbs itself
}

TEST_CASE("lookup_pdes_name_case") {
    TempFile tf_kernel("lkp-1-k");
    TempFile tf_cat("lkp-1-c");
    Engine e = open_synthetic(tf_kernel);

    // Before any catalog: nothing answers (NotFound, not a machinery
    // error).
    auto none = e.lookup("Testbody");
    CHECK(!none.ok());
    CHECK(none.error().code == ErrorCode::NotFound);

    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());
    const std::string pdes = std::to_string(kSpkid);
    for (const char* q : {pdes.c_str(), "Testbody", "testbody", "TESTBODY"}) {
        auto id = e.lookup(q);
        CHECK(id.ok());
        CHECK(id.value() == int(kSpkid));
    }
    auto miss = e.lookup("No such name");
    CHECK(!miss.ok());
    CHECK(miss.error().code == ErrorCode::NotFound);

    // The name resolves to a body calc() answers identically.
    const double t = kEpoch + 200.0;
    auto by_id = e.calc(int(kSpkid), t, bary_geom_icrf());
    auto by_name = e.calc(e.lookup("testbody").value(), t, bary_geom_icrf());
    CHECK(by_id.ok());
    CHECK(by_name.ok());
    CHECK(by_name.value().pos.lon_deg == by_id.value().pos.lon_deg);
    CHECK(by_name.value().pos.lat_deg == by_id.value().pos.lat_deg);
    CHECK(by_name.value().pos.dist_au == by_id.value().pos.dist_au);
}

TEST_CASE("lookup_newest_catalog_wins") {
    TempFile tf_kernel("lkp-2-k");
    TempFile tf_a("lkp-2-a");
    TempFile tf_b("lkp-2-b");
    Engine e = open_synthetic(tf_kernel);

    CHECK(e.add_catalog(write_catalog(tf_a, kEls.a, kSpkid)).ok());
    CHECK(e.add_catalog(write_catalog(tf_b, kEls.a + 0.01, kSpkid + 1)).ok());
    // Both records carry the proper name "Testbody": the newest
    // catalog answers it.
    auto id = e.lookup("Testbody");
    CHECK(id.ok());
    CHECK(id.value() == int(kSpkid + 1));
    // The older catalog's designation still resolves.
    auto old = e.lookup(std::to_string(kSpkid));
    CHECK(old.ok());
    CHECK(old.value() == int(kSpkid));
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

TEST_CASE("de440_ceres_vs_swetest") {
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto e = Engine::open(de);
    REQUIRE(e.ok());
    CHECK(e.value()
              .add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm")
              .ok());

    // The name index on real data: SBDB's designation "1" and the proper
    // name "Ceres" both answer the fixture's SPK-ID.
    auto by_name = e.value().lookup("Ceres");
    CHECK((by_name.ok() && by_name.value() == kCatalogFixtures[0].spkid));
    auto by_pdes = e.value().lookup("1");
    CHECK((by_pdes.ok() && by_pdes.value() == kCatalogFixtures[0].spkid));

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

        // The sample catalog carries SBDB's element sigmas but no
        // covariance: no calibrated uncertainty, so none is reported.
        CHECK(!app.value().sigma_arcsec.has_value());

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

TEST_CASE("de440_ceres_osculating_orbit_points_match_elements") {
    // At its element epoch Ceres is seeded from the catalog's heliocentric
    // elements, so its osculating nodes and apsides must reproduce them.
    // Residuals: the output frame's J2000 ecliptic (IAU 2006 obliquity plus
    // frame bias) is 0.04" from JPL's, and the engine adds Ceres' own mass.
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    const std::string cat = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    auto e = Engine::open(de);
    REQUIRE(e.ok());
    REQUIRE(e.value().add_catalog(cat).ok());
    auto reader = catalog::Reader::open(cat);
    REQUIRE(reader.ok());
    catalog::Record ceres{};
    REQUIRE(reader.value()
                .for_each([&](const catalog::Record& r, const catalog::Names&) {
                    if (r.spkid == 20000001)
                        ceres = r;
                })
                .ok());
    REQUIRE(ceres.spkid == 20000001);

    CalcOptions o = CalcOptions::geometric();
    o.center = Center::Heliocentric;
    o.frame = Frame::J2000;
    o.speed = false;
    const double jd_tt = time::tt_from_tdb(ceres.epoch_jtdb);
    const auto point = [&](OrbitPoint p) {
        auto r = e.value().calc_orbit_point(20000001, p, OrbitElements::Osculating, jd_tt, o);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        return r.value().pos;
    };
    const double kDeg = 180.0 / 3.14159265358979323846;
    const auto arcsec = [](double a, double b) {
        return std::fabs(std::remainder(a - b, 360.0)) * 3600.0;
    };
    const Position asc = point(OrbitPoint::AscendingNode);
    const Position peri = point(OrbitPoint::Perihelion);
    const Position aph = point(OrbitPoint::Aphelion);
    // SBDB's node is on the J2000 ecliptic; ours is on the ecliptic of date
    // (3.5a, amended 2026-09-18), a different point of the same orbit. It
    // lies on that ecliptic, and within the ecliptic's motion since J2000 of
    // SBDB's value (the plane tilts ~47"/century; over a 10.6 deg inclined
    // orbit the node slides by up to a few hundred arcsec).
    {
        CalcOptions od = o;
        od.frame = Frame::MeanOfDate;
        auto rd = e.value().calc_orbit_point(20000001, OrbitPoint::AscendingNode,
                                             OrbitElements::Osculating, jd_tt, od);
        REQUIRE(rd.ok());
        CHECK(std::fabs(rd.value().pos.lat_deg) * 3600.0 < 1e-6);
    }
    CHECK(arcsec(asc.lon_deg, ceres.node_rad * kDeg) < 300.0);
    // Perihelion direction from the elements.
    const double w = ceres.argp_rad, i = ceres.inc_rad, node = ceres.node_rad;
    const double peri_lon = node * kDeg + std::atan2(std::sin(w) * std::cos(i), std::cos(w)) * kDeg;
    const double peri_lat = std::asin(std::sin(w) * std::sin(i)) * kDeg;
    CHECK(arcsec(peri.lon_deg, peri_lon) < 0.5);
    CHECK(std::fabs(peri.lat_deg - peri_lat) * 3600.0 < 0.5);
    CHECK(std::fabs(peri.dist_au - ceres.a_au * (1.0 - ceres.e)) < 1e-6);
    CHECK(std::fabs(aph.dist_au - ceres.a_au * (1.0 + ceres.e)) < 1e-6);
    std::printf("  Ceres node %.3f\" peri %.3f\" q %.2e AU\n",
                arcsec(asc.lon_deg, ceres.node_rad * kDeg), arcsec(peri.lon_deg, peri_lon),
                std::fabs(peri.dist_au - ceres.a_au * (1.0 - ceres.e)));
}

TEST_CASE("de440_mean_orbit_points") {
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto e = Engine::open(de);
    REQUIRE(e.ok());
    REQUIRE(e.value()
                .add_catalog(std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm")
                .ok());
    const double kDeg = 180.0 / 3.14159265358979323846;
    const auto arcsec = [](double a, double b) {
        return std::fabs(std::remainder(a - b, 360.0)) * 3600.0;
    };

    // The Moon's mean node is the fundamental argument Omega itself, on the
    // mean ecliptic and equinox of date, regressing 0.05295 degrees a day.
    CalcOptions date = CalcOptions::geometric();
    date.frame = Frame::MeanOfDate;
    for (double jd : {2378496.5, 2451545.0, 2461300.5}) {
        auto node = e.value().calc_orbit_point(body::kMoon, OrbitPoint::AscendingNode,
                                               OrbitElements::Mean, jd, date);
        REQUIRE_MESSAGE(node.ok(), node.error().message);
        double phi[14];
        frames::fundamental_arguments(jd, phi);
        CHECK(arcsec(node.value().pos.lon_deg, phi[13] * kDeg) < 1e-6);
        CHECK(std::fabs(node.value().pos.lat_deg) < 1e-9);
        CHECK(std::fabs(node.value().pos.lon_speed + 0.052954) < 2e-5);
        auto peri = e.value().calc_orbit_point(body::kMoon, OrbitPoint::Perihelion,
                                               OrbitElements::Mean, jd, date);
        REQUIRE(peri.ok());
        CHECK(std::fabs(peri.value().pos.dist_au * 149597870.7 - 384399.0 * (1 - 0.0549006)) < 1.0);
    }

    // Planets at J2000 against the fit's own values (heliocentric, J2000
    // ecliptic): the published mean elements are Mercury node 48.331 deg,
    // Mars node 49.558 deg, the Earth-Moon barycentre's perihelion 102.937 deg.
    CalcOptions h = CalcOptions::geometric();
    h.center = Center::Heliocentric;
    h.frame = Frame::J2000;
    const auto mean = [&](int id, OrbitPoint p) {
        auto r = e.value().calc_orbit_point(id, p, OrbitElements::Mean, 2451545.0, h);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        return r.value().pos;
    };
    CHECK(arcsec(mean(199, OrbitPoint::AscendingNode).lon_deg, 48.3309) < 5.0);
    CHECK(arcsec(mean(4, OrbitPoint::AscendingNode).lon_deg, 49.5581) < 5.0);
    CHECK(arcsec(mean(body::kEarth, OrbitPoint::Perihelion).lon_deg, 102.9371) < 5.0);
    CHECK(std::fabs(mean(body::kEarth, OrbitPoint::Perihelion).dist_au - 0.98329) < 1e-5);
    // Planet-centre IDs share their system's elements.
    CHECK(mean(499, OrbitPoint::Aphelion).lon_deg == mean(4, OrbitPoint::Aphelion).lon_deg);
    // Mean elements exist for the Moon and the major planets only.
    CHECK(e.value()
              .calc_orbit_point(20000001, OrbitPoint::Perihelion, OrbitElements::Mean, 2451545.0, h)
              .error()
              .code == ErrorCode::NotFound);
}

TEST_CASE("de440_orbit_points_carry_the_observer_velocity_term") {
    // A node is asked for so it can be compared against apparent body
    // positions, so it is answered in the frame those are in: the same light
    // time, deflection and aberration a body gets, applied when the caller
    // asks for them. The magnitudes matter and are not intuitive, which is
    // why they are pinned here rather than left to a tolerance.
    //
    // Agreed with the Astrolog side for protocol v4 (docs/ORBIT-POINTS.md);
    // their Swiss-backed server measures 20.8370" for Jupiter's ascending
    // node against the 20.837" below, reached by a different route.
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();
    const double jd = 2451545.0;

    CalcOptions geom = CalcOptions::geometric();
    geom.speed = false;
    CalcOptions lt = geom;
    lt.light_time = true;
    CalcOptions apparent = lt;
    apparent.aberration = true;

    const auto sep = [](const Position& a, const Position& b) {
        const double *p = a.xyz_au, *q = b.xyz_au;
        const double dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
        const double cx[3] = {p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2],
                              p[0] * q[1] - p[1] * q[0]};
        const double cross = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
        return std::atan2(cross, dot) * (180.0 / 3.14159265358979323846) * 3600.0;
    };
    const auto at = [&](int id, OrbitPoint p, OrbitElements el, const CalcOptions& o) {
        auto r = e.calc_orbit_point(id, p, el, jd, o);
        REQUIRE_MESSAGE(r.ok(), r.error().message);
        return r.value().pos;
    };

    // A point of a distant orbit is very nearly fixed in inertial space, so
    // light time moves it by almost nothing and the observer's velocity moves
    // it by the full aberration constant. The two differ by four orders of
    // magnitude, which is the whole reason the distinction had to be settled.
    struct Far {
        const char* name;
        int id;
        OrbitPoint point;
        OrbitElements elements;
        double want_arcsec;
    };
    const Far far[] = {
        {"Jupiter asc node (oscu)", 5, OrbitPoint::AscendingNode, OrbitElements::Osculating,
         20.837},
        {"Jupiter asc node (mean)", 5, OrbitPoint::AscendingNode, OrbitElements::Mean, 20.843},
        {"Saturn asc node (oscu)", 6, OrbitPoint::AscendingNode, OrbitElements::Osculating, 20.145},
        // Near the apex, so the same constant times a small sine: small
        // because of WHERE it is, not because the correction is small.
        {"Jupiter perihelion (mean)", 5, OrbitPoint::Perihelion, OrbitElements::Mean, 2.673},
    };
    for (const Far& f : far) {
        const Position g = at(f.id, f.point, f.elements, geom);
        const Position l = at(f.id, f.point, f.elements, lt);
        const Position a = at(f.id, f.point, f.elements, apparent);
        CHECK_MESSAGE(sep(g, l) < 0.01, f.name);                            // light time: nothing
        CHECK_MESSAGE(std::fabs(sep(g, a) - f.want_arcsec) < 0.01, f.name); // aberration: all of it
    }

    // The Moon is the opposite case and the one a naive implementation gets
    // wrong. Its points are computed barycentrically like everything else, so
    // retarding them drags in the Earth's orbital motion: light time alone
    // moves the node 19", which aberration then very nearly takes back. Only
    // the sum means anything. An engine working geocentrically instead would
    // land on the same small total from a small light-time step, so the large
    // intermediate is pinned too -- it is what tells the two routes apart.
    struct Near {
        const char* name;
        OrbitPoint point;
        OrbitElements elements;
        double want_total_arcsec;
    };
    const Near close[] = {
        {"Moon true node", OrbitPoint::AscendingNode, OrbitElements::Osculating, 0.0029},
        {"Moon mean node", OrbitPoint::AscendingNode, OrbitElements::Mean, 0.0031},
        {"Moon oscu apogee", OrbitPoint::Aphelion, OrbitElements::Osculating, 0.0928},
    };
    for (const Near& n : close) {
        const Position g = at(body::kMoon, n.point, n.elements, geom);
        const Position l = at(body::kMoon, n.point, n.elements, lt);
        const Position a = at(body::kMoon, n.point, n.elements, apparent);
        CHECK_MESSAGE(sep(g, l) > 15.0, n.name); // the large intermediate
        CHECK_MESSAGE(std::fabs(sep(g, a) - n.want_total_arcsec) < 0.002, n.name);
    }

    // Corrections off is still exactly the geometry the elements test pins,
    // so asking for the geometric point remains free and exact.
    const Position g1 = at(5, OrbitPoint::AscendingNode, OrbitElements::Osculating, geom);
    const Position g2 =
        at(5, OrbitPoint::AscendingNode, OrbitElements::Osculating, CalcOptions::geometric());
    CHECK(sep(g1, g2) == 0.0);
}

} // namespace
