// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::stars — the fixed-star and Messier catalog compiled into the
// library (docs/STARS.md): the naked-eye stars of the Yale Bright Star
// Catalogue with Hipparcos astrometry, further IAU-named stars, the Messier
// objects, their IAU and traditional names, and the constellation
// boundaries. Positions are computed by Engine::calc_star.
//
// Every function here is a pure lookup over constant data: thread-safe, no
// allocation beyond the returned strings and vectors.
#ifndef PROMETHEIA_STARS_HPP
#define PROMETHEIA_STARS_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "prometheia/error.hpp"

namespace prometheia::stars {

enum class Kind {
    Star,
    Galaxy,
    GlobularCluster,
    OpenCluster,
    Nebula,
    PlanetaryNebula,
    SupernovaRemnant,
    Asterism,
    DoubleStar,
};

// Where an object's astrometry comes from.
enum class Astrometry {
    Hipparcos,  // new reduction (van Leeuwen 2007), epoch J1991.25
    BrightStar, // Yale Bright Star Catalogue, J2000, 1" / 0.1 s
    Simbad,     // deep sky, J2000, no motion
};

struct Object {
    size_t index = 0; // position in the catalog (stable within a release)
    Kind kind = Kind::Star;
    Astrometry astrometry = Astrometry::Hipparcos;

    // Catalog numbers; 0 where the object has none.
    int hr = 0, hd = 0, hip = 0, flamsteed = 0, messier = 0;
    int bayer = 0;       // Greek letter 1 (alpha) .. 24 (omega), 0 none
    int bayer_index = 0; // superscript (beta1 = 1), 0 none
    // IAU abbreviation ("Sco"): the catalog's designation constellation, else
    // the one whose boundaries contain the position.
    std::string_view constellation;

    double vmag = 0.0; // V magnitude; NaN where unknown (deep sky)
    // Astrometry: ICRS right ascension and declination (degrees) at
    // epoch_jyear (Julian year, TDB); proper motion in RA * cos(Dec) and in
    // Dec (mas/yr); parallax (mas, <= 0 when unknown); radial velocity
    // (km/s, positive receding).
    double ra_deg = 0.0, dec_deg = 0.0, epoch_jyear = 2000.0;
    double pm_ra_mas_yr = 0.0, pm_dec_mas_yr = 0.0, parallax_mas = 0.0, rv_km_s = 0.0;
    double size_arcmin = 0.0; // deep sky: major axis
    std::string_view spectral_type;
    std::vector<std::string_view> names; // IAU name first, then traditional

    // The display name: the first name, else the Bayer or Flamsteed
    // designation, else "M n", "HR n" or "HIP n".
    std::string name() const;
    // "β¹ Sco" / "80 UMa", empty without one.
    std::string bayer_designation() const;
    std::string flamsteed_designation() const;
    // "Beta1 Scorpii" style: the Bayer letter spelled out and the
    // constellation's genitive; empty without a Bayer letter.
    std::string bayer_designation_long() const;
    // Every designation the object answers to: HR, HD, HIP, M, Bayer and
    // Flamsteed forms.
    std::vector<std::string> designations() const;
};

size_t count();
// Throws nothing; index must be < count().
const Object& at(size_t index);

enum class MatchQuality {
    Exact,  // a name or designation equal to the query
    Alias,  // the query leaves out something the object has (a Bayer
            // superscript: "Beta Sco" for beta1 and beta2 Sco)
    Prefix, // the query is the start of a name (prefix lookups only)
};

struct Match {
    size_t index;
    MatchQuality quality;
    std::string matched; // the name or designation that matched
};

// Every object answering to `query`, best first (exact, alias, prefix; then
// brighter). Matching ignores case, spaces, punctuation and accents, and
// understands:
//   - names: "Acrab", "Graffias", "Zuben Elgenubi", "Pleiades";
//   - Bayer: "bet Sco", "Beta Sco", "Beta Scorpii", "β Sco", "beta1 sco", "β¹ Sco";
//   - Flamsteed: "80 UMa", "80 Ursae Majoris";
//   - catalog numbers: "HR 5984", "HD 144217", "HIP 78820", "M 45", "M45",
//     "Messier 45".
// With prefix, names that start with the query are added (at least 3
// letters). Up to max_matches results.
std::vector<Match> lookup(std::string_view query, size_t max_matches = 16, bool prefix = false);

// The one object a query means: the best match, and among several equally
// good ones the brightest when they are components of one designation
// ("Beta Sco"), else ArgumentError naming the candidates. NotFound when
// nothing matches.
Result<size_t> find(std::string_view query);

// Constellations: the 88 IAU abbreviations, names and genitives.
struct Constellation {
    std::string_view abbreviation; // "Sco"
    std::string_view name;         // "Scorpius"
    std::string_view genitive;     // "Scorpii"
};
const std::vector<Constellation>& constellations();
// The constellation containing an ICRS direction (right ascension and
// declination, degrees). The Delporte (1930) boundaries, as arranged by
// Roman (1987), are fixed to the mean equator and equinox of B1875.0; the
// direction is carried there by frame bias and IAU 2006 precession.
std::string_view constellation_at(double ra_deg, double dec_deg);

// Greek letter names: 1 -> "alpha", "alp", "α".
std::string_view greek_letter_name(int letter);
std::string_view greek_letter_abbreviation(int letter);
std::string_view greek_letter_symbol(int letter);

} // namespace prometheia::stars

#endif // PROMETHEIA_STARS_HPP
