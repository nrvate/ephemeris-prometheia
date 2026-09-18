// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ephem CLI (tools/ephem/ephem.c), run as a subprocess on the
// synthetic kernel and the in-tree sample catalog. CSV output prints
// every double with 17 significant digits, so the values it reports
// must round-trip to exactly what prometheia::Engine answers for the
// same request: this checks the argument parsing, the time handling and
// the option mapping end to end. Table and JSON output, exit statuses
// and error messages are checked structurally.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/stars.hpp>
#include <prometheia/time.hpp>

#include "synthetic_spk.hpp"
#include <doctest/doctest.h>

using namespace prometheia;
using synth::TempFile;

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

struct Run {
    int status = -1;
    std::string out, err;
};

std::string slurp(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

// Runs ephem with the given argument string (already shell-quoted where
// needed) and an environment prefix.
Run ephem(const std::string& args, const std::string& env = "") {
    TempFile err_file("ephem-stderr");
    const std::string cmd =
        "env -u PROMETHEIA_EPHEMERIS -u PROMETHEIA_CATALOGS -u PROMETHEIA_PERTURBERS "
        "-u PROMETHEIA_HYPOTHETICALS " +
        env + " " + EPHEM_BIN + " " + args + " 2>" + err_file.path.string();
    Run r;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0)
            r.out.append(buf, n);
        const int st = pclose(p);
        r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    r.err = slurp(err_file.path.string());
    return r;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep))
        v.push_back(cur);
    if (!s.empty() && s.back() == sep)
        v.push_back("");
    return v;
}

struct CsvRow {
    std::string utc, body, source;
    double jd_tt = 0;
    int id = 0;
    double v[13] = {}; // lon lat dist, speeds, xyz, vel, light time
    bool has_sigma = false, has_ayanamsa = false;
    double sigma = 0, ayanamsa = 0;
};

// Parses ephem's CSV (no quoted fields are produced for these bodies).
std::vector<CsvRow> parse_csv(const std::string& out) {
    std::vector<CsvRow> rows;
    auto lines = split(out, '\n');
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty())
            continue;
        auto f = split(lines[i], ',');
        if (f.size() != 20)
            continue;
        CsvRow r;
        r.utc = f[0];
        r.jd_tt = std::strtod(f[1].c_str(), nullptr);
        r.body = f[2];
        r.id = std::atoi(f[3].c_str());
        for (int k = 0; k < 13; ++k)
            r.v[k] = std::strtod(f[4 + k].c_str(), nullptr);
        r.has_sigma = !f[17].empty();
        r.sigma = std::strtod(f[17].c_str(), nullptr);
        r.has_ayanamsa = !f[18].empty();
        r.ayanamsa = std::strtod(f[18].c_str(), nullptr);
        r.source = f[19];
        rows.push_back(r);
    }
    return rows;
}

bool same(const CsvRow& row, const CalcResult& r) {
    const double want[13] = {r.pos.lon_deg,
                             r.pos.lat_deg,
                             r.pos.dist_au,
                             r.pos.lon_speed,
                             r.pos.lat_speed,
                             r.pos.dist_speed,
                             r.pos.xyz_au[0],
                             r.pos.xyz_au[1],
                             r.pos.xyz_au[2],
                             r.pos.vel_au_day[0],
                             r.pos.vel_au_day[1],
                             r.pos.vel_au_day[2],
                             r.provenance.light_time_days};
    bool eq = row.source == r.provenance.source && row.has_sigma == r.sigma_arcsec.has_value() &&
              row.has_ayanamsa == r.ayanamsa_deg.has_value();
    for (int k = 0; k < 13; ++k)
        eq = eq && row.v[k] == want[k];
    if (r.sigma_arcsec)
        eq = eq && row.sigma == *r.sigma_arcsec;
    if (r.ayanamsa_deg)
        eq = eq && row.ayanamsa == *r.ayanamsa_deg;
    return eq;
}

struct Kernel {
    TempFile file{"ephem-kernel"};
    Engine engine;
    std::string arg;
    Kernel() {
        synth::write_linear_spk(file.path);
        engine = Engine::open(file.path.string()).value();
        arg = "-e " + file.path.string();
    }
};

