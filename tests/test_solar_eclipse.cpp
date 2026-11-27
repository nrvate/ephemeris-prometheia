// SPDX-License-Identifier: GPL-2.0-or-later
//
// Solar eclipses (docs/SOLAR_ECLIPSES.md). Layers:
//   - arithmetic with no ephemeris (saros numbering, the overlap of two discs);
//   - DE440 (PROMETHEIA_DE440; SKIP when absent): the 2026 eclipses against
//     swetest's recorded output and the NASA elements printed for them;
//   - NASA's tables (PROMETHEIA_SOLAR_BESSELIAN, PROMETHEIA_SOLAR_CANON, from
//     tools/fetch/solar_eclipse_fetch.py; SKIP when unset): the Besselian
//     elements of every eclipse of the canon, and the canon's catalog.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <prometheia/prometheia.h>
#include <prometheia/solar_eclipse.hpp>
#include <prometheia/time.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
namespace se = prometheia::solar_eclipse;
namespace fs = std::filesystem;

namespace {

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return env && *env ? std::string(env) : fallback;
}

const std::string kDe440Path =
    env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");

bool have(const std::string& path, const char* var) {
    if (!path.empty() && fs::exists(path))
        return true;
    std::printf("  [SKIP] '%s' not found (set %s to enable)\n", path.c_str(), var);
    return false;
}

constexpr double kDeg = 3.14159265358979323846 / 180.0;

} // namespace

TEST_CASE("saros numbering: the pin and the structure") {
    // 2017-08-21, lunation 218, is saros 145 (NASA).
    CHECK(se::saros_number(218) == 145);
    // 2024-04-08 (300) is 139; 2026-02-17 (323) is 121; 2026-08-12 (329) is 126.
    CHECK(se::saros_number(300) == 139);
    CHECK(se::saros_number(323) == 121);
    CHECK(se::saros_number(329) == 126);
    // A series repeats every 223 lunations.
    for (long k : {-49456L, -3L, 0L, 218L, 40000L})
        CHECK(se::saros_number(k) == se::saros_number(k + 223));
    CHECK(se::saros_number(-49456) == 5); // the canon's first eclipse
}

