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

    spk::SpkFile file_;
    std::vector<int> ids_{10, 399, 5};
    std::vector<double> mus_{gm::kSun, gm::kEarth, gm::kJupiter};
    bool bad = false;
};

// The engine's seed construction, rebuilt independently: elements ->
// heliocentric state in the ecliptic of J2000, rotated to ICRF by the
// transpose of R1(eps0)*B, then translated by the Sun's barycentric state.
State oracle_seed(spk::SpkFile& file, const Elements& els) {
    auto helio = elements_to_state(gm::kSun, els);
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

TEST(sigma_absent_zero_and_planetary) {
    TempFile tf_kernel("sig-0-k");
    TempFile tf_cat("sig-0-c");
    Engine e = open_synthetic(tf_kernel);

    // No sigmas published: honestly absent, position unaffected.
    catalog::Record none = make_record(kEls.a);
    none.flags = 0;
    CHECK(e.add_catalog(write_record(tf_cat, none)).ok());
    auto res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    CHECK(res.ok());
    CHECK(!res.value().sigma_arcsec.has_value());

    // Sigmas present but all zero: exactly zero, not absent.
    catalog::Record zero = make_record(kEls.a);
    for (double& s : zero.sigmas)
        s = 0.0;
    CHECK(e.add_catalog(write_record(tf_cat, zero)).ok());
    res = e.calc(int(kSpkid), kEpoch + 10.0, CalcOptions::geometric());
    CHECK(res.ok());
    CHECK(res.value().sigma_arcsec.has_value());
    CHECK(*res.value().sigma_arcsec == 0.0);

    // The planetary ephemeris publishes no covariance: absent.
    auto earth = e.calc(body::kEarth, kEpoch + 10.0, bary_geom_icrf());
    CHECK(earth.ok());
    CHECK(!earth.value().sigma_arcsec.has_value());
}

TEST(sigma_circle_analytic_epoch) {
    TempFile tf_kernel("sig-circ-k");
    TempFile tf_cat("sig-circ-c");
    Engine e = open_synthetic(tf_kernel);

    // An exact circle (e = 0), sigma_M only: the covariance is rank one,
    // c = dr/dM = a * (unit tangent of the heliocentric circle), so
    // sigma = sigma_M * |u x c| / |r_bary| in arcsec — closed form. The
    // cross product with the (barycentric) line of sight u is what the
    // sky-plane projection leaves of c; u and c are built here from the
    // same pieces oracle_seed uses.
    catalog::Record r;
    r.spkid = kSpkid;
    r.epoch_jtdb = kEpoch;
    r.a_au = 2.0;
    r.e = 0.0;
    r.inc_rad = r.node_rad = r.argp_rad = 0.0;
    r.mean_anom_rad = 1.0471975511965976; // pi/3
    r.flags = catalog::RecordFlags::kSigmas;
    for (double& s : r.sigmas)
        s = 0.0;
    r.sigmas[5] = 1e-6;
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    const Elements circ{r.a_au, r.e, r.inc_rad, r.node_rad, r.argp_rad, r.mean_anom_rad};
    const double h = 1e-6; // the engine's step: sigma above its floor
    Elements ep = circ, em = circ;
    ep.mean_anom += h;
    em.mean_anom -= h;
    auto sp = elements_to_state(gm::kSun, ep);
    auto sm = elements_to_state(gm::kSun, em);
    CHECK(sp.ok() && sm.ok());
    const double c_ecl[3] = {(sp.value().pos.x - sm.value().pos.x) / (2 * h),
                             (sp.value().pos.y - sm.value().pos.y) / (2 * h),
                             (sp.value().pos.z - sm.value().pos.z) / (2 * h)};

    // Rotate c into ICRF by m^T and build the barycentric line of sight
    // from the seed construction (r_bary = m^T r_helio + sun).
    double b[9];
    frames::frame_bias_matrix(b);
    const double eps0 = frames::mean_obliquity(kJ2000);
    const double ce = std::cos(eps0), se = std::sin(eps0);
    const double r1[9] = {1, 0, 0, 0, ce, se, 0, -se, ce};
    double m[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[3 * i + j] = r1[3 * i] * b[j] + r1[3 * i + 1] * b[3 + j] + r1[3 * i + 2] * b[6 + j];
    double c[3], rbar[3];
    auto helio = elements_to_state(gm::kSun, circ);
    CHECK(helio.ok());
    const double rh[3] = {helio.value().pos.x, helio.value().pos.y, helio.value().pos.z};
    auto sunres = e.calc(body::kSun, kEpoch, bary_geom_icrf()); // kernel Sun, ICRF AU
    CHECK(sunres.ok());
    const double* sun_au = sunres.value().pos.xyz_au;
    for (int i = 0; i < 3; ++i) {
        c[i] = m[i] * c_ecl[0] + m[3 + i] * c_ecl[1] + m[6 + i] * c_ecl[2];
        rbar[i] = m[i] * rh[0] + m[3 + i] * rh[1] + m[6 + i] * rh[2] + sun_au[i];
    }
    const double d = std::sqrt(rbar[0] * rbar[0] + rbar[1] * rbar[1] + rbar[2] * rbar[2]);
    const double cx[3] = {rbar[1] * c[2] - rbar[2] * c[1], rbar[2] * c[0] - rbar[0] * c[2],
                          rbar[0] * c[1] - rbar[1] * c[0]};
    const double uc = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]) / d;
    const double expect = h * uc / d * (180.0 / 3.14159265358979323846) * 3600.0;

    auto res = e.calc(int(kSpkid), kEpoch, bary_geom_icrf());
    CHECK(res.ok());
    CHECK(res.value().sigma_arcsec.has_value());
    std::printf("  circle at epoch: %.6f\" (analytic %.6f\")\n", *res.value().sigma_arcsec, expect);
    CHECK(std::fabs(*res.value().sigma_arcsec - expect) < 1e-5 * expect);
}

