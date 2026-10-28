// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lunar eclipses (docs/ECLIPSES.md). Held to swetest's printed 2026-03-03
// and 2027-02-20 eclipses, to the saros arithmetic, and (env-gated:
// PROMETHEIA_LUNAR_CANON, the file tools/fetch/eclipse_fetch.py fetches) to
// every eclipse of NASA's Five Millennium Canon of Lunar Eclipses between
// 1900 and 2100.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/lunar_eclipse.hpp>
#include <prometheia/prometheia.h>
#include <prometheia/time.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
namespace fs = std::filesystem;
namespace le = prometheia::lunar_eclipse;

namespace {

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return env && *env ? std::string(env) : fallback;
}

const std::string kDe440Path =
    env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");

bool have_de440() {
    if (fs::exists(kDe440Path))
        return true;
    std::printf("  [SKIP] '%s' not found (set PROMETHEIA_DE440 to enable)\n", kDe440Path.c_str());
    return false;
}

double tt_of(int y, int mo, int d, int h, int mi, double s) {
    return time::jd_from_ymdhms(y, mo, d, h, mi, s);
}

// A clock time in a day as seconds since 00:00 of the date of `jd`.
double seconds_of_day(double jd) {
    const double f = jd + 0.5 - std::floor(jd + 0.5);
    return f * 86400.0;
}

} // namespace

TEST_CASE("saros_of: the pinned reference eclipse and the member count") {
    // NASA's canon, catalog number 09708: 2026 Mar 03 11:34:52 TD, total,
    // saros 133 (van den Bergh numbering). Swetest agrees: 133/27.
    const le::Saros a = le::saros_of(tt_of(2026, 3, 3, 11, 34, 52.0));
    CHECK(a.series == 133);
    CHECK(a.member == 27);
    // One saros (223 lunations, 6585.32 days) on: the same series, next member.
    const le::Saros b = le::saros_of(tt_of(2026, 3, 3, 11, 34, 52.0) + 6585.3211);
    CHECK(b.series == 133);
    CHECK(b.member == 28);
    // 2027 Feb 20, penumbral, 143/18 in swetest's output.
    const le::Saros c = le::saros_of(tt_of(2027, 2, 20, 23, 14, 0.0));
    CHECK(c.series == 143);
    CHECK(c.member == 18);
}

TEST_CASE("next: arguments") {
    if (!have_de440())
        return;
    auto e = Engine::open(kDe440Path);
    REQUIRE(e.ok());
    CHECK(le::next(e.value(), 2461000.0, 0).error().code == ErrorCode::ArgumentError);
    CHECK(le::next(e.value(), 2461000.0, 8).error().code == ErrorCode::ArgumentError);
    CHECK(le::next(e.value(), std::nan(""), le::kAllKinds).error().code ==
          ErrorCode::ArgumentError);
}

TEST_CASE("2026-03-03 total and 2027-02-20 penumbral against swetest") {
    if (!have_de440())
        return;
    auto e = Engine::open(kDe440Path);
    REQUIRE(e.ok());
    auto r = le::next(e.value(), tt_of(2026, 1, 1, 0, 0, 0.0), le::kTotal);
    REQUIRE(r.ok());
    const le::Eclipse& t = r.value();
    CHECK(t.kind == le::Kind::Total);
    CHECK(t.saros_series == 133);
    CHECK(t.saros_member == 27);
    // swetest: 11:33:41.2 UT, magnitudes 1.1507 / 2.1839, contacts (UT)
    // 08:44:24.0 09:50:05.1 11:04:32.3 12:02:51.5 13:17:17.7 14:23:07.5.
    CHECK(std::fabs(t.umbral_magnitude - 1.1507) < 0.0005);
    CHECK(std::fabs(t.penumbral_magnitude - 2.1839) < 0.0005);
    const double want[6] = {8 * 3600 + 44 * 60 + 24.0,  9 * 3600 + 50 * 60 + 5.1,
                            11 * 3600 + 4 * 60 + 32.3,  12 * 3600 + 2 * 60 + 51.5,
                            13 * 3600 + 17 * 60 + 17.7, 14 * 3600 + 23 * 60 + 7.5};
    for (int c = 0; c < 6; ++c)
        CHECK(std::fabs(seconds_of_day(t.contact_ut1[c]) - want[c]) < 5.0);
    CHECK(std::fabs(seconds_of_day(t.jd_max_ut1) - (11 * 3600 + 33 * 60 + 41.2)) < 5.0);
    CHECK(t.gamma < 0.0);
    CHECK(std::fabs(t.gamma - -0.3765) < 0.002);

    auto p = le::next(e.value(), tt_of(2027, 1, 1, 0, 0, 0.0), le::kPenumbral);
    REQUIRE(p.ok());
    CHECK(p.value().kind == le::Kind::Penumbral);
    CHECK(p.value().saros_series == 143);
    CHECK(p.value().saros_member == 18);
    CHECK(std::fabs(p.value().penumbral_magnitude - 0.9266) < 0.0005);
    CHECK(p.value().umbral_magnitude < 0.0);
    CHECK(std::isnan(p.value().contact_tt[le::U1]));
    CHECK(std::isnan(p.value().contact_tt[le::U2]));
    CHECK(std::isfinite(p.value().contact_tt[le::P1]));
    CHECK(std::isfinite(p.value().contact_tt[le::P4]));
    CHECK(std::fabs(seconds_of_day(p.value().contact_ut1[le::P1]) - (21 * 3600 + 12 * 60 + 22.7)) <
          5.0);

    // Backward from the penumbral one finds the total, then the one before.
    auto b = le::next(e.value(), p.value().jd_max_tt, le::kAllKinds, true);
    REQUIRE(b.ok());
    CHECK(b.value().jd_max_tt < p.value().jd_max_tt);
    CHECK(b.value().jd_max_tt > t.jd_max_tt);
    auto b2 = le::next(e.value(), t.jd_max_tt - 0.001, le::kTotal, true);
    REQUIRE(b2.ok());
    CHECK(b2.value().jd_max_tt < t.jd_max_tt - 100.0);
    CHECK(b2.value().kind == le::Kind::Total);
}