namespace {

// ---- NASA's tables ----------------------------------------------------------

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"')
            quoted = !quoted;
        else if (c == ',' && !quoted) {
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

struct NasaRow {
    int year = 0, month = 0, day = 0;
    double td_hours = 0.0; // greatest eclipse, hours TD of the day
    double dt = 0.0;
    long luna = 0;
    int saros = 0;
    std::string type;
    double gamma = 0, mag = 0, lat = 0, lon = 0, alt = 0, width = 0, dur_s = 0;
    double jd_max = 0; // TD
    double t0 = 0;     // hours TD of the day
    double x[4] = {}, y[4] = {}, d[3] = {}, mu[3] = {}, l1[3] = {}, l2[3] = {};
    double tan_f1 = 0, tan_f2 = 0;
    double jd_t0() const {
        // julian_date is the greatest eclipse (noon-based); remove its time of day.
        const double day_start = std::round(jd_max - td_hours / 24.0 - 0.5) + 0.5;
        // t0 is an hour of the date, but belongs to the nearer midnight when the
        // eclipse is in the day's last hours (t0 = 0 for a 23:52 greatest eclipse).
        double base = day_start + t0 / 24.0;
        if (jd_max - base > 0.5)
            base += 1.0;
        else if (jd_max - base < -0.5)
            base -= 1.0;
        return base;
    }
};

std::vector<NasaRow> read_besselian(const std::string& path) {
    std::vector<NasaRow> rows;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    std::map<std::string, size_t> col;
    {
        auto h = split_csv(line);
        for (size_t i = 0; i < h.size(); ++i)
            col[h[i]] = i;
    }
    while (std::getline(f, line)) {
        auto c = split_csv(line);
        if (c.size() < col.size())
            continue;
        auto num = [&](const std::string& n) { return std::atof(c[col.at(n)].c_str()); };
        NasaRow r;
        r.year = int(num("year"));
        r.month = int(num("month"));
        r.day = int(num("day"));
        int hh = 0, mm = 0, ss = 0;
        std::sscanf(c[col.at("td_ge")].c_str(), "%d:%d:%d", &hh, &mm, &ss);
        r.td_hours = hh + mm / 60.0 + ss / 3600.0;
        r.dt = num("dt");
        r.luna = long(num("luna_num"));
        r.saros = int(num("saros"));
        r.type = c[col.at("eclipse_type")];
        r.gamma = num("gamma");
        r.mag = num("magnitude");
        r.lat = num("lat_dd_ge");
        r.lon = num("lng_dd_ge");
        r.alt = num("sun_alt");
        r.width = num("path_width");
        r.dur_s = num("duration_secs");
        r.jd_max = num("julian_date");
        r.t0 = num("t0");
        for (int i = 0; i < 4; ++i) {
            r.x[i] = num("x" + std::to_string(i));
            r.y[i] = num("y" + std::to_string(i));
        }
        for (int i = 0; i < 3; ++i) {
            r.d[i] = num("d" + std::to_string(i));
            r.mu[i] = num("mu" + std::to_string(i));
            r.l1[i] = num("l1" + std::to_string(i));
            r.l2[i] = num("l2" + std::to_string(i));
        }
        r.tan_f1 = num("tan_f1");
        r.tan_f2 = num("tan_f2");
        rows.push_back(r);
    }
    return rows;
}

double poly3(const double* c, double h) {
    return c[0] + h * (c[1] + h * (c[2] + h * c[3]));
}
double poly2(const double* c, double h) {
    return c[0] + h * (c[1] + h * c[2]);
}

} // namespace

TEST_CASE("Besselian elements against NASA's, 1900-2100") {
    const std::string csv = env_or("PROMETHEIA_SOLAR_BESSELIAN", "");
    if (!have(csv, "PROMETHEIA_SOLAR_BESSELIAN") || !have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    double max_dxy = 0, max_dd = 0, max_dl1 = 0, max_dl2 = 0, max_dtan = 0;
    double sum2 = 0;
    long n = 0;
    for (const NasaRow& r : read_besselian(csv)) {
        if (r.year < 1900 || r.year > 2100)
            continue;
        for (double h : {-3.0, 0.0, 3.0}) {
            const double jd = r.jd_t0() + h / 24.0;
            auto e = se::besselian_elements(engine, jd);
            REQUIRE(e.ok());
            const double dx = e.value().x - poly3(r.x, h), dy = e.value().y - poly3(r.y, h);
            const double dxy = std::hypot(dx, dy);
            if (dxy > max_dxy) {
                max_dxy = dxy;
                std::printf("  worst so far %d-%02d-%02d h=%+.0f dx=%+.6f dy=%+.6f\n", r.year,
                            r.month, r.day, h, dx, dy);
            }
            sum2 += dxy * dxy;
            ++n;
            max_dd = std::max(max_dd, std::fabs(e.value().d_rad / kDeg - poly2(r.d, h)));
            max_dl1 = std::max(max_dl1, std::fabs(e.value().l1 - poly2(r.l1, h)));
            max_dl2 = std::max(max_dl2, std::fabs(e.value().l2 - poly2(r.l2, h)));
            max_dtan = std::max({max_dtan, std::fabs(e.value().tan_f1 - r.tan_f1),
                                 std::fabs(e.value().tan_f2 - r.tan_f2)});
        }
    }
    std::printf("  %ld samples: max |dxy| %.2e  rms %.2e  max |dd| %.2e deg  max |dl1| %.2e "
                "max |dl2| %.2e  max |dtan| %.2e\n",
                n, max_dxy, std::sqrt(sum2 / double(n)), max_dd, max_dl1, max_dl2, max_dtan);
    CHECK(max_dxy < 1e-4);
    CHECK(max_dl1 < 1e-5);
    CHECK(max_dl2 < 1e-5);
}

namespace {

struct CanonRow {
    int year = 0, month = 0, day = 0;
    double td_s = 0.0; // seconds of the day, TD
    long luna = 0;
    int saros = 0;
    std::string type;
    double gamma = 0, mag = 0, lat = 0, lon = 0;
    int alt = 0;
    double jd() const { return time::jd_from_civil(year, month, double(day)) + td_s / 86400.0; }
};

double parse_ns(const std::string& s) { // "6.0N" "33.3W"
    double v = std::atof(s.c_str());
    const char c = s.back();
    return (c == 'S' || c == 'W') ? -v : v;
}

std::vector<CanonRow> read_canon(const std::string& path) {
    static const std::map<std::string, int> months = {
        {"Jan", 1}, {"Feb", 2}, {"Mar", 3}, {"Apr", 4},  {"May", 5},  {"Jun", 6},
        {"Jul", 7}, {"Aug", 8}, {"Sep", 9}, {"Oct", 10}, {"Nov", 11}, {"Dec", 12}};
    std::vector<CanonRow> rows;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::vector<std::string> t;
        for (std::string w; is >> w;)
            t.push_back(w);
        if (t.size() < 16 || !months.count(t[3]) ||
            t[0].find_first_not_of("0123456789") != std::string::npos)
            continue;
        CanonRow r;
        r.year = std::atoi(t[2].c_str());
        r.month = months.at(t[3]);
        r.day = std::atoi(t[4].c_str());
        int hh = 0, mm = 0, ss = 0;
        std::sscanf(t[5].c_str(), "%d:%d:%d", &hh, &mm, &ss);
        r.td_s = hh * 3600.0 + mm * 60.0 + ss;
        r.luna = std::atol(t[7].c_str());
        r.saros = std::atoi(t[8].c_str());
        r.type = t[9];
        r.gamma = std::atof(t[10].c_str());
        r.mag = std::atof(t[11].c_str());
        r.lat = parse_ns(t[12]);
        r.lon = parse_ns(t[13]);
        r.alt = std::atoi(t[14].c_str());
        rows.push_back(r);
    }
    return rows;
}

// Angle between two surface points, degrees.
double sep_deg(double lat1, double lon1, double lat2, double lon2) {
    const double a = lat1 * kDeg, b = lat2 * kDeg, dl = (lon1 - lon2) * kDeg;
    const double x = std::cos(b) * std::sin(dl);
    const double y = std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(dl);
    const double z = std::sin(a) * std::sin(b) + std::cos(a) * std::cos(b) * std::cos(dl);
    return std::atan2(std::hypot(x, y), z) / kDeg;
}

} // namespace

TEST_CASE("global circumstances against NASA's Five Millennium Canon, 1900-2100") {
    const std::string cat = env_or("PROMETHEIA_SOLAR_CANON", "");
    if (!have(cat, "PROMETHEIA_SOLAR_CANON") || !have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    std::map<long, CanonRow> canon;
    for (const CanonRow& r : read_canon(cat))
        if (r.year >= 1900 && r.year <= 2099)
            canon[r.luna] = r;
    REQUIRE(canon.size() > 400);
    std::map<long, se::Global> ours;
    double start = time::jd_from_civil(1899, 12, 20.0);
    const double end = time::jd_from_civil(2100, 1, 1.0);
    while (true) {
        auto g = se::next_global(engine, start, se::kAllKinds, false);
        REQUIRE(g.ok());
        if (g.value().max_tt > end)
            break;
        ours[g.value().lunation] = g.value();
        start = g.value().max_tt + 1.0;
    }
    int missing = 0, extra = 0, kind_bad = 0, central_bad = 0, saros_bad = 0, n = 0;
    double max_dt = 0, max_dg = 0, max_dm = 0, max_pos = 0, max_pos_central = 0;
    std::vector<double> dts, dms;
    for (const auto& [k, c] : canon) {
        auto it = ours.find(k);
        if (it == ours.end()) {
            ++missing;
            std::printf("  MISSING %d-%02d-%02d %s gamma %.4f\n", c.year, c.month, c.day,
                        c.type.c_str(), c.gamma);
            continue;
        }
        const se::Global& g = it->second;
        ++n;
        const char kc = c.type[0];
        const unsigned want = kc == 'T'   ? se::kTotal
                              : kc == 'A' ? se::kAnnular
                              : kc == 'H' ? se::kHybrid
                                          : se::kPartial;
        if ((g.type & se::kAllKinds) != want) {
            ++kind_bad;
            std::printf("  KIND %d-%02d-%02d canon %s ours %u gamma %.4f/%.4f\n", c.year, c.month,
                        c.day, c.type.c_str(), g.type, c.gamma, g.gamma);
        }
        const bool noncentral = c.type.size() > 1 && (c.type[1] == '+' || c.type[1] == '-');
        if (want != se::kPartial && noncentral != bool(g.type & se::kNonCentral)) {
            ++central_bad;
            std::printf("  CENTRAL %d-%02d-%02d canon %s ours %u\n", c.year, c.month, c.day,
                        c.type.c_str(), g.type);
        }
        if (g.saros != c.saros)
            ++saros_bad;
        const double dt = std::fabs(g.max_tt - c.jd()) * 86400.0;
        max_dt = std::max(max_dt, dt);
        dts.push_back(dt);
        max_dg = std::max(max_dg, std::fabs(g.gamma - c.gamma));
        const double dm = g.magnitude - c.mag;
        max_dm = std::max(max_dm, std::fabs(dm));
        if (std::fabs(dm) > 0.001)
            std::printf("  MAG %d-%02d-%02d %s ours %.4f canon %.4f (%+.4f) obsc %.4f\n", c.year,
                        c.month, c.day, c.type.c_str(), g.magnitude, c.mag, dm, g.obscuration);
        dms.push_back(std::fabs(dm));
        const double pos = sep_deg(g.greatest.lat_deg, g.greatest.lon_deg, c.lat, c.lon);
        if (!(g.type & se::kNonCentral) && !(g.type & se::kPartial))
            max_pos_central = std::max(max_pos_central, pos);
        if (pos > 0.25)
            std::printf("  POS %d-%02d-%02d %s %.3f deg (%.1f,%.1f vs %.1f,%.1f)\n", c.year,
                        c.month, c.day, c.type.c_str(), pos, g.greatest.lat_deg, g.greatest.lon_deg,
                        c.lat, c.lon);
        max_pos = std::max(max_pos, pos);
    }
    for (const auto& [k, g] : ours)
        if (!canon.count(k))
            ++extra;
    std::sort(dts.begin(), dts.end());
    std::sort(dms.begin(), dms.end());
    std::printf("  canon %zu ours %zu matched %d missing %d extra %d kind %d central %d saros %d\n",
                canon.size(), ours.size(), n, missing, extra, kind_bad, central_bad, saros_bad);
    std::printf("  time: max %.2f s median %.2f s; gamma max %.5f; mag max %.5f median %.5f; "
                "position max %.3f deg (central eclipses %.3f)\n",
                max_dt, dts[dts.size() / 2], max_dg, max_dm, dms[dms.size() / 2], max_pos,
                max_pos_central);
    CHECK(missing == 0);
    CHECK(extra == 0);
    CHECK(kind_bad == 0);
    CHECK(central_bad == 0);
    CHECK(saros_bad == 0);
    CHECK(max_dt < 1.0);
    CHECK(max_dg < 0.001);
    CHECK(max_dm < 0.001);
    CHECK(max_pos_central < 0.14); // the canon prints 0.1 deg
    CHECK(max_pos < 0.25);
}

namespace {

double ut(int y, int mo, int d, int h, int mi, double s) {
    return time::jd_from_ymdhms(y, mo, d, h, mi, s);
}
double dsec(double jd_a, double jd_b) {
    return (jd_a - jd_b) * 86400.0;
}

struct SweGlobal {
    int y, mo, d;
    double max_s;      // UT seconds of the day
    double phase_s[4]; // partial begin, total begin, total end, partial end
    double mag_nasa, mag_cov, obsc;
    double lon, lat, dur_s;
    unsigned kind;
    int saros;
};

constexpr double hms(int h, int m, double s) {
    return h * 3600.0 + m * 60.0 + s;
}

// swetest -solecl on our DE440, UT (docs/SOLAR_ECLIPSES.md, "swetest").
const SweGlobal kSwe[] = {
    {2026,
     2,
     17,
     hms(12, 11, 53.3),
     {hms(9, 56, 47.0), hms(11, 43, 4.0), hms(12, 41, 4.2), hms(14, 27, 39.5)},
     0.9638,
     0.9798,
     0.9288,
     87 + 3 / 60.0 + 33 / 3600.0,
     -(64 + 41 / 60.0 + 2 / 3600.0),
     139.42,
     se::kAnnular,
     121},
    {2026,
     8,
     12,
     hms(17, 45, 59.2),
     {hms(15, 34, 33.1), hms(16, 58, 9.4), hms(18, 34, 0.7), hms(19, 57, 58.4)},
     1.0395,
     1.0178,
     1.0806,
     -(25 + 6 / 60.0 + 17 / 3600.0),
     65 + 9 / 60.0 + 55 / 3600.0,
     138.04,
     se::kTotal,
     126},
    {2027,
     8,
     2,
     hms(10, 6, 41.3),
     {hms(7, 30, 19.6), hms(8, 23, 31.2), hms(11, 49, 57.6), hms(12, 43, 9.7)},
     1.0800,
     1.0397,
     1.1664,
     33 + 10 / 60.0 + 5 / 3600.0,
     25 + 29 / 60.0 + 34 / 3600.0,
     382.56,
     se::kTotal,
     136},
};

} // namespace

TEST_CASE("2026-2027 eclipses against swetest, k as swetest has it and as NASA has it") {
    if (!have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    for (int variant = 0; variant < 2; ++variant) {
        se::Options o;
        if (variant == 1)
            o.k_umbra = o.k_penumbra = 0.2725076; // swetest's
        std::printf("  -- k %s\n", variant ? "0.2725076 (swetest)" : "NASA");
        for (const SweGlobal& s : kSwe) {
            const double start = time::jd_from_civil(s.y, s.mo, double(s.d)) - 3.0;
            auto r = se::next_global(engine, start, se::kAllKinds, false, o);
            REQUIRE(r.ok());
            const se::Global& g = r.value();
            const double day0 = time::jd_from_civil(s.y, s.mo, double(s.d));
            const double off = g.max_tt - g.max_ut1; // TT - UT1
            CHECK((g.type & se::kAllKinds) == s.kind);
            CHECK(g.saros == s.saros);
            CHECK(std::fabs(dsec(g.max_ut1, day0 + s.max_s / 86400.0)) < 6.0);
            if (variant == 1) {
                CHECK(std::fabs(g.magnitude - s.mag_nasa) < 6e-4);
                CHECK(std::fabs(g.magnitude_covered - s.mag_cov) < 3e-3);
            } else {
                CHECK(std::fabs(g.magnitude - s.mag_nasa) < 0.0015);
            }
            auto d = [&](double tt, int i) {
                return dsec(tt - off, day0 + s.phase_s[i] / 86400.0);
            };
            std::printf(
                "  %d-%02d-%02d max %+.2f s | p1 %+.1f c1 %+.1f c2 %+.1f u1 %+.1f p4 %+.1f s"
                " | mag %.4f/%.4f covered %.4f/%.4f obsc %.4f/%.4f | pos %.3f,%.3f vs "
                "%.3f,%.3f | dur %.2f/%.2f s | width %.1f km | gamma %.5f\n",
                s.y, s.mo, s.d, dsec(g.max_ut1, day0 + s.max_s / 86400.0), d(g.p1_tt, 0),
                d(g.c1_tt, 1), d(g.c2_tt, 2), d(g.u1_tt, 1), d(g.p4_tt, 3), g.magnitude, s.mag_nasa,
                g.magnitude_covered, s.mag_cov, g.obscuration, s.obsc, g.greatest.lon_deg,
                g.greatest.lat_deg, s.lon, s.lat, g.central_duration_s, s.dur_s, g.path_width_km,
                g.gamma);
        }
    }
}

namespace {

struct SweLocal {
    const char* name;
    double lon, lat, h;
    double max_s, c1_s, mag, obsc;
};
const SweLocal kSweLocal[] = {
    {"Berlin", 13.405, 52.52, 34.0, hms(18, 8, 25.7), hms(17, 15, 30.3), 0.8743, 0.8488},
    {"Madrid", -3.7038, 40.4168, 650.0, hms(18, 32, 24.3), hms(17, 36, 46.9), 0.9994, 0.9999},
};

} // namespace

TEST_CASE("2026-08-12 local circumstances against swetest -local") {
    if (!have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    const double day0 = time::jd_from_civil(2026, 8, 12.0);
    for (int variant = 0; variant < 2; ++variant) {
        se::Options o;
        if (variant == 1)
            o.k_umbra = o.k_penumbra = 0.2725076;
        std::printf("  -- k %s\n", variant ? "0.2725076 (swetest)" : "NASA");
        for (const SweLocal& s : kSweLocal) {
            const frames::GeoSite site{s.lon * kDeg, s.lat * kDeg, s.h};
            auto r = se::local_next(engine, time::jd_from_civil(2026, 7, 1.0), site, false, o);
            REQUIRE(r.ok());
            const se::Local& l = r.value();
            std::printf("  %s: type %u max %+.2f s alt %.2f c1 %+.2f s (c4 %s, alt %.2f) mag "
                        "%.4f/%.4f obsc %.4f/%.4f\n",
                        s.name, l.type, dsec(l.max_ut1, day0 + s.max_s / 86400.0), l.max_alt_deg,
                        dsec(l.contact_ut1[0], day0 + s.c1_s / 86400.0),
                        std::isnan(l.contact_tt[3]) ? "missing" : "present", l.contact_alt_deg[3],
                        l.magnitude, s.mag, l.obscuration, s.obsc);
            CHECK(std::fabs(dsec(l.max_ut1, day0 + s.max_s / 86400.0)) < 2.0);
            CHECK(std::fabs(dsec(l.contact_ut1[0], day0 + s.c1_s / 86400.0)) < 2.0);
            CHECK(std::isnan(l.contact_tt[3])); // the Sun sets during the eclipse
            if (variant == 1) {
                CHECK(std::fabs(l.magnitude - s.mag) < 2e-4);
                CHECK(std::fabs(l.obscuration - s.obsc) < 2e-4);
            }
        }
    }
}

TEST_CASE("the 2026-08-12 total eclipse: where, the limits, a site on the path") {
    if (!have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    auto g = se::next_global(engine, time::jd_from_civil(2026, 8, 1.0), se::kTotal, false);
    REQUIRE(g.ok());
    CHECK((g.value().type & se::kTotal) != 0);
    CHECK((g.value().type & se::kCentral) != 0);
    CHECK(g.value().magnitude > 1.03);
    CHECK(g.value().path_width_km > 280.0);
    CHECK(g.value().path_width_km < 320.0);
    CHECK(g.value().c1_tt < g.value().max_tt);
    CHECK(g.value().max_tt < g.value().c2_tt);
    CHECK(g.value().p1_tt < g.value().u1_tt);
    CHECK(g.value().u1_tt <= g.value().c1_tt);

    auto w = se::where(engine, g.value().max_tt);
    REQUIRE(w.ok());
    REQUIRE(w.value().axis_hits);
    CHECK(w.value().type == se::kTotal);
    CHECK(std::fabs(w.value().central.lat_deg - g.value().greatest.lat_deg) < 1e-6);
    CHECK(w.value().north_limit.lat_deg > w.value().central.lat_deg);
    CHECK(w.value().south_limit.lat_deg < w.value().central.lat_deg);
    CHECK(std::fabs(w.value().path_width_km - g.value().path_width_km) < 1e-6);
    // Iceland sits on the path: Reykjavik sees totality of about a minute.
    const frames::GeoSite reykjavik{-21.94 * kDeg, 64.15 * kDeg, 10.0};
    auto l = se::local_at(engine, g.value(), reykjavik);
    REQUIRE(l.ok());
    CHECK(l.value().type == se::kTotal);
    CHECK(l.value().magnitude > 1.0);
    CHECK(l.value().obscuration == doctest::Approx(1.0));
    CHECK(l.value().duration_s > 30.0);
    CHECK(!std::isnan(l.value().contact_tt[1]));
    CHECK(!std::isnan(l.value().contact_tt[2]));
    CHECK(l.value().contact_tt[0] < l.value().contact_tt[1]);
    // The antipode sees nothing.
    const frames::GeoSite antipode{(180.0 - 21.94) * kDeg, -64.15 * kDeg, 0.0};
    auto none = se::local_at(engine, g.value(), antipode);
    CHECK(!none.ok());
    // Away from the eclipse the axis misses the Earth.
    auto miss = se::where(engine, g.value().max_tt + 0.5);
    REQUIRE(miss.ok());
    CHECK(!miss.value().axis_hits);
}

TEST_CASE("searching backward, and by kind") {
    if (!have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    auto eng = Engine::open(kDe440Path);
    REQUIRE(eng.ok());
    Engine engine = std::move(eng).value();
    const double j2026 = time::jd_from_civil(2026, 8, 13.0);
    auto back = se::next_global(engine, j2026, se::kAllKinds, true);
    REQUIRE(back.ok());
    CHECK(back.value().lunation == 329); // 2026-08-12
    auto before = se::next_global(engine, back.value().max_tt, se::kAllKinds, true);
    REQUIRE(before.ok());
    CHECK(before.value().lunation == 323); // 2026-02-17
    // The next hybrid eclipse after 2026 is 2031-11-14.
    auto hybrid = se::next_global(engine, j2026, se::kHybrid, false);
    REQUIRE(hybrid.ok());
    CHECK(hybrid.value().type == (se::kHybrid | se::kCentral));
    CHECK(hybrid.value().max_tt > time::jd_from_civil(2031, 11, 14.0));
    CHECK(hybrid.value().max_tt < time::jd_from_civil(2031, 11, 15.0));
}

TEST_CASE("the C interface answers as the C++ one does") {
    if (!have(kDe440Path, "PROMETHEIA_DE440"))
        return;
    prometheia_engine* e = nullptr;
    prometheia_error err;
    REQUIRE(prometheia_engine_open(kDe440Path.c_str(), &e, &err) == PROMETHEIA_OK);
    const double start = time::jd_from_civil(2026, 8, 1.0);
    prometheia_solar_eclipse g;
    REQUIRE(prometheia_solar_eclipse_next(e, start, PROMETHEIA_SOLAR_TOTAL, 0, &g, &err) ==
            PROMETHEIA_OK);
    CHECK(g.type == (PROMETHEIA_SOLAR_TOTAL | PROMETHEIA_SOLAR_CENTRAL));
    CHECK(g.saros == 126);
    CHECK(g.lunation == 329);
    CHECK(g.max_tt - g.max_ut1 > 60.0 / 86400.0);
    CHECK(g.elements.l2 < 0.0);
    prometheia_solar_where w;
    REQUIRE(prometheia_solar_eclipse_where(e, g.max_tt, &w, &err) == PROMETHEIA_OK);
    CHECK(w.axis_hits == 1);
    CHECK(w.type == PROMETHEIA_SOLAR_TOTAL);
    CHECK(w.lat_deg == doctest::Approx(g.lat_deg));
    REQUIRE(prometheia_solar_eclipse_where(e, g.max_tt + 0.5, &w, &err) == PROMETHEIA_OK);
    CHECK(w.axis_hits == 0);
    CHECK(std::isnan(w.lat_deg));
    const prometheia_site berlin{13.405, 52.52, 34.0};
    prometheia_solar_local l;
    REQUIRE(prometheia_solar_eclipse_local_next(e, start - 40, &berlin, 0, &l, &err) ==
            PROMETHEIA_OK);
    CHECK(l.type == PROMETHEIA_SOLAR_PARTIAL);
    CHECK(l.saros == 126);
    CHECK(l.visible_max == 1);
    CHECK(std::isnan(l.contact_tt[3]));
    CHECK(std::isnan(l.contact_tt[1]));
    CHECK(l.magnitude > 0.87);
    CHECK(l.magnitude < 0.88);
    prometheia_solar_eclipse_options o;
    prometheia_solar_eclipse_options_init(&o);
    CHECK(o.k_umbra == 0.272281);
    o.k_penumbra = 2.0;
    CHECK(prometheia_solar_eclipse_next_ex(e, start, 0, 0, &o, &g, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_solar_eclipse_next(e, start, 99, 0, &g, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_solar_eclipse_next(nullptr, start, 0, 0, &g, &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    prometheia_engine_close(e);
}
