// SPDX-License-Identifier: GPL-2.0-or-later
//
// The fixed-star catalog: lookup, designations, constellations (always), and
// apparent places against ERFA (tests/star_fixtures.inc; needs DE440, SKIPs
// without it).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <prometheia/engine.hpp>
#include <prometheia/stars.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

struct StarFixture {
    const char* label;
    const char* lookup;
    double jd_tt;
    double ra, dec, lon, lat; // apparent, true of date
    double ara, adec;         // astrometric, ICRS
};

#include "star_fixtures.inc"

size_t must_find(const char* q) {
    auto r = stars::find(q);
    REQUIRE_MESSAGE(r.ok(), q);
    return r.value();
}

double sep_mas(double ra1, double dec1, double ra2, double dec2) {
    const double k = 3.14159265358979323846 / 180.0;
    const double a[3] = {std::cos(dec1 * k) * std::cos(ra1 * k),
                         std::cos(dec1 * k) * std::sin(ra1 * k), std::sin(dec1 * k)};
    const double b[3] = {std::cos(dec2 * k) * std::cos(ra2 * k),
                         std::cos(dec2 * k) * std::sin(ra2 * k), std::sin(dec2 * k)};
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                         a[0] * b[1] - a[1] * b[0]};
    const double s = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(s, d) / k * 3.6e6;
}

} // namespace

TEST_CASE("stars_catalog_contents") {
    CHECK(stars::count() > 9200);
    const stars::Object& acrab = stars::at(must_find("Acrab"));
    CHECK(acrab.hr == 5984);
    CHECK(acrab.hd == 144217);
    CHECK(acrab.hip == 78820);
    CHECK(acrab.bayer == 2);
    CHECK(acrab.bayer_index == 1);
    CHECK(acrab.constellation == "Sco");
    CHECK(acrab.astrometry == stars::Astrometry::Hipparcos);
    CHECK(acrab.epoch_jyear == 1991.25);
    CHECK(acrab.names.size() >= 2);
    CHECK(acrab.names[0] == "Acrab");
    CHECK(acrab.name() == "Acrab");
    CHECK(acrab.bayer_designation() == "β¹ Sco");
    CHECK(acrab.bayer_designation_long() == "Beta1 Scorpii");
    CHECK(acrab.flamsteed_designation() == "8 Sco");
    const auto d = acrab.designations();
    CHECK(std::find(d.begin(), d.end(), "HIP 78820") != d.end());

    const stars::Object& m31 = stars::at(must_find("M 31"));
    CHECK(m31.kind == stars::Kind::Galaxy);
    CHECK(m31.constellation == "And");
    CHECK(std::isnan(m31.vmag));
    CHECK(m31.name() == "M 31");
    CHECK(stars::at(must_find("M 45")).kind == stars::Kind::OpenCluster);
    CHECK(stars::at(must_find("M 40")).kind == stars::Kind::DoubleStar);
    for (int n = 1; n <= 110; ++n) {
        CHECK(stars::find("M" + std::to_string(n)).ok());
    }
    CHECK(stars::constellations().size() == 88);
    CHECK(stars::greek_letter_name(24) == "omega");
    CHECK(stars::greek_letter_symbol(2) == "β");
}

TEST_CASE("stars_lookup_forms") {
    const size_t acrab = must_find("Acrab");
    for (const char* q :
         {"acrab", "GRAFFIAS", "Elacrab", "HR 5984", "hr5984", "HD 144217", "HIP 78820", "β¹ Sco",
          "bet1 Sco", "Beta1 Scorpii", "8 Sco", "8 Scorpii", "Beta Scorpii", "bet Sco", "β Sco"}) {
        auto r = stars::find(q);
        CHECK_MESSAGE((r.ok() && r.value() == acrab), q);
    }
    // Without the superscript both components answer, as aliases, brighter first.
    const auto both = stars::lookup("Beta Sco");
    REQUIRE(both.size() == 2);
    CHECK(both[0].index == acrab);
    CHECK(both[0].quality == stars::MatchQuality::Alias);
    CHECK(stars::find("beta2 Sco").value() != acrab);
    // Greek letters, accents and spacing.
    CHECK(stars::find("α CMa").value() == must_find("Sirius"));
    CHECK(stars::find("ALPHA canis majoris").value() == must_find("Sirius"));
    CHECK(stars::find("Zuben Elgenubi").value() == must_find("Zubenelgenubi"));
    CHECK(stars::find("Messier 44").value() == must_find("Praesepe"));
    CHECK(stars::find("alf Psc").value() == must_find("Alrescha"));
    CHECK(stars::find("Cynosura").value() == must_find("Polaris"));
    // Prefix lookups add names that start with the query.
    const auto alg = stars::lookup("Alg", 20, true);
    CHECK(alg.size() >= 4);
    for (const auto& m : alg)
        CHECK(m.quality == stars::MatchQuality::Prefix);
    CHECK(stars::lookup("Al", 20, true).empty()); // prefixes need 3 letters
    // Misses and ambiguity.
    CHECK(stars::find("Vulcan").error().code == ErrorCode::NotFound);
    CHECK(stars::lookup("").empty());
}

