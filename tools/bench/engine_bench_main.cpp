// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-engine-bench: what the Engine's public calls cost, per call,
// on a real ephemeris (docs/ENGINE.md, "Performance"). Each scenario runs
// over instants spread across the file, so no cache flatters it, and reports
// the median of five runs in microseconds per call.
//
// Usage: prometheia-engine-bench DE_FILE [CATALOG.epm] [--only NAME]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/stars.hpp>

using namespace prometheia;

namespace {

struct Scenario {
    const char* name;
    int calls_per_instant;
    std::function<bool(Engine&, double)> run; // one instant; false on any error
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: prometheia-engine-bench DE_FILE [CATALOG.epm] [--only NAME]\n");
        return 2;
    }
    std::string catalog, only;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            only = argv[++i];
        else
            catalog = argv[i];
    }
    auto t_open = std::chrono::steady_clock::now();
    auto opened = Engine::open(argv[1]);
    if (!opened) {
        std::fprintf(stderr, "%s\n", opened.error().message.c_str());
        return 1;
    }
    Engine e = std::move(opened).value();
    const double open_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_open)
            .count();
    std::printf("open %.1f ms (%s)\n", open_ms, e.source().data());
    if (!catalog.empty()) {
        auto t0 = std::chrono::steady_clock::now();
        auto added = e.add_catalog(catalog);
        if (!added.ok()) {
            std::fprintf(stderr, "%s\n", added.error().message.c_str());
            return 1;
        }
        std::printf("add_catalog %.1f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());
    }

    const int planets[] = {body::kSun,     body::kMoon,    body::kMercury, 299,
                           body::kMars,    body::kJupiter, body::kSaturn,  body::kUranus,
                           body::kNeptune, body::kPluto};
    const auto chart = [&](CalcOptions o) {
        return [o, &planets](Engine& en, double t) {
            for (int b : planets)
                if (!(o.center == Center::Heliocentric && b == body::kSun) &&
                    !en.calc(b, t, o).ok())
                    return false;
            return true;
        };
    };
    CalcOptions apparent;
    CalcOptions no_speed;
    no_speed.speed = false;
    CalcOptions topo;
    topo.center = Center::Topocentric;
    topo.site = {8.55 * 3.141592653589793 / 180.0, 47.37 * 3.141592653589793 / 180.0, 500.0};
    CalcOptions lahiri;
    lahiri.sidereal = SiderealMode::Lahiri;
    CalcOptions citra;
    citra.sidereal = SiderealMode::TrueCitra;
    CalcOptions helio;
    helio.center = Center::Heliocentric;
    helio.deflection = helio.aberration = false;
    CalcOptions geometric = CalcOptions::geometric();
    std::vector<size_t> stars;
    for (const char* n : {"Aldebaran", "Regulus", "Spica", "Antares", "Sirius", "Vega", "Polaris",
                          "Betelgeuse", "Rigel", "Arcturus"})
        if (auto s = stars::find(n))
            stars.push_back(s.value());

    std::vector<Scenario> scenarios = {
        {"planets apparent", 10, chart(apparent)},
        {"planets no rates", 10, chart(no_speed)},
        {"planets geometric", 10, chart(geometric)},
        {"planets heliocentric", 10, chart(helio)},
        {"planets topocentric", 10, chart(topo)},
        {"planets lahiri", 10, chart(lahiri)},
        {"planets true-citra", 10, chart(citra)},
        {"stars apparent", int(stars.size()),
         [&](Engine& en, double t) {
             for (size_t s : stars)
                 if (!en.calc_star(s, t, apparent).ok())
                     return false;
             return true;
         }},
        {"moon mean node+apogee", 2,
         [&](Engine& en, double t) {
             return en.calc_orbit_point(body::kMoon, OrbitPoint::AscendingNode, OrbitElements::Mean,
                                        t, apparent)
                        .ok() &&
                    en.calc_orbit_point(body::kMoon, OrbitPoint::Aphelion, OrbitElements::Mean, t,
                                        apparent)
                        .ok();
         }},
        {"moon osculating node+apogee", 2,
         [&](Engine& en, double t) {
             return en.calc_orbit_point(body::kMoon, OrbitPoint::AscendingNode,
                                        OrbitElements::Osculating, t, apparent)
                        .ok() &&
                    en.calc_orbit_point(body::kMoon, OrbitPoint::Aphelion,
                                        OrbitElements::Osculating, t, apparent)
                        .ok();
         }},
        {"moon natural apogee", 1,
         [&](Engine& en, double t) {
             return en
                 .calc_orbit_point(body::kMoon, OrbitPoint::Aphelion, OrbitElements::Interpolated,
                                   t, apparent)
                 .ok();
         }},
        {"hamburg cupido..poseidon", 8,
         [&](Engine& en, double t) {
             for (const char* h : {"cupido", "hades", "zeus", "kronos", "apollon", "admetos",
                                   "vulcanus", "poseidon"})
                 if (!en.calc_hypothetical(h, t, apparent).ok())
                     return false;
             return true;
         }},
    };
    if (!catalog.empty()) {
        CalcOptions nosig;
        nosig.sigma = false;
        scenarios.push_back({"ceres eris sedna", 3, [nosig](Engine& en, double t) {
                                 for (int b : {20000001, 20136199, 20090377})
                                     if (!en.calc(b, t, nosig).ok())
                                         return false;
                                 return true;
                             }});
    }

    // Instants spread over 1900-2100 in a scrambled order, the same for
    // every scenario; small bodies get fewer (they integrate).
    std::vector<double> instants;
    for (int i = 0; i < 200; ++i)
        instants.push_back(2415020.5 + double((i * 7919) % 200) * 365.2425 + 0.37 * i);
    std::printf("%-30s %12s %12s\n", "scenario", "us/call", "calls");
    for (const Scenario& sc : scenarios) {
        if (!only.empty() && only != sc.name)
            continue;
        const size_t n = std::string(sc.name).find("ceres") == 0 ? 20 : instants.size();
        std::vector<double> runs;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < n; ++i)
                if (!sc.run(e, instants[i])) {
                    std::fprintf(stderr, "%s failed\n", sc.name);
                    return 1;
                }
            runs.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count() /
                double(n * size_t(sc.calls_per_instant)));
        }
        std::printf("%-30s %12.2f %12zu\n", sc.name, median(runs),
                    n * size_t(sc.calls_per_instant));
    }
    return 0;
}