TEST(sigma_matches_independent_fd) {
    TempFile tf_kernel("sig-ind-k");
    TempFile tf_cat("sig-ind-c");
    Engine e = open_synthetic(tf_kernel);

    // Two non-degenerate columns; both sigmas sit above the engine's FD
    // floor, so its difference step is the sigma itself.
    const double sig_a = 1e-5, sig_argp = 1e-5;
    catalog::Record r = make_record(kEls.a);
    for (double& s : r.sigmas)
        s = 0.0;
    r.sigmas[0] = sig_a;
    r.sigmas[4] = sig_argp;
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    auto spk = spk::SpkFile::open(tf_kernel.path.string());
    CHECK(spk.ok());
    KernelPerturbers pert(std::move(spk).value());
    BarycentricForce force{&pert};

    const double els0[6] = {kEls.a, kEls.e, kEls.inc, kEls.node, kEls.argp, kEls.mean_anom};
    const int cols[2] = {0, 4};
    const double sigs[2] = {sig_a, sig_argp};

    for (double dt : {400.25, -800.5}) {
        const double t = kEpoch + dt;
        auto res = e.calc(int(kSpkid), t, bary_geom_icrf());
        CHECK(res.ok());
        CHECK(res.value().sigma_arcsec.has_value());

        // Independent columns: propagate each perturbed seed with a
        // free-running dp54 against the kernel read per evaluation.
        double cov[6] = {0, 0, 0, 0, 0, 0};
        for (int ci = 0; ci < 2; ++ci) {
            const int k = cols[ci];
            const double s = sigs[ci];
            double rp[3], rm[3];
            for (int sign : {+1, -1}) {
                double els[6];
                for (int i = 0; i < 6; ++i)
                    els[i] = els0[i];
                els[k] += sign * s;
                const State seed = oracle_seed(
                    pert.file_, Elements{els[0], els[1], els[2], els[3], els[4], els[5]});
                double y[6];
                to_array(seed, y);
                IntegrateStats stats;
                CHECK(integrate_dp54(y, kEpoch, t, force, IntegrateOptions{}, &stats).ok());
                CHECK(pert.ok());
                for (int i = 0; i < 3; ++i)
                    (sign > 0 ? rp : rm)[i] = y[i];
            }
            const double d[3] = {(rp[0] - rm[0]) / (2 * s), (rp[1] - rm[1]) / (2 * s),
                                 (rp[2] - rm[2]) / (2 * s)};
            cov[0] += s * s * d[0] * d[0];
            cov[1] += s * s * d[0] * d[1];
            cov[2] += s * s * d[0] * d[2];
            cov[3] += s * s * d[1] * d[1];
            cov[4] += s * s * d[1] * d[2];
            cov[5] += s * s * d[2] * d[2];
        }
        // Barycentric geometric: xyz_au is the ICRF position, the line
        // of sight is its direction and length.
        double u[3] = {res.value().pos.xyz_au[0], res.value().pos.xyz_au[1],
                       res.value().pos.xyz_au[2]};
        const double n = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& x : u)
            x /= n;
        const double expect = sky_sigma_arcsec(cov, u, n);
        std::printf("  dt=%+g: engine %.6f\", oracle %.6f\"\n", dt, *res.value().sigma_arcsec,
                    expect);
        CHECK(std::fabs(*res.value().sigma_arcsec - expect) < 1e-3 * expect);
    }
}