TEST_CASE("ephem_csv_matches_engine") {
    Kernel k;
    const double jd = 2461300.25;
    const Run r = ephem(k.arg + " -j 2461300.25 -f csv sun jupiter 5 10");
    CHECK(r.status == 0);
    CHECK(r.err.empty());
    auto rows = parse_csv(r.out);
    CHECK(rows.size() == 4);
    if (rows.size() != 4)
        return;
    CHECK((rows[0].body == "Sun" && rows[0].id == 10 && rows[1].body == "Jupiter"));
    CHECK((rows[2].body == "Jupiter" && rows[3].body == "Sun")); // IDs take the known label
    const auto sun = k.engine.calc(10, jd);
    const auto jup = k.engine.calc(5, jd);
    CHECK(sun.ok());
    CHECK(jup.ok());
    CHECK((same(rows[0], sun.value()) && same(rows[3], sun.value())));
    CHECK((same(rows[1], jup.value()) && same(rows[2], jup.value())));
    CHECK(rows[0].jd_tt == jd);
    CHECK(rows[0].utc == "2026-09-16 17:58:50.816"); // 18:00 TT - 69.184 s
}

TEST_CASE("ephem_utc_input_and_series") {
    Kernel k;
    const double base = time::utc_to_tt(2017, 1, 1, 0, 0, 0.0).value();
    Run r = ephem(k.arg + " -t 2016-12-31T23:59:60.5Z -f csv sun");
    auto rows = parse_csv(r.out);
    CHECK(r.status == 0);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        const double jd = time::utc_to_tt(2016, 12, 31, 23, 59, 60.5).value();
        CHECK(rows[0].jd_tt == jd);
        CHECK(rows[0].utc == "2016-12-31 23:59:60.500"); // the leap second is labelled
        CHECK(same(rows[0], k.engine.calc(10, jd).value()));
    }

    // Four rows six hours apart, uniform in TT.
    r = ephem(k.arg + " -t '2017-01-01 00:00' -n 4 --step 6h -f csv jupiter");
    rows = parse_csv(r.out);
    CHECK(r.status == 0);
    CHECK(rows.size() == 4);
    for (size_t i = 0; i < rows.size(); ++i) {
        const double jd = base + double(i) * 0.25;
        CHECK(rows[i].jd_tt == jd);
        CHECK(same(rows[i], k.engine.calc(5, jd).value()));
    }
    if (rows.size() == 4)
        CHECK(rows[3].utc == "2017-01-01 18:00:00.000");

    // Steps in other units, and backwards.
    r = ephem(k.arg + " -j 2461300 -n 3 -s -90m -f csv sun");
    rows = parse_csv(r.out);
    CHECK((rows.size() == 3 && rows[2].jd_tt == 2461300.0 - 2.0 * (1.0 / 1440.0 * 90.0)));
}

