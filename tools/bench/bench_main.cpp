// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-bench — the performance story, measured not estimated.
// Scenarios run against the committed 100-body real-data fixture:
//   A. cold: 79 bodies integrated from catalog elements over ±1 year
//      (the "positions of 79 objects at a timestamp" first-query case),
//   B. warm: repeated memoized evaluations (steady-state per-query cost),
//   C. arc: all 100 bodies over a 10-year forward arc.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <prometheia/catalog.hpp>
#include <prometheia/forces.hpp>
#include <prometheia/integrator.hpp>
#include <prometheia/kepler.hpp>
#include <prometheia/memo.hpp>
#include <prometheia/trajectory.hpp>

namespace {

using namespace prometheia;

constexpr double kMuSun = 2.959122082855911e-4; // AU^3/day^2
constexpr double kMuJup = 2.8246746e-8;
constexpr double kMuSat = 8.458e-8;

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

State kepler_state(const Elements& el, double t, double t0) {
    State s0 = elements_to_state(kMuSun, el).value();
    return kepler_propagate(kMuSun, s0, t0, t).value();
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "tests/data/sample-100.epm";
    auto cat = catalog::Reader::open(path);
    if (!cat.ok()) {
        std::fprintf(stderr, "open %s: %s\n", path.c_str(), cat.error().message.c_str());
        return 1;
    }
    catalog::Reader& reader = cat.value();

    // Collect elliptic records (the fixture is all asteroids).
    std::vector<Elements> els;
    std::vector<uint64_t> spkids;
    auto fe = reader.for_each([&](const catalog::Record& r, const catalog::Names&) {
        if (r.e < 1.0) {
            Elements el;
            el.a = r.a_au;
            el.e = r.e;
            el.inc = r.inc_rad;
            el.node = r.node_rad;
            el.argp = r.argp_rad;
            el.mean_anom = r.mean_anom_rad;
            els.push_back(el);
            spkids.push_back(r.spkid);
        }
    });
    if (!fe.ok()) {
        std::fprintf(stderr, "scan: %s\n", fe.error().message.c_str());
        return 1;
    }
    const size_t n_bodies = els.size();
    std::printf("catalog: %s — %zu elliptic bodies\n", path.c_str(), n_bodies);
    if (n_bodies == 0)
        return 1;

    // Perturbers: Jupiter and Saturn on Kepler-sampled tables spanning the
    // bench epochs (epoch ± 1 yr, plus the 10-year arc).
    const double epoch = 2461200.5;
    const double t0 = -400.0, t1 = 400.0 + 3652.5; // relative to epoch
    Elements jup{5.2044, 0.0489, 0.0228, 1.753, 4.787, 0.6};
    Elements sat{9.5826, 0.0565, 0.0432, 1.617, 5.716, 5.2};
    static Trajectory jup_traj =
        Trajectory::sample_uniform([&](double t) { return kepler_state(jup, t + epoch, epoch); },
                                   t0, t1, 2048)
            .value();
    static Trajectory sat_traj =
        Trajectory::sample_uniform([&](double t) { return kepler_state(sat, t + epoch, epoch); },
                                   t0, t1, 2048)
            .value();
    static std::vector<Perturber> perturbers = {Perturber{kMuJup, &jup_traj},
                                                Perturber{kMuSat, &sat_traj}};
    static HeliocentricForce force{kMuSun, &perturbers};

    // ---- Scenario A: cold, 79 bodies, ±1 year windows --------------------
    const size_t nA = std::min<size_t>(79, n_bodies);
    {
        const auto begin = std::chrono::steady_clock::now();
        IntegrateStats total{};
        for (size_t i = 0; i < nA; ++i) {
            State s0 = elements_to_state(kMuSun, els[i]).value();
            double y[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
            IntegrateStats st;
            auto e = integrate_dp54(y, -365.25, 365.25, force, IntegrateOptions{}, &st);
            if (!e.ok()) {
                std::fprintf(stderr, "integrate failed: %s\n", e.error().message.c_str());
                return 1;
            }
            total.accel_evals += st.accel_evals;
            total.steps += st.steps;
        }
        const double ms = ms_since(begin);
        std::printf("A. cold 79 bodies, ±1 yr each: %.2f ms total, %.3f ms/body "
                    "(%llu steps, %llu evals)\n",
                    ms, ms / double(nA), (unsigned long long)total.steps,
                    (unsigned long long)total.accel_evals);
    }

    // ---- Scenario B: warm memoized evaluations ----------------------------
    {
        WindowMemo::Config cfg;
        cfg.window_days = 365.25;
        WindowMemo memo(&force, IntegrateOptions{}, cfg);
        memo.set_seed(elements_to_state(kMuSun, els[0]).value(), 0.0);
        // Prime one window.
        for (int i = 0; i <= 16; ++i)
            memo.at(365.25 * double(i) / 16.0);
        const auto begin = std::chrono::steady_clock::now();
        const int kEvals = 1000000;
        double sink = 0.0;
        for (int i = 0; i < kEvals; ++i) {
            const double t = (double(i % 977) / 977.0) * 365.25;
            sink += memo.at(t).pos.x;
        }
        const double ms = ms_since(begin);
        std::printf("B. warm: %d memoized evals in %.2f ms (%.0f ns/eval, "
                    "checksum %.3e)\n",
                    kEvals, ms, ms * 1e6 / double(kEvals), sink);
    }

    // ---- Scenario C: 10-year arc for all bodies ---------------------------
    {
        const auto begin = std::chrono::steady_clock::now();
        IntegrateStats total{};
        for (size_t i = 0; i < n_bodies; ++i) {
            State s0 = elements_to_state(kMuSun, els[i]).value();
            double y[6] = {s0.pos.x, s0.pos.y, s0.pos.z, s0.vel.x, s0.vel.y, s0.vel.z};
            IntegrateStats st;
            auto e = integrate_dp54(y, 0.0, 3652.5, force, IntegrateOptions{}, &st);
            if (!e.ok()) {
                std::fprintf(stderr, "integrate failed: %s\n", e.error().message.c_str());
                return 1;
            }
            total.accel_evals += st.accel_evals;
            total.steps += st.steps;
        }
        const double ms = ms_since(begin);
        std::printf("C. arc: all %zu bodies, 10 yr forward: %.2f ms total "
                    "(%llu steps, %llu evals)\n",
                    n_bodies, ms, (unsigned long long)total.steps,
                    (unsigned long long)total.accel_evals);
    }
    return 0;
}