TEST(sigma_growth_symmetry) {
    TempFile tf_kernel("sig-grw-k");
    TempFile tf_cat("sig-grw-c");
    Engine e = open_synthetic(tf_kernel);

    // A sigma_a-only record: the along-track spread grows ~linearly in
    // |t - epoch| (mean-motion offset), so the uncertainty away from the
    // epoch dwarfs the epoch value, near-symmetrically in time.
    catalog::Record r = make_record(kEls.a);
    for (double& s : r.sigmas)
        s = 0.0;
    r.sigmas[0] = 1e-6;
    CHECK(e.add_catalog(write_record(tf_cat, r)).ok());

    auto at = [&](double t) {
        auto res = e.calc(int(kSpkid), t, bary_geom_icrf());
        CHECK(res.ok());
        CHECK(res.value().sigma_arcsec.has_value());
        return *res.value().sigma_arcsec;
    };
    const double s0 = at(kEpoch), sp = at(kEpoch + 2000.0), sm = at(kEpoch - 2000.0);
    std::printf("  epoch %.4f\", +2000d %.4f\", -2000d %.4f\"\n", s0, sp, sm);
    CHECK(s0 > 0.0);
    CHECK(sp > 3.0 * s0);
    CHECK(sm > 3.0 * s0);
    CHECK(std::fabs(sp - sm) < 0.4 * 0.5 * (sp + sm));
}

TEST(sigma_newest_catalog_rescales) {
    TempFile tf_kernel("sig-nc-k");
    TempFile tf_a("sig-nc-a");
    TempFile tf_b("sig-nc-b");
    Engine e = open_synthetic(tf_kernel);

    // Same elements, ten times the sigma_M: the cached tracks are
    // invalidated by add_catalog and rebuilt, and the (linear) answer
    // scales by ten.
    catalog::Record r = make_record(kEls.a);
    for (double& s : r.sigmas)
        s = 0.0;
    r.sigmas[5] = 1e-5;
    CHECK(e.add_catalog(write_record(tf_a, r)).ok());
    const double t = kEpoch + 300.0;
    auto first = e.calc(int(kSpkid), t, bary_geom_icrf());
    CHECK(first.ok());
    CHECK(first.value().sigma_arcsec.has_value());

    r.sigmas[5] = 1e-4;
    CHECK(e.add_catalog(write_record(tf_b, r)).ok());
    auto second = e.calc(int(kSpkid), t, bary_geom_icrf());
    CHECK(second.ok());
    CHECK(second.value().sigma_arcsec.has_value());

    const double ratio = *second.value().sigma_arcsec / *first.value().sigma_arcsec;
    std::printf("  sigma ratio after 10x sigma_M: %.6f\n", ratio);
    CHECK(ratio > 9.9 && ratio < 10.1);
    // The elements did not change: same position as before.
    CHECK(second.value().pos.dist_au == first.value().pos.dist_au);
}