TEST_CASE("ephem_options_map_to_engine") {
    Kernel k;
    const double jd = 2455000.5;
    struct Case {
        const char* args;
        CalcOptions opts;
    };
    CalcOptions geo_j2000_eq = CalcOptions::geometric();
    geo_j2000_eq.frame = Frame::J2000;
    geo_j2000_eq.coords = Coords::Equatorial;
    CalcOptions astro_mean = CalcOptions::astrometric();
    astro_mean.frame = Frame::MeanOfDate;
    CalcOptions helio_icrf;
    helio_icrf.center = Center::Heliocentric;
    helio_icrf.frame = Frame::ICRF;
    CalcOptions topo;
    topo.center = Center::Topocentric;
    topo.site = {-70.25 * kDegToRad, -30.5 * kDegToRad, 2200.0};
    CalcOptions lahiri;
    lahiri.sidereal = SiderealMode::Lahiri;
    CalcOptions lahiri_ltp = lahiri;
    lahiri_ltp.precession = Precession::Vondrak2011;
    CalcOptions user;
    user.sidereal = SiderealMode::User;
    user.sidereal_epoch_jtdb = 2440000.5;
    user.sidereal_ayanamsa_deg = 22.25;
    CalcOptions partial;
    partial.deflection = false;
    partial.speed = false;
    CalcOptions bary;
    bary.center = Center::Barycentric;
    const Case cases[] = {
        {"--geometric --frame j2000 --equatorial", geo_j2000_eq},
        {"--astrometric --frame=mean", astro_mean},
        {"--center helio --frame icrf", helio_icrf},
        {"--site=-70.25,-30.5,2200", topo},
        {"--center topo --site -70.25,-30.5,2200", topo},
        {"--sidereal lahiri", lahiri},
        {"--precession vondrak2011 --sidereal lahiri", lahiri_ltp},
        {"--sidereal user:2440000.5:22.25", user},
        {"--no-deflection --no-speed", partial},
        {"--center barycentric", bary},
    };
    for (const Case& c : cases) {
        const Run r = ephem(k.arg + " -j 2455000.5 -f csv jupiter " + c.args);
        auto rows = parse_csv(r.out);
        const auto want = k.engine.calc(5, jd, c.opts);
        CHECK(r.status == 0);
        CHECK(rows.size() == 1);
        CHECK(want.ok());
        if (rows.size() == 1 && want.ok() && !same(rows[0], want.value())) {
            std::printf("  mismatch for: %s\n", c.args);
            CHECK(false);
        }
    }
}

TEST_CASE("ephem_ut1_and_fixed_delta_t") {
    Kernel k;
    struct Fixed final : time::DeltaTModel {
        double delta_t_seconds(double) const override { return 42.5; }
    } fixed;
    CalcOptions topo;
    topo.center = Center::Topocentric;
    topo.site = {10.0 * kDegToRad, 50.0 * kDegToRad, 0.0};

    Run r = ephem(k.arg + " --scale ut1 -j 2455000.5 --delta-t 42.5 --site 10,50 -f csv sun");
    auto rows = parse_csv(r.out);
    k.engine.set_delta_t_model(&fixed);
    auto want = k.engine.calc_ut(10, 2455000.5, topo);
    CHECK(r.status == 0);
    CHECK(rows.size() == 1);
    CHECK(want.ok());
    if (rows.size() == 1 && want.ok()) {
        CHECK(same(rows[0], want.value()));
        CHECK(std::fabs(rows[0].jd_tt - (2455000.5 + 42.5 / 86400.0)) < 1e-9);
    }

    // A calendar date in TT before the UTC era.
    k.engine.set_delta_t_model(nullptr);
    r = ephem(k.arg + " --scale tt -t 1950-06-01T12:00 -f csv jupiter");
    rows = parse_csv(r.out);
    const double jd = time::jd_from_ymdhms(1950, 6, 1, 12, 0, 0.0);
    CHECK(r.status == 0);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        CHECK((rows[0].jd_tt == jd && rows[0].utc.empty())); // no UTC label before 1972
        CHECK(same(rows[0], k.engine.calc(5, jd).value()));
    }
}

TEST_CASE("ephem_catalog_bodies") {
    Kernel k;
    const std::string cat = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    CHECK(k.engine.add_catalog(cat).ok());
    const double jd = 2460600.5;
    const auto want = k.engine.calc(20000001, jd);
    CHECK(want.ok());

    Run r = ephem(k.arg + " -c " + cat + " -j 2460600.5 -f csv Ceres @1 20000001");
    auto rows = parse_csv(r.out);
    CHECK(r.status == 0);
    CHECK(rows.size() == 3);
    for (const CsvRow& row : rows) {
        CHECK(row.id == 20000001);
        CHECK(!row.has_sigma); // element sigmas only, no covariance
        CHECK(want.ok());
        CHECK(same(row, want.value()));
    }
    if (rows.size() == 3)
        CHECK((rows[0].body == "Ceres" && rows[1].body == "1"));

    // The same catalog through the environment.
    r = ephem(k.arg + " -j 2460600.5 -f csv ceres", "PROMETHEIA_CATALOGS=:" + cat + ":");
    rows = parse_csv(r.out);
    CHECK((r.status == 0 && rows.size() == 1 && want.ok() && same(rows[0], want.value())));
}