TEST_CASE("local: the Moon's altitude at the contacts") {
    if (!have_de440())
        return;
    auto e = Engine::open(kDe440Path);
    REQUIRE(e.ok());
    // Honolulu: the 2026-03-03 totality was at night there, the Moon high.
    const frames::GeoSite site{-157.86 * 3.14159265358979323846 / 180.0,
                               21.31 * 3.14159265358979323846 / 180.0, 0.0};
    auto l = le::local(e.value(), tt_of(2026, 3, 3, 11, 34, 52.0), site);
    REQUIRE(l.ok());
    for (int c = 0; c <= le::kMaximum; ++c) {
        CHECK(std::isfinite(l.value().altitude_deg[c]));
        CHECK(std::fabs(l.value().apparent_altitude_deg[c] - l.value().altitude_deg[c]) < 1.0);
    }
    CHECK(l.value().altitude_deg[le::kMaximum] > 30.0);
    // A site on the other side of the world: the Moon is below the horizon.
    const frames::GeoSite far{22.0 * 3.14159265358979323846 / 180.0,
                              -20.0 * 3.14159265358979323846 / 180.0, 0.0};
    auto f = le::local(e.value(), tt_of(2026, 3, 3, 11, 34, 52.0), far);
    REQUIRE(f.ok());
    CHECK(f.value().altitude_deg[le::kMaximum] < 0.0);
    CHECK(le::local(e.value(), tt_of(2026, 4, 3, 0, 0, 0.0), site).error().code ==
          ErrorCode::NotFound);
}

namespace {

struct CanonRow {
    int catalog = 0;
    double jd_td = 0.0;
    int saros = 0;
    char klass = 'N'; // N, P, T
    double gamma = 0.0, pen = 0.0, um = 0.0;
};

std::vector<CanonRow> read_canon(const std::string& path, double jd_first, double jd_last) {
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const std::regex row(
        R"(^(\d{5})\s+(-?\d+) (\w{3}) (\d+)\s+(\d\d):(\d\d):(\d\d)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\w+)[+-]?\s+\S+\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+))");
    std::vector<CanonRow> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, row))
            continue;
        int mo = 0;
        for (int i = 0; i < 12; ++i)
            if (m[3].str() == months[i])
                mo = i + 1;
        const int y = std::stoi(m[2]);
        if (y < 1800 || y > 2200)
            continue;
        CanonRow r;
        r.catalog = std::stoi(m[1]);
        r.jd_td = time::jd_from_ymdhms(y, mo, std::stoi(m[4]), std::stoi(m[5]), std::stoi(m[6]),
                                       std::stod(m[7]));
        if (r.jd_td < jd_first || r.jd_td > jd_last)
            continue;
        r.saros = std::stoi(m[10]);
        r.klass = m[11].str()[0];
        r.gamma = std::stod(m[12]);
        r.pen = std::stod(m[13]);
        r.um = std::stod(m[14]);
        out.push_back(r);
    }
    return out;
}

} // namespace