TEST(lookup_pdes_name_case) {
    TempFile tf_kernel("lkp-1-k");
    TempFile tf_cat("lkp-1-c");
    Engine e = open_synthetic(tf_kernel);

    // Before any catalog: nothing answers (NotFound, not a machinery
    // error).
    auto none = e.lookup("Testbody");
    CHECK(!none.ok() && none.error().code == ErrorCode::NotFound);

    CHECK(e.add_catalog(write_catalog(tf_cat, kEls.a)).ok());
    const std::string pdes = std::to_string(kSpkid);
    for (const char* q : {pdes.c_str(), "Testbody", "testbody", "TESTBODY"}) {
        auto id = e.lookup(q);
        CHECK(id.ok() && id.value() == int(kSpkid));
    }
    auto miss = e.lookup("No such name");
    CHECK(!miss.ok() && miss.error().code == ErrorCode::NotFound);

    // The name resolves to a body calc() answers identically.
    const double t = kEpoch + 200.0;
    auto by_id = e.calc(int(kSpkid), t, bary_geom_icrf());
    auto by_name = e.calc(e.lookup("testbody").value(), t, bary_geom_icrf());
    CHECK(by_id.ok() && by_name.ok());
    CHECK(by_name.value().pos.lon_deg == by_id.value().pos.lon_deg);
    CHECK(by_name.value().pos.lat_deg == by_id.value().pos.lat_deg);
    CHECK(by_name.value().pos.dist_au == by_id.value().pos.dist_au);
}

TEST(lookup_newest_catalog_wins) {
    TempFile tf_kernel("lkp-2-k");
    TempFile tf_a("lkp-2-a");
    TempFile tf_b("lkp-2-b");
    Engine e = open_synthetic(tf_kernel);

    CHECK(e.add_catalog(write_catalog(tf_a, kEls.a, kSpkid)).ok());
    CHECK(e.add_catalog(write_catalog(tf_b, kEls.a + 0.01, kSpkid + 1)).ok());
    // Both records carry the proper name "Testbody": the newest
    // catalog answers it.
    auto id = e.lookup("Testbody");
    CHECK(id.ok() && id.value() == int(kSpkid + 1));
    // The older catalog's designation still resolves.
    auto old = e.lookup(std::to_string(kSpkid));
    CHECK(old.ok() && old.value() == int(kSpkid));
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

    // The name index on real data: SBDB's designation "1" and the proper
    // name "Ceres" both answer the fixture's SPK-ID.
    auto by_name = e.value().lookup("Ceres");
    CHECK(by_name.ok() && by_name.value() == kCatalogFixtures[0].spkid);
    auto by_pdes = e.value().lookup("1");
    CHECK(by_pdes.ok() && by_pdes.value() == kCatalogFixtures[0].spkid);

    double worst_apparent = 0.0, worst_geometric = 0.0, worst_dist = 0.0, worst_sigma = 0.0;
    for (const CatalogFixture& f : kCatalogFixtures) {
        auto app = e.value().calc(f.spkid, f.jd_tt, CalcOptions::apparent());
        CHECK(app.ok());
        CalcOptions oj2000 = CalcOptions::geometric();
        oj2000.frame = Frame::J2000;
        auto geo = e.value().calc(f.spkid, f.jd_tt, oj2000);
        CHECK(geo.ok());
        if (!app.ok() || !geo.ok())
            continue;

        // The Ceres record carries SBDB sigmas: a real, small number
        // (order milliarcsec), published at every epoch.
        CHECK(app.value().sigma_arcsec.has_value());
        const double sig = *app.value().sigma_arcsec;
        CHECK(std::isfinite(sig) && sig > 0.0 && sig < 0.05);
        worst_sigma = std::max(worst_sigma, sig);

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
    std::printf("  Ceres vs swetest/DE440: apparent %.4f\", geometric J2000 %.4f\", dist %.2e AU, "
                "sigma %.4f\"\n",
                worst_apparent, worst_geometric, worst_dist, worst_sigma);
}

} // namespace

int main() {
    return ptest::run_all();
}