TEST_CASE("ephem_star_bodies") {
    Kernel k;
    Run r = ephem(k.arg + " -j 2451545.0 -f csv star:Graffias 'star:Beta Scorpii' star:M45");
    auto rows = parse_csv(r.out);
    CHECK(r.status == 0);
    REQUIRE(rows.size() == 3);
    const size_t acrab = stars::find("Acrab").value();
    CHECK(same(rows[0], k.engine.calc_star(acrab, 2451545.0).value()));
    CHECK(same(rows[1], k.engine.calc_star(acrab, 2451545.0).value()));
    CHECK(same(rows[2], k.engine.calc_star(stars::find("M45").value(), 2451545.0).value()));
    r = ephem(k.arg + " -j 2451545.0 star:Vulcan");
    CHECK(r.status != 0);
}

TEST_CASE("ephem_hypothetical_bodies") {
    Kernel k;
    // Two invented bodies; every number is made up for the test.
    TempFile file("ephem-hyp");
    {
        FILE* f = std::fopen(file.path.c_str(), "wb");
        REQUIRE(f);
        std::fputs(
            R"({"token":"testone","name":"Test One","set":"Invented","citation":"tests/test_ephem.cpp","epoch":2451545.0,"equinox":"J2000","M":[10],"a":[40],"e":[0.02],"w":[20],"node":[30],"i":[1.5]})"
            "\n"
            R"({"token":"testtwo","set":"Invented","citation":"tests/test_ephem.cpp","epoch":2415020.0,"equinox":"J1900","origin":"earth","M":[5,1000],"a":[0.01],"e":[0],"w":[0],"node":[0],"i":[0]})"
            "\n",
            f);
        std::fclose(f);
    }
    REQUIRE(k.engine.add_hypotheticals(file.path.string()).ok());
    const std::string opt = " --hypotheticals " + file.path.string();

    Run r = ephem(k.arg + opt + " -j 2452000.5 -f csv hyp:testone hyp:TestTwo");
    auto rows = parse_csv(r.out);
    CHECK(r.status == 0);
    REQUIRE(rows.size() == 2);
    CHECK(same(rows[0], k.engine.calc_hypothetical("testone", 2452000.5).value()));
    CHECK(same(rows[1], k.engine.calc_hypothetical("testtwo", 2452000.5).value()));

    // hyp:all is every body defined; the environment variable adds files too.
    r = ephem(k.arg + " -j 2452000.5 -f csv hyp:all",
              "PROMETHEIA_HYPOTHETICALS=" + file.path.string());
    CHECK(r.status == 0);
    CHECK(parse_csv(r.out).size() == k.engine.hypothetical_tokens().size());

    r = ephem(k.arg + opt + " -j 2452000.5 hyp:nosuch");
    CHECK(r.status == 1);
    CHECK(r.err.find("unknown hypothetical body 'nosuch'") != std::string::npos);

    // A malformed element file stops ephem before any output, naming the line.
    {
        FILE* f = std::fopen(file.path.c_str(), "wb");
        REQUIRE(f);
        std::fputs("{\"token\":\"broken\"}\n", f);
        std::fclose(f);
    }
    r = ephem(k.arg + opt + " -j 2452000.5 hyp:all");
    CHECK(r.status == 2);
    CHECK(r.err.find(":1:") != std::string::npos);
}