TEST_CASE("the NASA canon, 1900-2100") {
    const char* canon = std::getenv("PROMETHEIA_LUNAR_CANON");
    if (!canon || !*canon || !fs::exists(canon) || !have_de440()) {
        std::printf("  [SKIP] PROMETHEIA_LUNAR_CANON (tools/fetch/eclipse_fetch.py) not set\n");
        return;
    }
    auto e = Engine::open(kDe440Path);
    REQUIRE(e.ok());
    const double first = tt_of(1900, 1, 1, 0, 0, 0.0), last = tt_of(2100, 12, 31, 23, 59, 59.0);
    const std::vector<CanonRow> rows = read_canon(canon, first, last);
    REQUIRE(rows.size() > 270);
    double cursor = first - 1.0;
    int missing = 0, extra = 0, wrong_class = 0, wrong_saros = 0, wrong_gamma = 0;
    double worst_s = 0.0, worst_pen = 0.0, worst_um = 0.0, worst_gamma = 0.0;
    for (const CanonRow& r : rows) {
        auto n = le::next(e.value(), cursor, le::kAllKinds);
        REQUIRE(n.ok());
        const le::Eclipse& x = n.value();
        cursor = x.jd_max_tt;
        const double dt_s = (x.jd_max_tt - r.jd_td) * 86400.0;
        if (std::fabs(dt_s) > 3600.0) {
            // Not the same eclipse: ours is extra or the canon's is missing.
            if (x.jd_max_tt < r.jd_td) {
                ++extra;
                cursor = x.jd_max_tt;
            } else {
                ++missing;
                cursor = r.jd_td;
            }
            std::printf("  canon %05d: ours at %.5f, canon %.5f\n", r.catalog, x.jd_max_tt,
                        r.jd_td);
            continue;
        }
        worst_s = std::max(worst_s, std::fabs(dt_s));
        worst_pen = std::max(worst_pen, std::fabs(x.penumbral_magnitude - r.pen));
        worst_um = std::max(worst_um, std::fabs(x.umbral_magnitude - r.um));
        worst_gamma = std::max(worst_gamma, std::fabs(x.gamma - r.gamma));
        const char k = x.kind == le::Kind::Total ? 'T' : x.kind == le::Kind::Partial ? 'P' : 'N';
        if (k != r.klass) {
            ++wrong_class;
            std::printf("  canon %05d class %c, ours %c (um %.4f vs %.4f)\n", r.catalog, r.klass, k,
                        r.um, x.umbral_magnitude);
        }
        if (x.saros_series != r.saros)
            ++wrong_saros;
        if (std::fabs(x.gamma - r.gamma) > 0.002)
            ++wrong_gamma;
    }
    // The first one past the canon's last row must be after the range.
    std::printf("  canon 1900-2100: %zu eclipses; missing %d extra %d class %d saros %d;\n"
                "  worst greatest-eclipse time %.2f s, penumbral mag %.4f, umbral mag %.4f, "
                "gamma %.4f\n",
                rows.size(), missing, extra, wrong_class, wrong_saros, worst_s, worst_pen, worst_um,
                worst_gamma);
    CHECK(missing == 0);
    CHECK(extra == 0);
    CHECK(wrong_class == 0);
    CHECK(wrong_saros == 0);
    CHECK(wrong_gamma == 0);
    CHECK(worst_s < 5.0);
    CHECK(worst_pen < 0.002);
    CHECK(worst_um < 0.002);
}

TEST_CASE("the C API agrees with the C++ one and refuses bad arguments") {
    if (!have_de440())
        return;
    prometheia_engine* eng = nullptr;
    prometheia_error err{};
    REQUIRE(prometheia_engine_open(kDe440Path.c_str(), &eng, &err) == PROMETHEIA_OK);
    prometheia_lunar_eclipse x;
    CHECK(prometheia_lunar_eclipse_next(eng, 2461042.5, PROMETHEIA_LUNAR_ECLIPSE_TOTAL, 0, &x,
                                        &err) == PROMETHEIA_OK);
    CHECK(x.kind == PROMETHEIA_LUNAR_ECLIPSE_TOTAL);
    CHECK(x.saros_series == 133);
    CHECK(x.saros_member == 27);
    CHECK(std::isfinite(x.contact_tt[PROMETHEIA_LUNAR_CONTACT_U2]));
    CHECK(prometheia_lunar_eclipse_next(eng, 2461042.5, 0, 0, &x, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(x.kind == 0);
    CHECK(prometheia_lunar_eclipse_next(nullptr, 2461042.5, 7, 0, &x, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_lunar_eclipse_next(eng, 2461042.5, 7, 0, nullptr, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    prometheia_lunar_eclipse_local l;
    const prometheia_site site{-157.86, 21.31, 0.0};
    CHECK(prometheia_lunar_eclipse_at(eng, 2461102.5, &site, &l, &err) == PROMETHEIA_OK);
    CHECK(l.eclipse.saros_series == 133);
    CHECK(l.altitude_deg[PROMETHEIA_LUNAR_CONTACT_MAXIMUM] > 30.0);
    CHECK(prometheia_lunar_eclipse_at(eng, 2461102.5, nullptr, &l, &err) == PROMETHEIA_OK);
    CHECK(std::isnan(l.altitude_deg[0]));
    CHECK(prometheia_lunar_eclipse_at(eng, 2461042.5, &site, &l, &err) ==
          PROMETHEIA_ERROR_NOT_FOUND);
    prometheia_engine_close(eng);
}