TEST_CASE("stars_constellation_boundaries") {
    // The examples of the boundary data's ReadMe (Roman 1987), given at
    // equinox B1875 there; here as ICRS directions a few arcminutes away,
    // well inside each region.
    CHECK(stars::constellation_at(9.0 * 15.0, 65.0) == "UMa");
    CHECK(stars::constellation_at(23.5 * 15.0, -20.0) == "Aqr");
    CHECK(stars::constellation_at(5.12 * 15.0, 9.12) == "Ori");
    CHECK(stars::constellation_at(0.0, 90.0) == "UMi");
    CHECK(stars::constellation_at(0.0, -90.0) == "Oct");
    // Every designated Bright Star Catalogue star lies in its designation's
    // constellation (the 1930 boundaries were drawn around them; the few
    // stars left outside, like 10 UMa in Lyn, carry no designation there).
    int designated = 0, agree = 0;
    for (size_t i = 0; i < stars::count(); ++i) {
        const stars::Object& o = stars::at(i);
        if (!o.hr || !(o.bayer || o.flamsteed))
            continue;
        ++designated;
        agree += stars::constellation_at(o.ra_deg, o.dec_deg) == o.constellation;
    }
    CHECK(designated > 3000);
    CHECK(agree == designated);
}

TEST_CASE("stars_apparent_place_vs_erfa") {
    const char* env = std::getenv("PROMETHEIA_DE440");
    const std::string de =
        (env && *env) ? env : std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440";
    if (access(de.c_str(), F_OK) != 0) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    auto e = Engine::open(de);
    REQUIRE(e.ok());
    double worst_app = 0.0, worst_ecl = 0.0, worst_astro = 0.0;
    std::string worst_label;
    for (const StarFixture& f : kStarFixtures) {
        const size_t i = must_find(f.lookup);
        CalcOptions eq;
        eq.coords = Coords::Equatorial;
        const auto a = e.value().calc_star(i, f.jd_tt, eq);
        REQUIRE(a.ok());
        const auto l = e.value().calc_star(i, f.jd_tt, CalcOptions{});
        REQUIRE(l.ok());
        CalcOptions astro = CalcOptions::astrometric();
        astro.frame = Frame::ICRF;
        astro.coords = Coords::Equatorial;
        const auto s = e.value().calc_star(i, f.jd_tt, astro);
        REQUIRE(s.ok());
        const double da = sep_mas(a.value().pos.lon_deg, a.value().pos.lat_deg, f.ra, f.dec);
        const double dl = sep_mas(l.value().pos.lon_deg, l.value().pos.lat_deg, f.lon, f.lat);
        const double ds = sep_mas(s.value().pos.lon_deg, s.value().pos.lat_deg, f.ara, f.adec);
        if (da > worst_app)
            worst_label = std::string(f.label) + " " + std::to_string(f.jd_tt);
        worst_app = std::max(worst_app, da);
        worst_ecl = std::max(worst_ecl, dl);
        worst_astro = std::max(worst_astro, ds);
    }
    std::printf("  vs ERFA: apparent %.3f mas (%s), ecliptic %.3f mas, astrometric %.3f mas\n",
                worst_app, worst_label.c_str(), worst_ecl, worst_astro);
    CHECK(worst_app < 1.0);
    CHECK(worst_ecl < 1.0);
    CHECK(worst_astro < 1.0);
}