TEST_CASE("ephem_table_and_json") {
    Kernel k;
    Run r = ephem(k.arg + " -j 2451545 sun jupiter");
    CHECK(r.status == 0);
    CHECK(r.out.find("# NAIF SPK kernel") == 0);
    CHECK(r.out.find("geocentric, apparent, ecliptic and true equinox of date, tropical") !=
          std::string::npos);
    CHECK(r.out.find("2000-01-01 11:58:55.816 UTC, JD 2451545.000000 TT") != std::string::npos);
    CHECK(r.out.find("\nSun ") != std::string::npos);
    CHECK(r.out.find("\nJupiter ") != std::string::npos);

    r = ephem(k.arg + " -j 2451545 --dms --equatorial jupiter");
    CHECK(r.status == 0);
    CHECK(r.out.find("RA") != std::string::npos);
    CHECK(r.out.find("h") != std::string::npos);
    CHECK(r.out.find("\xC2\xB0") != std::string::npos);

    r = ephem(k.arg + " -j 2451545 -n 2 sun");
    CHECK(r.status == 0);
    CHECK(r.out.find("# 2 rows from JD 2451545.000000 TT") != std::string::npos);

    r = ephem(k.arg + " -j 2451545 -f json sun 5 --sidereal fb");
    CHECK(r.status == 0);
    CHECK(r.out.find("{\"ephemeris\": \"NAIF SPK kernel") == 0);
    CHECK(r.out.find("\"body\": \"Sun\", \"id\": 10") != std::string::npos);
    CHECK(r.out.find("\"body\": \"Jupiter\", \"id\": 5") != std::string::npos);
    CHECK(r.out.find("\"sigma_arcsec\": null, \"ayanamsa_deg\": 2") != std::string::npos);
    CHECK(r.out.find("\"zodiac\": \"sidereal (Fagan/Bradley)\"") != std::string::npos);
    CHECK(r.out.substr(r.out.size() - 3) == "]}\n");
}

TEST_CASE("ephem_errors_and_status") {
    Kernel k;
    // Usage and setup errors: status 2, message on stderr, nothing on stdout.
    const char* const usage[] = {
        "-j 2451545 sun", // (no ephemeris: the Kernel arg is omitted below)
        "--frame bogus",
        "--center topo", // topo without a site
        "--jd",          // missing value
        "--jd abc",
        "-t 2023-02-30", // no such date
        "-t 2023-13-01",
        "-t 1960-01-01",          // UTC before 1972
        "--scale utc -j 2451545", // a JD is never UTC
        "--count 0",
        "--step 1w",
        "--site 1,95",
        "--sidereal krishnamurti",
        "--precession iau1976",
        "--geometric=yes",
        "--bogus",
        "-f xml",
    };
    for (size_t i = 0; i < std::size(usage); ++i) {
        const Run r = ephem(i == 0 ? std::string(usage[i]) : k.arg + " " + usage[i]);
        if (r.status != 2 || r.err.empty() || !r.out.empty()) {
            std::printf("  expected usage error for: %s (status %d)\n", usage[i], r.status);
            CHECK(false);
        }
    }
    Run r = ephem("-e /nonexistent/kernel.bsp sun");
    CHECK(r.status == 2);
    CHECK(r.err.find("cannot open") != std::string::npos);
    r = ephem(k.arg + " -p /nonexistent/sb441.bsp sun");
    CHECK(r.status == 2);
    CHECK(r.err.find("perturbers") != std::string::npos);
    r = ephem(k.arg + " -p " + k.file.path.string() + " sun"); // planets only: no asteroids
    CHECK(r.status == 2);
    r = ephem(k.arg + " -c /nonexistent/cat.epm sun");
    CHECK(r.status == 2);
    CHECK(r.err.find("catalog") != std::string::npos);

    // Per-body failures: status 1, the good rows still printed.
    r = ephem(k.arg + " -j 2451545 -f csv sun nosuchbody 499 earth jupiter");
    CHECK(r.status == 1);
    CHECK(parse_csv(r.out).size() == 2);
    CHECK(r.err.find("unknown body 'nosuchbody'") != std::string::npos);
    CHECK(r.err.find("499") != std::string::npos);
    CHECK(r.err.find("Earth") != std::string::npos); // the observer as the body
    r = ephem(k.arg + " -j 2600000 -f csv sun");     // outside the kernel
    CHECK(r.status == 1);
    CHECK(parse_csv(r.out).empty());

    // Help, version, and the environment's ephemeris.
    r = ephem("--help");
    CHECK(r.status == 0);
    CHECK(r.out.find("Usage: ephem") == 0);
    r = ephem("-V");
    CHECK(r.status == 0);
    CHECK(r.out.find("ephem (Ephemeris Prometheia) 0.1.0") == 0);
    r = ephem("-j 2451545 -f csv sun", "PROMETHEIA_EPHEMERIS=" + k.file.path.string());
    CHECK(r.status == 0);
    CHECK(parse_csv(r.out).size() == 1);
}

} // namespace
