// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::Engine: ephemeris sources, the apparent-place pipeline and
// frame output. See include/prometheia/engine.hpp and docs/ENGINE.md.
#include "prometheia/engine.hpp"
#include "prometheia/stars.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "natural_apsides.hpp"
#include "prometheia/apparent.hpp"
#include "prometheia/catalog.hpp"
#include "prometheia/de.hpp"
#include "prometheia/elements.hpp"
#include "prometheia/forces.hpp"
#include "prometheia/hypotheticals.hpp"
#include "prometheia/kepler.hpp"
#include "prometheia/memo.hpp"
#include "prometheia/spk.hpp"

namespace prometheia {

namespace natural_apsides {
#include "natural_apsides.inc"
} // namespace natural_apsides
namespace {

constexpr double kAuKm = 149597870.7; // IAU 2012 Resolution B2 (exact)
// Obliquity defining JPL's J2000 ecliptic frame (SBDB elements, Horizons,
// SPICE ECLIPJ2000): the IAU 1976 value, applied to the ICRF without bias.
constexpr double kJplEclipticObliquityArcsec = 84381.448;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kJ2000 = 2451545.0;
constexpr double kB1950 = 2433282.42345905; // Besselian B1950.0, TT
constexpr double kJ1900 = 2415020.0;
// Earth rotation rate in rad per UT1 day (the ERA rate, Circular 179).
constexpr double kEarthRotationRadPerDay = kTwoPi * 1.00273781191135448;
// SBDB SPK-IDs of numbered asteroids are 20000000 + number; older NAIF
// kernels (JPL's SB441-N16) use 2000000 + number.
constexpr long kSpkidNumberedBase = 20000000;
constexpr long kOldSpkidNumberedBase = 2000000;
// Central-difference half step for rates (days): 1/4096 (21 s), exact in
// binary. Measured against a five-point difference of positions (ENGINE.md,
// "Rates"): the three-point truncation, h^2 f'''/6, is set by a topocentric
// Moon's diurnal parallax and was 2.6e-5 deg/day at 1e-3; here 2.9e-6.
// Smaller steps lose to roundoff (4.7e-6 at 1/8192).
constexpr double kSpeedStepDays = 1.0 / 4096.0;

// ---------------------------------------------------------------------------
// Zodiacs defined at the instant (docs/FRAMES.md, "Zodiacs defined at the
// instant"). Definitions from the Swiss Ephemeris general documentation
// (Astrodienst, published; tools/fetch/stars_fetch.py, swisseph-doc),
// sections 2.8.7-2.8.9 and 2.8.12 items 4-5: the anchor's TRUE position (no
// aberration, no deflection) held at a fixed longitude on the true ecliptic
// of date; longitude, not polar, except Wilhelm's, which is polar by
// definition. Data, each from a pinned source in the same fetch tool:
// - the four stars: this catalog (Hipparcos new reduction), by HR number;
// - Sgr A*: SIMBAD's ICRS position (Petrov et al. 2011, VLBI), with the
//   apparent motion of Reid & Brunthaler 2020 (ApJ 892, 39): -6.411 mas/yr
//   along the Galactic plane, -0.219 mas/yr toward the pole;
// - the galactic poles in ICRS from Liu, Zhu & Zhang 2011 (A&A 526, A16):
//   the IAU 1958 pole carried into the ICRS (their eq. 19) and the pole of
//   the system centred on Sgr A* (their eq. 22), which is the "true/modern"
//   one: it moves the galactic node by the 3'11" the documentation quotes.
enum class ZodiacAnchor {
    Star,                // a catalog star at `at_deg`
    GalacticCentre,      // Sgr A* at `at_deg`
    GalacticCentrePolar, // the ecliptic point on Sgr A*'s hour circle at `at_deg`
    GalacticNode,        // the galactic equator's node near 0° Capricorn at `at_deg`
};

struct InstantZodiac {
    SiderealMode mode;
    ZodiacAnchor anchor;
    int hr;           // ZodiacAnchor::Star
    bool modern_pole; // ZodiacAnchor::GalacticNode: Liu et al.'s eq. 22 pole
    double at_deg;    // the anchor's sidereal longitude
};

constexpr double kMula = 246.0 + 40.0 / 60.0; // the middle of the nakshatra Mula
// Gil Brand: the golden section of the 90° from 0° Scorpio to 0° Aquarius,
// the shorter part from 0° Scorpio (the documentation: "very close to the
// ayanamsha of B.V. Raman", which the other section is not).
constexpr double kGilBrand = 210.0 + 90.0 * 0.38196601125010515;

constexpr InstantZodiac kInstantZodiacs[] = {
    {SiderealMode::GalacticCentre0Sag, ZodiacAnchor::GalacticCentre, 0, false, 240.0},
    {SiderealMode::TrueCitra, ZodiacAnchor::Star, 5056, false, 180.0},
    {SiderealMode::TrueRevati, ZodiacAnchor::Star, 361, false, 359.0 + 50.0 / 60.0},
    {SiderealMode::TruePushya, ZodiacAnchor::Star, 3461, false, 106.0},
    {SiderealMode::GalacticCentreGilBrand, ZodiacAnchor::GalacticCentre, 0, false, kGilBrand},
    {SiderealMode::GalacticEquatorIau1958, ZodiacAnchor::GalacticNode, 0, false, 240.0},
    {SiderealMode::GalacticEquatorTrue, ZodiacAnchor::GalacticNode, 0, true, 240.0},
    {SiderealMode::GalacticEquatorMula, ZodiacAnchor::GalacticNode, 0, true, kMula},
    {SiderealMode::TrueMula, ZodiacAnchor::Star, 6527, false, 240.0},
    {SiderealMode::GalacticCentreMulaWilhelm, ZodiacAnchor::GalacticCentrePolar, 0, false, kMula},
    {SiderealMode::GalacticCentreCochrane, ZodiacAnchor::GalacticCentre, 0, false, 270.0},
};

const InstantZodiac* instant_zodiac(SiderealMode mode) {
    for (const InstantZodiac& z : kInstantZodiacs) {
        if (z.mode == mode)
            return &z;
    }
    return nullptr;
}

// Sgr A* (SIMBAD, ICRS; epoch taken as J2000.0) and its apparent motion.
constexpr double kSgrARaDeg = 266.41681662499997, kSgrADecDeg = -29.00782497222222;
constexpr double kSgrAMuLMasYr = -6.411, kSgrAMuBMasYr = -0.219;
// North galactic poles in the ICRS (Liu, Zhu & Zhang 2011, eqs. 19 and 22).
constexpr double kPoleIau1958RaDeg = 192.859477875, kPoleIau1958DecDeg = 27.128252416667;
constexpr double kPoleModernRaDeg = 192.902979992083, kPoleModernDecDeg = 27.103109214444;

// ---------------------------------------------------------------------------
// Binary stars whose companion bends the bright star's path (STARS.md,
// "Binary stars"). The catalog's straight line is Hipparcos's, a few years'
// motion that carries the orbit's velocity at its epoch; over centuries the
// orbit's curvature takes the star arcseconds from it (FK5: alpha Cen A 28.6",
// Sirius 2.3", Procyon 1.5"). Relative orbits from the Sixth Catalog of Orbits
// of Visual Binary Stars (USNO; tools/fetch/stars_fetch.py, orb6), masses from
// the orbits' own papers (the same tool's arxiv-* sources).
// What the catalog's straight line describes, from its Hipparcos solution
// type (I/311): the system's barycentre, where the published catalog fitted
// the orbit ("orbital binary", old-reduction type 4); or the star itself at
// the catalog epoch, orbital velocity included (an ordinary 5-parameter
// solution of one component).
// A secondary is placed from its primary's model plus the relative orbit, so
// the two stars keep the orbit's separation exactly: two independent
// component solutions do not (alpha Cen A and B's disagree by more than a
// second of arc in ten years), and the primary's solution is the better one
// (alpha Cen A's proper motion to 3-4 mas/yr, B's 20-26; FK5 holds A's
// implied barycentre to 0.72" over two centuries). The secondary's own
// catalog line then gives only its distance.
enum class BinaryLine { Barycentre, Component, Secondary };

struct BinaryOrbit {
    int hip; // the catalog star this bends
    BinaryLine line;
    int partner_hip;  // BinaryLine::Secondary: the primary
    double fraction;  // its share of the relative orbit: -M_B/M for a primary,
                      // +M_A/M for the secondary (the orbit is B about A)
    double period_yr; // P
    double a_arcsec;  // semi-major axis of the relative orbit
    double i_deg, node_deg, omega_deg, e;
    double t_peri_yr; // periastron, Besselian year
};

constexpr BinaryOrbit kBinaryOrbits[] = {
    // Sirius A (Bond et al. 2017: 2.063 and 1.018 Msun).
    {32349, BinaryLine::Barycentre, 0, -1.018 / (2.063 + 1.018), 50.1284, 7.4957, 136.336, 45.400,
     149.161, 0.59142, 1994.5715},
    // Procyon A (Bond et al. 2015: 1.478 and 0.592 Msun).
    {37279, BinaryLine::Barycentre, 0, -0.592 / (1.478 + 0.592), 40.840, 4.3075, 31.408, 100.683,
     89.23, 0.39785, 1968.076},
    // alpha Cen A and B (Akeson et al. 2021, Table 8: the orbit, and the mass
    // fraction m_A / (m_A + m_B) = 0.54266 fitted with it).
    {71683, BinaryLine::Component, 0, -(1.0 - 0.54266), 79.762, 17.4930, 79.2430, 205.073, 231.519,
     0.51947, 1955.564},
    {71681, BinaryLine::Secondary, 71683, 0.54266, 79.762, 17.4930, 79.2430, 205.073, 231.519,
     0.51947, 1955.564},
};

const BinaryOrbit* binary_orbit(int hip) {
    for (const BinaryOrbit& b : kBinaryOrbits) {
        if (b.hip == hip)
            return &b;
    }
    return nullptr;
}

// The star's offset from its system's barycentre at jd, arcsec toward east
// (RA x cos Dec) and north: the relative orbit (Thiele-Innes; position angle
// from north through east) scaled by the star's share.
void binary_offset(const BinaryOrbit& b, double jd, double& east, double& north) {
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    const double t_peri_jd = 2415020.31352 + (b.t_peri_yr - 1900.0) * 365.242198781;
    const double mean = kTwoPi * (jd - t_peri_jd) / (b.period_yr * 365.25);
    double ecc = mean; // Kepler's equation, Newton's method
    for (int k = 0; k < 30; ++k) {
        const double d = (ecc - b.e * std::sin(ecc) - mean) / (1.0 - b.e * std::cos(ecc));
        ecc -= d;
        if (std::fabs(d) < 1e-15)
            break;
    }
    const double x = std::cos(ecc) - b.e, y = std::sqrt(1.0 - b.e * b.e) * std::sin(ecc);
    const double cw = std::cos(b.omega_deg * kRad), sw = std::sin(b.omega_deg * kRad);
    const double cn = std::cos(b.node_deg * kRad), sn = std::sin(b.node_deg * kRad);
    const double ci = std::cos(b.i_deg * kRad);
    const double A = b.a_arcsec * (cw * cn - sw * sn * ci),
                 B = b.a_arcsec * (cw * sn + sw * cn * ci);
    const double F = b.a_arcsec * (-sw * cn - cw * sn * ci),
                 G = b.a_arcsec * (-sw * sn + cw * cn * ci);
    north = b.fraction * (A * x + F * y);
    east = b.fraction * (B * x + G * y);
}

void unit_radec(double ra_deg, double dec_deg, double u[3]) {
    const double a = ra_deg / kRad2Deg, d = dec_deg / kRad2Deg;
    u[0] = std::cos(d) * std::cos(a);
    u[1] = std::cos(d) * std::sin(a);
    u[2] = std::sin(d);
}

// ---------------------------------------------------------------------------
// Ephemeris sources: barycentric ICRF states in km and km/day.

class Source {
public:
    virtual ~Source() = default;
    virtual Result<void> barycentric(int id, double jd_tdb, double out[6]) = 0;
    // Best-known GM of a perturbing mass (AU^3/day^2): published by the
    // ephemeris when it carries the constant, absent otherwise (the
    // caller falls back to the DE440 values in forces.hpp).
    virtual std::optional<double> gm_au3(int) const { return std::nullopt; }
    std::string description;
    int denum = 0;
};

double gm_or_builtin(const Source* s, int id) {
    if (auto g = s->gm_au3(id))
        return *g;
    switch (id) {
    case 10:
        return gm::kSun;
    case 1:
    case 199:
        return gm::kMercury;
    case 2:
    case 299:
        return gm::kVenus;
    case 3:
        return gm::kEarthMoonBary;
    case 399:
        return gm::kEarth;
    case 301:
        return gm::kMoon;
    case 4:
        return gm::kMars;
    case 5:
        return gm::kJupiter;
    case 6:
        return gm::kSaturn;
    case 7:
        return gm::kUranus;
    case 8:
        return gm::kNeptune;
    case 9:
        return gm::kPluto;
    }
    if (id > kSpkidNumberedBase && id < kSpkidNumberedBase + 1000000)
        return gm::asteroid(int(id - kSpkidNumberedBase));
    return 0.0;
}

// Mean elements of the planets, fitted to DE440 (tools/gen/gen_mean_elements.cpp).
struct MeanElementsFit {
    int id;
    double a[3], h[3], k[3], p[3], q[3]; // quadratics in T; see the table header
    double rms[5];
};
#include "mean_elements.inc"

// The Moon's mean orbit: node and perigee from the fundamental arguments;
// inclination, eccentricity and semi-major axis as constants (the published
// mean values of the lunar orbit).
constexpr double kMoonMeanInclinationDeg = 5.1453964;
constexpr double kMoonMeanEccentricity = 0.0549006;
constexpr double kMoonMeanDistanceKm = 384399.0;

class DeSource final : public Source {
public:
    explicit DeSource(de::DeFile f) : file_(std::move(f)) {
        denum = file_.header().denum;
        description = "JPL DE" + std::to_string(denum) + " binary";
    }

    Result<void> barycentric(int id, double jd_tdb, double out[6]) override {
        using T = de::Target;
        T target;
        switch (id) {
        case 0:
            for (int i = 0; i < 6; ++i)
                out[i] = 0.0;
            return {};
        case 1:
        case 199:
            target = T::Mercury;
            break;
        case 2:
        case 299:
            target = T::Venus;
            break;
        case 3:
            target = T::EarthMoonBary;
            break;
        case 399:
            target = T::Earth;
            break;
        case 301:
            target = T::Moon;
            break;
        case 10:
            target = T::Sun;
            break;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            target = static_cast<T>(id);
            break;
        default:
            return make_error(ErrorCode::NotFound,
                              "body " + std::to_string(id) + " is not in " + description);
        }
        return file_.relative_state(target, T::SolarSystemBary, jd_tdb, out);
    }

    // DE headers publish GMs in AU^3/day^2 (GM1..GM9, GMB, GMS; older
    // files may carry only some of them). Earth and Moon come from the
    // GMB/EMRAT split.
    std::optional<double> gm_au3(int id) const override {
        const de::Header& h = file_.header();
        auto c = [&](const char* name) -> std::optional<double> { return file_.constant(name); };
        switch (id) {
        case 10:
            return c("GMS");
        case 1:
        case 199:
            return c("GM1");
        case 2:
        case 299:
            return c("GM2");
        case 3:
            return c("GMB");
        case 399: {
            auto gmb = c("GMB");
            if (!gmb || !(h.emrat > 0.0))
                return std::nullopt;
            return *gmb * h.emrat / (1.0 + h.emrat);
        }
        case 301: {
            auto gmb = c("GMB");
            if (!gmb || !(h.emrat > 0.0))
                return std::nullopt;
            return *gmb / (1.0 + h.emrat);
        }
        case 4:
            return c("GM4");
        case 5:
            return c("GM5");
        case 6:
            return c("GM6");
        case 7:
            return c("GM7");
        case 8:
            return c("GM8");
        case 9:
            return c("GM9");
        }
        // Asteroid masses the DE integration used: MA0001, MA0002, ...
        if (id > kSpkidNumberedBase && id < kSpkidNumberedBase + 10000) {
            char name[8];
            std::snprintf(name, sizeof name, "MA%04d", int(id - kSpkidNumberedBase));
            return c(name);
        }
        return std::nullopt;
    }

private:
    de::DeFile file_;
};

class SpkSource final : public Source {
public:
    explicit SpkSource(spk::SpkFile f) : file_(std::move(f)) {
        description = "NAIF SPK kernel";
        if (!file_.internal_name().empty())
            description += " (" + file_.internal_name() + ")";
    }

    Result<void> barycentric(int id, double jd_tdb, double out[6]) override {
        if (id == 0) {
            for (int i = 0; i < 6; ++i)
                out[i] = 0.0;
            return {};
        }
        return file_.state(id, 0, jd_tdb, out);
    }

private:
    spk::SpkFile file_;
};

// Asteroid perturber kernel (JPL SB441-N16 and similar): heliocentric SPK
// segments for numbered asteroids under old-style ids (2000000 + number),
// answered under the SBDB SPK-ID (20000000 + number) as barycentric states
// by adding the main ephemeris's Sun.
class AsteroidSource final : public Source {
public:
    AsteroidSource(spk::SpkFile f, Source* main) : file_(std::move(f)), main_(main) {
        description = "asteroid perturbers";
        if (!file_.internal_name().empty())
            description += " (" + file_.internal_name() + ")";
        for (const spk::Segment& seg : file_.segments()) {
            const long num = long(seg.target) - kOldSpkidNumberedBase;
            if (seg.center == body::kSun && num > 0 && num < 1000000 &&
                std::find(bodies_.begin(), bodies_.end(), int(kSpkidNumberedBase + num)) ==
                    bodies_.end())
                bodies_.push_back(int(kSpkidNumberedBase + num));
        }
    }

    // SBDB SPK-IDs of the bodies the kernel carries.
    const std::vector<int>& bodies() const { return bodies_; }

    bool has(int id) const {
        return std::find(bodies_.begin(), bodies_.end(), id) != bodies_.end();
    }

    Result<void> barycentric(int id, double jd_tdb, double out[6]) override {
        if (!has(id))
            return make_error(ErrorCode::NotFound,
                              "body " + std::to_string(id) + " is not in " + description);
        const int kid = int(id - kSpkidNumberedBase + kOldSpkidNumberedBase);
        if (auto r = file_.state(kid, body::kSun, jd_tdb, out); !r)
            return r;
        double sun[6];
        if (auto r = main_->barycentric(body::kSun, jd_tdb, sun); !r)
            return r;
        for (int i = 0; i < 6; ++i)
            out[i] += sun[i];
        return {};
    }

    std::optional<double> gm_au3(int id) const override { return main_->gm_au3(id); }

private:
    spk::SpkFile file_;
    Source* main_;
    std::vector<int> bodies_;
};

// ---------------------------------------------------------------------------
// Small matrix helpers (row-major, r_out = M r_in), matching frames.cpp.

void rot1(double a, double m[9]) {
    const double c = std::cos(a), s = std::sin(a);
    const double r[9] = {1, 0, 0, 0, c, s, 0, -s, c};
    std::memcpy(m, r, sizeof r);
}
void rot3(double a, double m[9]) {
    const double c = std::cos(a), s = std::sin(a);
    const double r[9] = {c, s, 0, -s, c, 0, 0, 0, 1};
    std::memcpy(m, r, sizeof r);
}
void matmul(const double a[9], const double b[9], double out[9]) {
    double t[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            t[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    std::memcpy(out, t, sizeof t);
}
void apply(const double m[9], const double v[3], double out[3]) {
    const double x = v[0], y = v[1], z = v[2];
    out[0] = m[0] * x + m[1] * y + m[2] * z;
    out[1] = m[3] * x + m[4] * y + m[5] * z;
    out[2] = m[6] * x + m[7] * y + m[8] * z;
}
void apply_transpose(const double m[9], const double v[3], double out[3]) {
    const double x = v[0], y = v[1], z = v[2];
    out[0] = m[0] * x + m[3] * y + m[6] * z;
    out[1] = m[1] * x + m[4] * y + m[7] * z;
    out[2] = m[2] * x + m[5] * y + m[8] * z;
}

// Everything frame-related that depends only on the TT epoch.
struct EpochFrames {
    double jd_tt = NAN;
    frames::PrecessionModel model = frames::PrecessionModel::IAU2006;
    bool have_nutation = false;
    double pb[9];  // P * B: ICRF -> mean equator of date
    double npb[9]; // N * P * B: ICRF -> true equator of date
    double eps_mean = 0.0;
    double dpsi = 0.0, deps = 0.0;
};

// ---------------------------------------------------------------------------
// Small bodies: the barycentric force model fed from the ephemeris.

// The point masses that perturb (and attract) a catalog body. Mars..
// Pluto are system barycentres — exactly where DE puts the mass, since
// its GMs are system GMs; Earth and Moon are split from the EMB by
// EMRAT. Bodies the opened ephemeris does not carry are skipped, which
// is how the synthetic test kernels (three bodies) work; a DE file has
// them all.
constexpr int kPerturberIds[] = {10, 199, 299, 399, 301, 4, 5, 6, 7, 8, 9};

// Samples the ephemeris onto per-body cubic-Hermite tables, in blocks of
// kBlockDays, extending coverage lazily as integration windows march.
// With kBlockSamples per block the Hermite interpolation error of even
// Mercury (~2600 km) enters the asteroid's acceleration at the 1e-15
// level of the Sun's — orders below every tolerance the engine works to.
class PerturberSet final : public PerturberStates {
public:
    void attach(Source* s) { source_ = s; }

    // Adds the asteroid masses of a perturber kernel (after the Sun and
    // planets) and drops every table built so far; they rebuild lazily.
    void attach_asteroids(Source* s, std::vector<int> ids) {
        asteroid_source_ = s;
        asteroid_ids_ = std::move(ids);
        reset();
    }

    void reset() {
        built_ = false;
        ok_ = true;
        error_.clear();
        sun_index_ = -1;
        lo_ = hi_ = 0.0;
        ids_.clear();
        sources_.clear();
        mus_.clear();
        grid_.clear();
    }

    // PerturberStates. ensure() builds the table on first use and then
    // extends coverage; a failure (no masses at all, or the ephemeris
    // does not cover the epoch) is sticky and makes every later state
    // read zero — the engine refuses results after checking ok().
    void ensure(double t) override {
        if (!ok_)
            return;
        if (!built_) {
            build(t);
            if (!ok_)
                return;
        }
        while (ok_ && t > hi_)
            extend(kBlockDays);
        while (ok_ && t < lo_)
            extend(-kBlockDays);
    }

    void state(size_t i, double t, double out[6]) override {
        if (!ok_ || i >= ids_.size() || rows() == 0) {
            for (int k = 0; k < 6; ++k)
                out[k] = 0.0;
            return;
        }
        const Row r = locate(t);
        if (r.clamped)
            copy_sample(at(r.lo, i), out);
        else
            r.weights.apply(at(r.lo, i), at(r.lo + 1, i), out);
    }

    // One interval lookup and one set of Hermite weights serve every mass:
    // all tables share their sample epochs, stored row by row.
    void states(double t, double* out) override {
        const size_t n = ids_.size();
        if (!ok_ || n == 0 || rows() == 0) {
            for (size_t k = 0; k < 6 * n; ++k)
                out[k] = 0.0;
            return;
        }
        const Row r = locate(t);
        for (size_t i = 0; i < n; ++i) {
            if (r.clamped)
                copy_sample(at(r.lo, i), out + 6 * i);
            else
                r.weights.apply(at(r.lo, i), at(r.lo + 1, i), out + 6 * i);
        }
    }

    size_t count() const override { return ids_.size(); }
    const double* mus() const override { return mus_.data(); }
    bool ok() const override { return ok_; }
    long sun_index() const override { return sun_index_; }
    long index_of(long id) const override {
        for (size_t i = 0; i < ids_.size(); ++i)
            if (ids_[i] == id)
                return long(i);
        return -1;
    }

private:
    static constexpr double kBlockDays = 365.25;
    static constexpr int kBlockSamples = 128;

    void fail(std::string msg) {
        if (ok_) {
            ok_ = false;
            error_ = std::move(msg);
        }
    }

    void build(double t) {
        built_ = true;
        for (int id : kPerturberIds) {
            double st[6];
            if (source_->barycentric(id, t, st).ok()) {
                if (id == body::kSun)
                    sun_index_ = long(ids_.size());
                ids_.push_back(id);
                sources_.push_back(source_);
                mus_.push_back(gm_or_builtin(source_, id));
            }
        }
        if (asteroid_source_ && sun_index_ >= 0) {
            for (int id : asteroid_ids_) {
                double st[6];
                const double mu = gm_or_builtin(asteroid_source_, id);
                if (mu > 0.0 && asteroid_source_->barycentric(id, t, st).ok()) {
                    ids_.push_back(id);
                    sources_.push_back(asteroid_source_);
                    mus_.push_back(mu);
                }
            }
        }
        if (ids_.empty()) {
            fail("the ephemeris carries none of the perturbing masses at JD " + std::to_string(t) +
                 " (outside its coverage?)");
            return;
        }
        lo_ = hi_ = t; // no samples yet; extend() seeds both directions
        extend(kBlockDays);
        extend(-kBlockDays);
    }

    // Grows coverage by one block in the given direction (positive =
    // forward). The block boundary sample is shared with the existing rows
    // (or, on the very first block, is the probe epoch itself).
    void extend(double days) {
        const double from = days > 0.0 ? hi_ : lo_;
        const double to = from + days;
        const int n = kBlockSamples;
        const double step = days / double(n);
        const size_t nb = ids_.size();
        // block[i * nb + b]: sample i of the block (i = 0 is the boundary).
        std::vector<TrajSample> block(size_t(n + 1) * nb);
        for (size_t b = 0; b < nb; ++b) {
            for (int i = 0; i <= n; ++i) {
                const double tt = from + step * double(i);
                double st[6];
                auto r = sources_[b]->barycentric(ids_[b], tt, st);
                if (!r) {
                    fail(r.error().message);
                    return;
                }
                TrajSample& p = block[size_t(i) * nb + b];
                p.t = tt;
                p.px = st[0] / kAuKm;
                p.py = st[1] / kAuKm;
                p.pz = st[2] / kAuKm;
                p.vx = st[3] / kAuKm;
                p.vy = st[4] / kAuKm;
                p.vz = st[5] / kAuKm;
            }
        }
        const int first = grid_.empty() ? 0 : 1; // skip the shared boundary
        if (days > 0.0) {
            for (int i = first; i <= n; ++i)
                grid_.insert(grid_.end(), block.begin() + i * nb, block.begin() + (i + 1) * nb);
            hi_ = to;
        } else {
            // Backward samples run from the boundary down to `to`; the new
            // rows go in front, ascending.
            std::vector<TrajSample> rows_in;
            rows_in.reserve(size_t(n + 1) * nb);
            for (int i = n; i >= first; --i)
                rows_in.insert(rows_in.end(), block.begin() + i * nb, block.begin() + (i + 1) * nb);
            grid_.insert(grid_.begin(), rows_in.begin(), rows_in.end());
            lo_ = to;
        }
    }

    // Cubic Hermite weights for one epoch within [ta, tb].
    struct Hermite {
        double dt = 0, h00 = 0, h10 = 0, h01 = 0, h11 = 0, d00 = 0, d10 = 0, d01 = 0, d11 = 0;
        Hermite() = default;
        Hermite(double ta, double tb, double t) {
            dt = tb - ta;
            const double u = (t - ta) / dt;
            const double u2 = u * u, u3 = u2 * u;
            h00 = 2 * u3 - 3 * u2 + 1;
            h10 = u3 - 2 * u2 + u;
            h01 = -2 * u3 + 3 * u2;
            h11 = u3 - u2;
            d00 = (6 * u2 - 6 * u) / dt;
            d10 = 3 * u2 - 4 * u + 1;
            d01 = (-6 * u2 + 6 * u) / dt;
            d11 = 3 * u2 - 2 * u;
        }
        void apply(const TrajSample& a, const TrajSample& b, double out[6]) const {
            out[0] = h00 * a.px + h10 * dt * a.vx + h01 * b.px + h11 * dt * b.vx;
            out[1] = h00 * a.py + h10 * dt * a.vy + h01 * b.py + h11 * dt * b.vy;
            out[2] = h00 * a.pz + h10 * dt * a.vz + h01 * b.pz + h11 * dt * b.vz;
            out[3] = d00 * a.px + d10 * a.vx + d01 * b.px + d11 * b.vx;
            out[4] = d00 * a.py + d10 * a.vy + d01 * b.py + d11 * b.vy;
            out[5] = d00 * a.pz + d10 * a.vz + d01 * b.pz + d11 * b.vz;
        }
    };

    size_t rows() const { return ids_.empty() ? 0 : grid_.size() / ids_.size(); }
    const TrajSample& at(size_t row, size_t b) const { return grid_[row * ids_.size() + b]; }
    double row_time(size_t row) const { return grid_[row * ids_.size()].t; }

    // Where t falls: a clamped row outside the coverage (the first or last
    // sample, as before), else the interval [lo, lo + 1] and its weights.
    // Rows are uniformly spaced, so the index is computed and then nudged
    // past any rounding at the block seams.
    struct Row {
        size_t lo = 0;
        bool clamped = false;
        Hermite weights;
    };
    Row locate(double t) const {
        const size_t nr = rows();
        Row r;
        if (nr < 2 || t <= row_time(0)) {
            r.clamped = true;
            return r;
        }
        if (t >= row_time(nr - 1)) {
            r.lo = nr - 1;
            r.clamped = true;
            return r;
        }
        const double h = kBlockDays / double(kBlockSamples);
        const double k = std::floor((t - row_time(0)) / h);
        size_t lo = k <= 0.0 ? 0 : std::min(size_t(k), nr - 2);
        while (lo > 0 && row_time(lo) > t)
            --lo;
        while (lo + 2 < nr && row_time(lo + 1) <= t)
            ++lo;
        r.lo = lo;
        r.weights = Hermite(row_time(lo), row_time(lo + 1), t);
        return r;
    }

    static void copy_sample(const TrajSample& p, double out[6]) {
        out[0] = p.px;
        out[1] = p.py;
        out[2] = p.pz;
        out[3] = p.vx;
        out[4] = p.vy;
        out[5] = p.vz;
    }

    Source* source_ = nullptr;
    bool ok_ = true;
    long sun_index_ = -1;
    bool built_ = false;
    std::string error_;
    double lo_ = 0.0, hi_ = 0.0;
    std::vector<int> ids_;         // NAIF ids of the masses, table order
    std::vector<Source*> sources_; // where each mass's states come from
    Source* asteroid_source_ = nullptr;
    std::vector<int> asteroid_ids_;
    std::vector<TrajSample> grid_; // rows of ids_.size() samples, ascending epochs
    std::vector<double> mus_;

public:
    const std::string& error() const { return error_; }
};

// ---------------------------------------------------------------------------
// Small-body uncertainty: element sigmas -> position covariance.

// The record's 1-sigma element uncertainties, propagated to a 3x3
// barycentric position covariance by central finite differences of the
// integrated trajectory: element k stepped by h_k, both perturbed states
// carried in their own windowed memos so repeated queries amortize the
// extra integrations. Element uncertainties are uncorrelated (the catalog
// stores one sigma per element), so the covariance is the sum of rank-one
// terms sigma_k^2 c_k c_k^T with c_k = dr/d(element_k).
// JPL cometary elements {e, q [AU], tp [JD TDB], node, peri, i [rad]} at TDB
// JD t -> osculating Elements {a, e, i, node, argp, M}: a = q / (1 - e)
// (negative when hyperbolic), M = n (t - tp) with n = sqrt(mu / |a|^3).
std::optional<Elements> cometary_to_elements(double mu, const double c[6], double t) {
    const double e = c[0], q = c[1];
    if (!(q > 0.0) || !(e >= 0.0) || e == 1.0 || !std::isfinite(c[2]))
        return std::nullopt;
    const double a = q / (1.0 - e);
    const double n = std::sqrt(mu / std::fabs(a * a * a));
    double m = n * (t - c[2]);
    if (e < 1.0)
        m = std::remainder(m, 2.0 * 3.14159265358979323846);
    return Elements{a, e, c[5], c[3], c[4], m};
}

// Eigen-decomposition of a real symmetric n x n matrix (n <= 6) by cyclic
// Jacobi rotations: a is destroyed; values w[k] with unit eigenvectors in
// the columns of v (v[i][k]).
void symmetric_eigen(double a[6][6], int n, double w[6], double v[6][6]) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            v[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                off += a[i][j] * a[i][j];
        if (off < 1e-30)
            break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                if (a[p][q] == 0.0)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i)
        w[i] = a[i][i];
}

class SigmaTracks {
public:
    struct Column {
        double sigma; // 1-sigma along the column's direction (unit scale)
        double h;     // finite-difference step taken along it
        std::unique_ptr<WindowMemo<BarycentricForce>> plus, minus;
    };

    SigmaTracks(std::unique_ptr<BarycentricForce> force, std::vector<Column> columns)
        : force_(std::move(force)), columns_(std::move(columns)) {}

    bool empty() const { return columns_.empty(); }

    // Symmetric 3x3 position covariance at TDB JD t, packed xx, xy, xz,
    // yy, yz, zz, in AU^2. False when any perturbed track failed to
    // reach t (non-finite state, or coverage that never got there).
    bool covariance_at(double t, double cov[6]) const {
        for (int i = 0; i < 6; ++i)
            cov[i] = 0.0;
        for (const Column& c : columns_) {
            const State sp = c.plus->at(t);
            const State sm = c.minus->at(t);
            if (!std::isfinite(sp.pos.x) || !std::isfinite(sm.pos.x))
                return false;
            if (t < c.plus->coverage_lo() || t > c.plus->coverage_hi() ||
                t < c.minus->coverage_lo() || t > c.minus->coverage_hi())
                return false;
            const double d[3] = {(sp.pos.x - sm.pos.x) / (2.0 * c.h),
                                 (sp.pos.y - sm.pos.y) / (2.0 * c.h),
                                 (sp.pos.z - sm.pos.z) / (2.0 * c.h)};
            const double w = c.sigma * c.sigma;
            cov[0] += w * d[0] * d[0];
            cov[1] += w * d[0] * d[1];
            cov[2] += w * d[0] * d[2];
            cov[3] += w * d[1] * d[1];
            cov[4] += w * d[1] * d[2];
            cov[5] += w * d[2] * d[2];
        }
        return true;
    }

private:
    std::unique_ptr<BarycentricForce> force_; // the columns' memos integrate with it
    std::vector<Column> columns_;
};

} // namespace

// Where a body's state came from, for provenance and uncertainty.
enum Origin { kFromEphemeris = 0, kFromCatalog = 1 };

// One integrated catalog body: its own force model (which leaves the body
// itself out when it is one of the perturbing masses) and trajectory memo.
struct SmallBody {
    std::unique_ptr<BarycentricForce> force;
    std::unique_ptr<WindowMemo<BarycentricForce>> memo;
};

struct Engine::Impl {
    std::unique_ptr<Source> source;

    // Named hypothetical bodies: every definition ever added (a deque, so the
    // set names answers point at stay put), the current one per token, and
    // the order tokens were first defined in.
    std::deque<hypotheticals::Body> hypothetical_bodies;
    std::unordered_map<std::string, const hypotheticals::Body*> hypothetical_by_token;
    std::vector<std::string> hypothetical_order;

    void define_hypotheticals(std::vector<hypotheticals::Body> bodies) {
        for (hypotheticals::Body& b : bodies) {
            const hypotheticals::Body& kept = hypothetical_bodies.emplace_back(std::move(b));
            auto [it, fresh] = hypothetical_by_token.try_emplace(kept.token, &kept);
            if (fresh)
                hypothetical_order.push_back(kept.token);
            else
                it->second = &kept;
        }
    }

    // The element frame's rotation for the fixed equinoxes, kept apart from
    // the three-slot date cache, which a light-time solve and its rate
    // stencil already fill.
    double element_frame_jd = NAN;
    Precession element_frame_model = Precession::IAU2006;
    double element_frame[9] = {};
    std::unique_ptr<AsteroidSource> asteroids; // optional asteroid perturber kernel
    const time::DeltaTModel* delta_t = nullptr;
    time::ObservedDeltaT default_delta_t;
    double bias[9];
    double eps_j2000 = 0.0;
    EpochFrames cache[3];
    // Catalog stars that anchor a zodiac, by HR number (star_by_hr).
    std::unordered_map<int, size_t> star_by_hr_;
    // A binary's partner component, by HIP number (star_by_hip).
    std::unordered_map<int, size_t> star_by_hip_;
    int cache_next = 0;

    // Small-body overlay: EPM1 catalogs, newest wins. Positions are
    // integrated on demand from the catalog's osculating elements with
    // the barycentric force model and memoized per body.
    std::vector<std::unique_ptr<catalog::Reader>> catalogs;
    PerturberSet perturbers;
    std::unordered_map<uint64_t, SmallBody> small_bodies;
    // Decoding a catalog record walks its chunk from the start, so a body
    // asked for repeatedly — which is every body in a chart — would pay that
    // walk on every single position. The bodies in play are few; the whole
    // cache is dropped rather than evicted once it outgrows any working set a
    // client plausibly has.
    std::unordered_map<uint64_t, catalog::Record> record_cache;
    static constexpr size_t kRecordCacheMax = 4096;
    std::unordered_map<uint64_t, std::unique_ptr<SigmaTracks>> sigma_tracks;
    std::string overlay_source_; // provenance for catalog bodies
    std::string perturber_note_; // appended when asteroid perturbers are loaded

    // Per catalog (parallel to `catalogs`), built on the first lookup():
    // sorted (FNV-1a hash of the lowercased designation or name, SPK-ID)
    // pairs. A hash hit is confirmed against the record's own names, so
    // collisions cannot answer a wrong body. ~16 bytes per name instead of a
    // string map (1.57M bodies: ~50 MB and ~0.5 s, was ~250 MB and ~1.4 s).
    struct NameIndex {
        bool built = false;
        std::vector<std::pair<uint64_t, uint64_t>> entries;
    };
    std::vector<NameIndex> name_indexes;

    static std::string lowercase(std::string_view s) {
        std::string key(s);
        for (char& c : key)
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
        return key;
    }

    static uint64_t name_hash(std::string_view lower) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : lower) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    Result<void> ensure_name_index(size_t i) {
        NameIndex& idx = name_indexes[i];
        if (idx.built)
            return {};
        std::vector<std::pair<uint64_t, uint64_t>> entries;
        entries.reserve(size_t(catalogs[i]->record_count()) + 1024);
        auto fe = catalogs[i]->for_each([&](const catalog::Record& rec, const catalog::Names& n) {
            if (!n.pdes.empty())
                entries.emplace_back(name_hash(lowercase(n.pdes)), rec.spkid);
            if (!n.name.empty())
                entries.emplace_back(name_hash(lowercase(n.name)), rec.spkid);
        });
        if (!fe)
            return fe.error();
        std::sort(entries.begin(), entries.end());
        idx.entries = std::move(entries);
        idx.built = true;
        return {};
    }

    // Provenance of catalog answers: the ephemeris, the catalog stack (newest
    // first) and, when loaded, the asteroid perturbers.
    void refresh_overlay_source() {
        if (catalogs.empty()) {
            overlay_source_.clear();
            return;
        }
        std::string counts;
        for (size_t i = catalogs.size(); i-- > 0;) {
            if (!counts.empty())
                counts += ", ";
            counts += std::to_string(catalogs[i]->record_count());
        }
        overlay_source_ =
            source->description + " + EPM1 catalog(s) [" + counts + " bodies]" + perturber_note_;
    }

    double delta_t_seconds(double jd_tt) const {
        return (delta_t ? delta_t : &default_delta_t)->delta_t_seconds(jd_tt);
    }

    const EpochFrames& frames_at(double jd_tt, bool need_nutation, Precession precession) {
        const auto model = frames::PrecessionModel(int(precession));
        for (EpochFrames& f : cache) {
            if (f.jd_tt == jd_tt && f.model == model) {
                if (need_nutation && !f.have_nutation)
                    add_nutation(f);
                return f;
            }
        }
        EpochFrames& f = cache[cache_next];
        cache_next = (cache_next + 1) % 3;
        f.jd_tt = jd_tt;
        f.model = model;
        f.have_nutation = false;
        double p[9];
        frames::mean_equator_of_date_matrix(jd_tt, model, p);
        matmul(p, bias, f.pb);
        f.eps_mean = frames::mean_obliquity(jd_tt, model);
        if (need_nutation)
            add_nutation(f);
        return f;
    }

    // Nutation interpolated from half-day nodes (frames::NutationInterpolator,
    // 0.004 uas): the value depends only on the epoch, and the node cache is
    // shared by every body and every rate stencil this engine evaluates.
    frames::NutationInterpolator nutation;

    void add_nutation(EpochFrames& f) {
        nutation.at(f.jd_tt, f.dpsi, f.deps);
        // N = R1(-(eps + deps)) R3(-dpsi) R1(eps), docs/FRAMES.md.
        double a[9], b[9], c[9], n[9];
        rot1(-(f.eps_mean + f.deps), a);
        rot3(-f.dpsi, b);
        rot1(f.eps_mean, c);
        matmul(a, b, n);
        matmul(n, c, n);
        matmul(n, f.pb, f.npb);
        f.have_nutation = true;
    }

    // The last few observer and Sun states, keyed exactly: a request of many
    // bodies at one instant (and its rate stencil) reads the ephemeris for
    // them once. Three entries cover t - h, t, t + h.
    struct ObserverMemo {
        bool valid = false;
        double jd_tt = 0.0;
        Center center = Center::Geocentric;
        Precession precession = Precession::IAU2006;
        frames::GeoSite site{};
        int center_body = 0;
        double delta_t = 0.0; // topocentric: the UT1 the site was rotated with
        double state[6];
    };
    ObserverMemo observer_memo[3];
    int observer_memo_next = 0;
    struct SunMemo {
        double jd_tdb = NAN;
        double state[6];
    };
    SunMemo sun_memo[3];
    int sun_memo_next = 0;

    void forget_observers() {
        for (ObserverMemo& m : observer_memo)
            m.valid = false;
    }

    Result<void> sun_at(double jd_tdb, double out[6]) {
        for (const SunMemo& m : sun_memo) {
            if (m.jd_tdb == jd_tdb) {
                std::memcpy(out, m.state, sizeof m.state);
                return {};
            }
        }
        auto r = source->barycentric(body::kSun, jd_tdb, out);
        if (!r)
            return r;
        SunMemo& m = sun_memo[sun_memo_next];
        sun_memo_next = (sun_memo_next + 1) % 3;
        m.jd_tdb = jd_tdb;
        std::memcpy(m.state, out, sizeof m.state);
        return {};
    }

    Result<void> observer(const CalcOptions& o, double jd_tt, double jd_tdb, double out[6]) {
        const bool topo = o.center == Center::Topocentric;
        // A Delta T model (the C interface's callback, say) may change its
        // answer between calls, so a topocentric entry is keyed on it too.
        const double dt = topo ? delta_t_seconds(jd_tt) : 0.0;
        for (const ObserverMemo& m : observer_memo) {
            if (m.valid && m.jd_tt == jd_tt && m.center == o.center &&
                (o.center != Center::Body || m.center_body == o.center_body) &&
                (!topo || (m.precession == o.precession && m.site.lon_rad == o.site.lon_rad &&
                           m.site.lat_rad == o.site.lat_rad && m.site.height_m == o.site.height_m &&
                           m.delta_t == dt))) {
                std::memcpy(out, m.state, sizeof m.state);
                return {};
            }
        }
        auto r = observer_uncached(o, jd_tt, jd_tdb, out);
        if (!r)
            return r;
        ObserverMemo& m = observer_memo[observer_memo_next];
        observer_memo_next = (observer_memo_next + 1) % 3;
        m.valid = true;
        m.jd_tt = jd_tt;
        m.center = o.center;
        m.precession = o.precession;
        m.site = o.site;
        m.center_body = o.center_body;
        m.delta_t = dt;
        std::memcpy(m.state, out, sizeof m.state);
        return {};
    }

    // Observer barycentric state (km, km/day) at the given epoch.
    Result<void> observer_uncached(const CalcOptions& o, double jd_tt, double jd_tdb,
                                   double out[6]) {
        switch (o.center) {
        case Center::Barycentric:
            for (int i = 0; i < 6; ++i)
                out[i] = 0.0;
            return {};
        case Center::Heliocentric:
            return sun_at(jd_tdb, out);
        case Center::Geocentric:
            return source->barycentric(body::kEarth, jd_tdb, out);
        case Center::Body:
            return body_barycentric(o.center_body, jd_tdb, out, nullptr);
        case Center::Topocentric: {
            // A site the geodetic formulas can place: finite, and above the
            // Earth's centre along its vertical (the WGS84 polar radius). The
            // protocol bounds a site's latitude and longitude but not its
            // height, so this is the only check a height gets.
            if (!std::isfinite(o.site.lon_rad) || !std::isfinite(o.site.lat_rad) ||
                !std::isfinite(o.site.height_m) || o.site.height_m <= -6356752.0)
                return make_error(ErrorCode::ArgumentError,
                                  "a topocentric site must be finite and above the Earth's centre");
            auto r = source->barycentric(body::kEarth, jd_tdb, out);
            if (!r)
                return r;
            const EpochFrames& f = frames_at(jd_tt, true, o.precession);
            const double jd_ut1 = jd_tt - delta_t_seconds(jd_tt) / 86400.0;
            const double gast = frames::gast_rad(jd_ut1, jd_tt, f.dpsi, f.eps_mean);
            double site[3];
            frames::observer_geocentric(o.site, gast, site);
            // Site velocity in the true-of-date frame: omega x r about z
            // (per TT day; the UT1/TT rate difference is ~1e-8).
            const double vsite[3] = {-kEarthRotationRadPerDay * site[1],
                                     kEarthRotationRadPerDay * site[0], 0.0};
            double p[3], v[3];
            apply_transpose(f.npb, site, p);
            apply_transpose(f.npb, vsite, v);
            for (int i = 0; i < 3; ++i) {
                out[i] += p[i];
                out[3 + i] += v[i];
            }
            return {};
        }
        }
        return make_error(ErrorCode::ArgumentError, "unknown center");
    }

    // Body state dispatch: the planetary ephemeris first; a body it does
    // not know is looked up in the catalog overlay (small bodies, by
    // SPK-ID). out is barycentric km, km/day either way.
    Result<void> body_barycentric(int id, double jd_tdb, double out[6], int* origin) {
        auto r = source->barycentric(id, jd_tdb, out);
        if (r.ok()) {
            if (origin)
                *origin = kFromEphemeris;
            return r;
        }
        if (r.error().code != ErrorCode::NotFound)
            return r;
        if (origin)
            *origin = kFromCatalog;
        return small_body_state(id, jd_tdb, out);
    }

    // A force model for integrating body `id`: every mass but the body.
    std::unique_ptr<BarycentricForce> force_for(int id) {
        auto f = std::make_unique<BarycentricForce>();
        f->perturbers = &perturbers;
        f->exclude_id = id;
        return f;
    }

    // Newest-catalog-wins record lookup for a body the planetary
    // ephemeris does not carry. Engaged only after it returned NotFound.
    // The pair's second is the catalog the record came from (its name pool
    // goes with it).
    Result<std::optional<std::pair<catalog::Record, size_t>>> find_record_at(int id) {
        if (auto hit = record_cache.find(uint64_t(id)); hit != record_cache.end()) {
            // Which catalog carried it is not cached; names() is not a hot
            // path, so find it again. Newest wins, as everywhere here.
            for (size_t i = catalogs.size(); i-- > 0;) {
                if (auto rr = catalogs[i]->lookup(uint64_t(id)); rr.ok())
                    return std::optional<std::pair<catalog::Record, size_t>>({rr.value(), i});
            }
            return std::optional<std::pair<catalog::Record, size_t>>({hit->second, 0});
        }
        for (size_t i = catalogs.size(); i-- > 0;) {
            auto rr = catalogs[i]->lookup(uint64_t(id));
            if (rr.ok()) {
                if (record_cache.size() >= kRecordCacheMax)
                    record_cache.clear();
                record_cache[uint64_t(id)] = rr.value();
                return std::optional<std::pair<catalog::Record, size_t>>({rr.value(), i});
            }
            if (rr.error().code != ErrorCode::NotFound)
                return rr.error();
        }
        return std::optional<std::pair<catalog::Record, size_t>>();
    }

    Result<std::optional<catalog::Record>> find_record(int id) {
        auto r = find_record_at(id);
        if (!r.ok()) {
            return r.error();
        }
        if (!r.value()) {
            return std::optional<catalog::Record>();
        }
        return std::optional<catalog::Record>(r.value()->first);
    }

    Result<void> small_body_state(int id, double jd_tdb, double out[6]) {
        auto recr = find_record(id);
        if (!recr.ok())
            return recr.error();
        if (!recr.value())
            return make_error(ErrorCode::NotFound, "body " + std::to_string(id) +
                                                       " is in neither " + source->description +
                                                       " nor the loaded catalog(s)");
        const catalog::Record& rec = *recr.value();

        auto& slot = small_bodies[uint64_t(id)];
        if (!slot.memo) {
            auto built = build_small_body(rec);
            if (!built)
                return built.error();
            slot = std::move(built).value();
        }
        const State s = slot.memo->at(jd_tdb);
        if (!perturbers.ok())
            return make_error(ErrorCode::ArgumentError, perturbers.error());
        if (!std::isfinite(s.pos.x) || !std::isfinite(s.vel.x) ||
            jd_tdb < slot.memo->coverage_lo() || jd_tdb > slot.memo->coverage_hi())
            return make_error(ErrorCode::ArgumentError,
                              "integration failed for body " + std::to_string(id));
        const double p[3] = {s.pos.x, s.pos.y, s.pos.z};
        const double v[3] = {s.vel.x, s.vel.y, s.vel.z};
        for (int i = 0; i < 3; ++i) {
            out[i] = p[i] * kAuKm;
            out[3 + i] = v[i] * kAuKm;
        }
        return {};
    }

    // Elements (heliocentric, ecliptic and equinox of J2000, at a TDB
    // epoch) -> ICRF barycentric seed state (AU, AU/day): elements ->
    // heliocentric Cartesian in JPL's J2000 ecliptic, rotated to ICRF, then
    // translated by the Sun's barycentric state at the epoch. JPL's
    // ecliptic (SBDB elements, Horizons, SPICE ECLIPJ2000) is the ICRF
    // rotated about x by the IAU 1976 obliquity 84381.448" with no frame
    // bias -- not the IAU 2006 mean ecliptic our J2000 output uses; the
    // difference (0.042" plus the 23 mas bias) is ~50 km at 2.7 AU.
    Result<State> seed_from_elements(const Elements& el, double epoch_jtdb) {
        const double mu_sun = gm_or_builtin(source.get(), body::kSun);
        auto st = elements_to_state(mu_sun, el);
        if (!st)
            return st.error();
        const State helio = st.value();

        double m[9];
        rot1(kJplEclipticObliquityArcsec / 206264.80624709636, m);
        double r[3] = {helio.pos.x, helio.pos.y, helio.pos.z};
        double rv[3] = {helio.vel.x, helio.vel.y, helio.vel.z};
        double p[3], v[3];
        apply_transpose(m, r, p);
        apply_transpose(m, rv, v);

        double sun[6];
        auto rs = source->barycentric(body::kSun, epoch_jtdb, sun);
        if (!rs)
            return rs.error();

        State seed;
        seed.pos = Vec3(p[0] + sun[0] / kAuKm, p[1] + sun[1] / kAuKm, p[2] + sun[2] / kAuKm);
        seed.vel = Vec3(v[0] + sun[3] / kAuKm, v[1] + sun[4] / kAuKm, v[2] + sun[5] / kAuKm);
        return seed;
    }

    // The memo that integrates one catalog body, seeded at the record's
    // epoch (TDB) from the record's osculating elements.
    Result<SmallBody> build_small_body(const catalog::Record& rec) {
        const Elements el{rec.a_au,     rec.e,        rec.inc_rad,
                          rec.node_rad, rec.argp_rad, rec.mean_anom_rad};
        auto seed = seed_from_elements(el, rec.epoch_jtdb);
        if (!seed)
            return seed.error();
        SmallBody b;
        b.force = force_for(int(rec.spkid));
        b.memo = std::make_unique<WindowMemo<BarycentricForce>>(b.force.get(), IntegrateOptions{});
        b.memo->set_seed(seed.value(), rec.epoch_jtdb);
        return b;
    }

    // The perturbed trajectories behind sigma_arcsec, built lazily on the
    // first query that needs them, from the record's full covariance C (at
    // its own epoch, in cometary elements). C is scaled to a correlation
    // matrix (element variances span ~1e-23 to ~1e-2), decomposed into
    // principal axes, and each axis with a positive eigenvalue becomes one
    // column: seeds at the nominal cometary elements +- s * v_k, where v_k
    // is the axis scaled to one sigma, integrated in their own memos. The
    // columns' outer products sum to J C J^T (J = d position / d elements)
    // exactly in the linear regime. The step s is 1 (the one-sigma point),
    // raised when that moves the seed by less than kMinSeedStepAu (double
    // and integrator noise) and lowered to keep e on its side of 0 and 1
    // and q positive.
    Result<std::unique_ptr<SigmaTracks>> build_sigma_tracks(const catalog::Record& rec) {
        static constexpr double kMinSeedStepAu = 1e-7;
        std::vector<SigmaTracks::Column> columns;
        auto track_force = force_for(int(rec.spkid));
        const double mu = gm_or_builtin(source.get(), body::kSun);
        const double t0 = rec.cov_epoch_jtdb;
        const double* x0 = rec.cov_elements;
        auto nominal = cometary_to_elements(mu, x0, t0);
        if (!nominal)
            return make_error(ErrorCode::ArgumentError, "invalid covariance elements");
        auto nominal_state = elements_to_state(mu, *nominal);
        if (!nominal_state)
            return nominal_state.error();

        double d[6], r[6][6], w[6], axes[6][6];
        for (int i = 0; i < 6; ++i)
            d[i] = std::sqrt(std::max(0.0, rec.covariance[catalog::packed_index(i, i)]));
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j)
                r[i][j] = d[i] > 0.0 && d[j] > 0.0
                              ? rec.covariance[catalog::packed_index(i, j)] / (d[i] * d[j])
                              : 0.0;
        symmetric_eigen(r, 6, w, axes);
        const double w_max = std::max({w[0], w[1], w[2], w[3], w[4], w[5], 0.0});

        for (int k = 0; k < 6; ++k) {
            if (!(w[k] > 1e-14 * w_max))
                continue;
            double v[6];
            for (int i = 0; i < 6; ++i)
                v[i] = d[i] * axes[i][k] * std::sqrt(w[k]);

            // Seed displacement of the one-sigma point (two-body, at t0).
            double s = 1.0;
            {
                double x[6];
                for (int i = 0; i < 6; ++i)
                    x[i] = x0[i] + v[i];
                auto el = cometary_to_elements(mu, x, t0);
                auto st = el ? elements_to_state(mu, *el) : Result<State>(State{});
                if (el && st.ok()) {
                    const Vec3 dr = st.value().pos - nominal_state.value().pos;
                    const double moved = std::sqrt(dr.x * dr.x + dr.y * dr.y + dr.z * dr.z);
                    if (moved > 0.0 && moved < kMinSeedStepAu)
                        s = kMinSeedStepAu / moved;
                }
            }
            const double e0 = x0[0], q0 = x0[1];
            if (v[0] != 0.0)
                s = std::min(s, 0.5 * (e0 < 1.0 ? std::min(e0, 1.0 - e0) : e0 - 1.0) /
                                    std::fabs(v[0]));
            if (v[1] != 0.0)
                s = std::min(s, 0.5 * q0 / std::fabs(v[1]));
            if (!(s > 0.0))
                continue; // degenerate (e.g. an exact circle with variance in e)

            SigmaTracks::Column col;
            col.sigma = 1.0;
            col.h = s;
            for (int sign : {+1, -1}) {
                double x[6];
                for (int i = 0; i < 6; ++i)
                    x[i] = x0[i] + sign * s * v[i];
                auto el = cometary_to_elements(mu, x, t0);
                if (!el)
                    return make_error(ErrorCode::ArgumentError,
                                      "covariance step left the element domain");
                auto seed = seed_from_elements(*el, t0);
                if (!seed)
                    return seed.error();
                auto memo = std::make_unique<WindowMemo<BarycentricForce>>(track_force.get(),
                                                                           IntegrateOptions{});
                memo->set_seed(seed.value(), t0);
                (sign > 0 ? col.plus : col.minus) = std::move(memo);
            }
            columns.push_back(std::move(col));
        }
        return std::make_unique<SigmaTracks>(std::move(track_force), std::move(columns));
    }

    // 1-sigma sky-plane uncertainty of a catalog body (arcsec), or
    // nullopt when the record carries no covariance or the perturbed
    // tracks failed to reach the epoch; exactly zero for an all-zero
    // covariance. The barycentric position covariance at the retarded epoch is
    // projected on the plane perpendicular to the observer->body line;
    // the square root of the larger eigenvalue of the projected 2x2
    // covariance, divided by the observer->body distance, is the
    // 1-sigma uncertainty of the body's direction. Eigenvalues are
    // invariant under the (orthogonal) frame rotations, so the output
    // frame and the light-optics corrections never enter; the observer is
    // treated as exact.
    std::optional<double> sigma_arcsec(int id, const CalcOptions& o, double jd_tt, double tau) {
        auto recr = find_record(id);
        if (!recr.ok() || !recr.value())
            return std::nullopt;
        const catalog::Record rec = *recr.value();
        if (!rec.has(catalog::RecordFlags::kCovariance))
            return std::nullopt; // element sigmas alone are uncorrelated: no calibrated answer

        auto& tracks = sigma_tracks[uint64_t(id)];
        if (!tracks) {
            auto built = build_sigma_tracks(rec);
            if (!built.ok())
                return std::nullopt;
            tracks = std::move(built).value();
        }
        if (tracks->empty())
            return 0.0; // covariance present but zero

        const double jd_tdb = time::tdb_from_tt(jd_tt);
        const double t_ret = jd_tdb - tau;
        double cov[6];
        if (!tracks->covariance_at(t_ret, cov) || !perturbers.ok())
            return std::nullopt;

        // Line of sight and distance, ICRF (the covariance's frame).
        double body[6], obs[6];
        if (!body_barycentric(id, t_ret, body, nullptr).ok())
            return std::nullopt;
        if (!observer(o, jd_tt, jd_tdb, obs).ok())
            return std::nullopt;
        double u[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i < 3; ++i)
            u[i] = (body[i] - obs[i]) / kAuKm;
        const double d = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(d > 0.0) || !std::isfinite(d))
            return std::nullopt;
        for (double& x : u)
            x /= d;

        // Tangent-plane basis: the coordinate axis least aligned with the
        // line of sight, made orthogonal to it.
        const double au[3] = {std::fabs(u[0]), std::fabs(u[1]), std::fabs(u[2])};
        double axis[3] = {0.0, 0.0, 0.0};
        axis[au[0] <= au[1] && au[0] <= au[2] ? 0 : (au[1] <= au[2] ? 1 : 2)] = 1.0;
        double e1[3] = {u[1] * axis[2] - u[2] * axis[1], u[2] * axis[0] - u[0] * axis[2],
                        u[0] * axis[1] - u[1] * axis[0]};
        const double n1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
        if (!(n1 > 0.0))
            return std::nullopt;
        for (double& x : e1)
            x /= n1;
        const double e2[3] = {u[1] * e1[2] - u[2] * e1[1], u[2] * e1[0] - u[0] * e1[2],
                              u[0] * e1[1] - u[1] * e1[0]};

        // Bilinear form of the packed covariance, then the larger
        // eigenvalue of the 2x2 projection.
        auto q = [&](const double a[3], const double b[3]) {
            return cov[0] * a[0] * b[0] + cov[3] * a[1] * b[1] + cov[5] * a[2] * b[2] +
                   cov[1] * (a[0] * b[1] + a[1] * b[0]) + cov[2] * (a[0] * b[2] + a[2] * b[0]) +
                   cov[4] * (a[1] * b[2] + a[2] * b[1]);
        };
        const double A = q(e1, e1), B = q(e1, e2), C = q(e2, e2);
        const double disc = std::sqrt(std::max(0.0, (A - C) * (A - C) + 4.0 * B * B));
        const double lambda = 0.5 * (A + C + disc);
        return std::sqrt(std::max(lambda, 0.0)) / d * kRad2Deg * 3600.0;
    }

    // The sidereal zodiac's longitude shift (degrees) for the output
    // frame at jd_tt: the true ayanamsha in the true ecliptic of date,
    // the mean ayanamsha in the mean frames, and for the fixed frames
    // (J2000, ICRF) the zero point's fixed longitude on the mean
    // ecliptic of J2000. Error for an unknown mode or a non-finite
    // user anchor.
    Result<double> sidereal_shift(const CalcOptions& o, double jd_tt) {
        if (o.sidereal_plane != SiderealPlane::EclipticOfDate) {
            // A fixed plane: the zero point does not move, and the value
            // reported is the anchor's A0 (protocol v4 3.5a); for a zodiac
            // defined at the instant, its mean ayanamsha then.
            auto a = sidereal_anchor(o, jd_tt);
            if (!a)
                return a.error();
            return a.value().mean0_deg;
        }
        const auto model = frames::PrecessionModel(int(o.precession));
        std::optional<frames::Ayanamsa> aya;
        if (const InstantZodiac* z = instant_zodiac(o.sidereal)) {
            // The J2000 and ICRF frames: the zero point's longitude on the
            // mean ecliptic of J2000, which is the mean ayanamsha there (3.5a:
            // "the constant A(J2000.0) mean").
            const bool fixed_frame = o.frame == Frame::J2000 || o.frame == Frame::ICRF;
            auto a = instant_ayanamsa(*z, fixed_frame ? kJ2000 : jd_tt, o.precession);
            if (!a)
                return a.error();
            if (fixed_frame)
                return a.value().mean_deg;
            aya = a.value();
        } else {
            // An anchored zodiac: frames::ayanamsa_anchored's arithmetic, with
            // the nutation in longitude from the epoch cache (interpolated,
            // 0.004 uas from the full series) rather than the full series on
            // every call, which cost ~60 us a position (three calls with rates).
            auto a = sidereal_anchor(o, jd_tt);
            if (!a)
                return a.error();
            const double mean = a.value().mean0_deg +
                                frames::precession_in_longitude_deg(jd_tt, model) -
                                frames::precession_in_longitude_deg(a.value().t0_jtdb, model);
            const double dpsi = o.frame == Frame::TrueOfDate
                                    ? frames_at(jd_tt, true, o.precession).dpsi * kRad2Deg
                                    : 0.0;
            aya = frames::Ayanamsa{mean, mean + dpsi};
        }
        switch (o.frame) {
        case Frame::TrueOfDate:
            return aya->true_deg;
        case Frame::MeanOfDate:
            return aya->mean_deg;
        case Frame::J2000:
        case Frame::ICRF:
            // The zodiac's zero point has a fixed longitude on the mean
            // ecliptic of J2000: the ayanamsha less the precession
            // accumulated since J2000.
            return aya->mean_deg - frames::precession_in_longitude_deg(jd_tt, model);
        }
        return make_error(ErrorCode::ArgumentError, "unknown frame");
    }

    // A catalog star's index by HIP number, remembered after the first search.
    Result<size_t> star_by_hip(int hip) {
        if (auto it = star_by_hip_.find(hip); it != star_by_hip_.end())
            return it->second;
        for (size_t i = 0; i < stars::count(); ++i) {
            if (stars::at(i).hip == hip) {
                star_by_hip_.emplace(hip, i);
                return i;
            }
        }
        return make_error(ErrorCode::NotFound, "no catalog star HIP " + std::to_string(hip));
    }

    // A catalog star's index by HR number, remembered after the first search.
    Result<size_t> star_by_hr(int hr) {
        if (auto it = star_by_hr_.find(hr); it != star_by_hr_.end())
            return it->second;
        for (size_t i = 0; i < stars::count(); ++i) {
            if (stars::at(i).hr == hr) {
                star_by_hr_.emplace(hr, i);
                return i;
            }
        }
        return make_error(ErrorCode::NotFound, "no catalog star HR " + std::to_string(hr));
    }

    // Sgr A* as a catalog object: SIMBAD's ICRS place, and the apparent motion
    // Reid & Brunthaler measured in galactic coordinates turned into proper
    // motion in RA and Dec about the IAU 1958 pole (theirs is measured in the
    // standard galactic system). No parallax: the motion is an angle a year.
    static stars::Object galactic_centre() {
        double u[3], p[3];
        unit_radec(kSgrARaDeg, kSgrADecDeg, u);
        unit_radec(kPoleIau1958RaDeg, kPoleIau1958DecDeg, p);
        // e_b: toward the pole across the line of sight; e_l = e_b x u, the
        // direction of increasing galactic longitude (x to the centre, z to
        // the pole, y = z x x).
        const double pu = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
        double eb[3] = {p[0] - pu * u[0], p[1] - pu * u[1], p[2] - pu * u[2]};
        const double nb = std::sqrt(eb[0] * eb[0] + eb[1] * eb[1] + eb[2] * eb[2]);
        for (double& x : eb)
            x /= nb;
        const double el[3] = {eb[1] * u[2] - eb[2] * u[1], eb[2] * u[0] - eb[0] * u[2],
                              eb[0] * u[1] - eb[1] * u[0]};
        double mu[3];
        for (int i = 0; i < 3; ++i)
            mu[i] = kSgrAMuLMasYr * el[i] + kSgrAMuBMasYr * eb[i];
        const double a = kSgrARaDeg / kRad2Deg, d = kSgrADecDeg / kRad2Deg;
        const double east[3] = {-std::sin(a), std::cos(a), 0.0};
        const double north[3] = {-std::sin(d) * std::cos(a), -std::sin(d) * std::sin(a),
                                 std::cos(d)};
        stars::Object gc;
        gc.ra_deg = kSgrARaDeg;
        gc.dec_deg = kSgrADecDeg;
        gc.epoch_jyear = 2000.0;
        gc.pm_ra_mas_yr = mu[0] * east[0] + mu[1] * east[1] + mu[2] * east[2];
        gc.pm_dec_mas_yr = mu[0] * north[0] + mu[1] * north[1] + mu[2] * north[2];
        return gc;
    }

    // The ayanamsha of a zodiac defined at the instant, true and mean
    // (degrees): the anchor's longitude on the true ecliptic and equinox of
    // date less the longitude the zodiac gives it; the mean value is that less
    // the nutation in longitude, as for the anchored modes.
    Result<frames::Ayanamsa> instant_ayanamsa(const InstantZodiac& z, double jd_tt,
                                              Precession precession) {
        const EpochFrames& f = frames_at(jd_tt, true, precession);
        // ICRF to the true ecliptic and equinox of date, as to_output builds it.
        double e[9], a[9], b[9];
        rot3(-f.dpsi, a);
        rot1(f.eps_mean, b);
        matmul(a, b, e);
        matmul(e, f.pb, e);
        double lon = 0.0; // radians
        if (z.anchor == ZodiacAnchor::GalacticNode) {
            // The node lies on both great circles, so it is perpendicular to
            // both poles: n = k x p in ecliptic coordinates, k the ecliptic
            // pole. Of n and -n, the one near 0° Capricorn (the documentation:
            // "at present ... near 0 Capricorn").
            double p[3], v[3];
            if (z.modern_pole)
                unit_radec(kPoleModernRaDeg, kPoleModernDecDeg, p);
            else
                unit_radec(kPoleIau1958RaDeg, kPoleIau1958DecDeg, p);
            apply(e, p, v);
            lon = std::atan2(v[0], -v[1]);
            if (lon < 0.0)
                lon += kTwoPi;
            if (lon < kTwoPi / 2.0)
                lon += kTwoPi / 2.0;
        } else {
            double pos[3];
            const double jd_tdb = time::tdb_from_tt(jd_tt);
            if (z.anchor == ZodiacAnchor::Star) {
                auto idx = star_by_hr(z.hr);
                if (!idx)
                    return idx.error();
                star_barycentric_km(stars::at(idx.value()), jd_tdb, pos);
            } else {
                star_barycentric_km(galactic_centre(), jd_tdb, pos);
            }
            if (z.anchor == ZodiacAnchor::GalacticCentrePolar) {
                // The ecliptic point on the anchor's hour circle, through the
                // MEAN pole of date: the same right ascension on the mean
                // equator, tan(lon) = tan(ra) / cos(eps) on the ecliptic with
                // the mean equinox, then the equinox slides by dpsi like every
                // other zodiac's. The true pole would carry its nutation into
                // the zero point itself (~0.6", an 18.6-year wobble), where
                // 3.5a's true ayanamsha is the mean one plus dpsi and nothing
                // else.
                double q[3];
                apply(f.pb, pos, q);
                const double ra = std::atan2(q[1], q[0]);
                lon = std::atan2(std::sin(ra), std::cos(f.eps_mean) * std::cos(ra)) + f.dpsi;
            } else {
                double v[3];
                apply(e, pos, v);
                lon = std::atan2(v[1], v[0]);
            }
        }
        double aya = std::fmod(lon * kRad2Deg - z.at_deg, 360.0);
        if (aya > 180.0)
            aya -= 360.0;
        else if (aya <= -180.0)
            aya += 360.0;
        return frames::Ayanamsa{aya - f.dpsi * kRad2Deg, aya};
    }

    // The zodiac's zero point: its anchor epoch t0 (TT) and MEAN ayanamsha
    // A0 there, for the user anchor or a published mode. A zodiac defined at
    // the instant has no t0: on the invariable plane its zero point is the
    // one of the instant asked (t0 = jd_tt), and the ecliptic of the anchor
    // epoch, which needs a t0 of the zodiac's own, is refused.
    Result<frames::AyanamsaAnchor> sidereal_anchor(const CalcOptions& o, double jd_tt) {
        if (const InstantZodiac* z = instant_zodiac(o.sidereal)) {
            if (o.sidereal_plane == SiderealPlane::EclipticOfAnchor)
                return make_error(ErrorCode::ArgumentError,
                                  "a zodiac defined at the instant has no anchor epoch, so no "
                                  "ecliptic of the anchor epoch");
            auto a = instant_ayanamsa(*z, jd_tt, o.precession);
            if (!a)
                return a.error();
            return frames::AyanamsaAnchor{jd_tt, a.value().mean_deg};
        }
        if (o.sidereal == SiderealMode::User) {
            if (!std::isfinite(o.sidereal_epoch_jtdb) || !std::isfinite(o.sidereal_ayanamsa_deg))
                return make_error(ErrorCode::ArgumentError, "sidereal User anchor is not finite");
            return frames::AyanamsaAnchor{o.sidereal_epoch_jtdb, o.sidereal_ayanamsa_deg};
        }
        auto a = frames::ayanamsa_anchor(int(o.sidereal));
        if (!a)
            return make_error(ErrorCode::ArgumentError, "unknown sidereal mode");
        return *a;
    }

    // ICRF to a fixed sidereal plane with the longitude zero at the zodiac's
    // zero point (SiderealPlane::EclipticOfAnchor or ::Invariable).
    Result<void> fixed_sidereal_matrix(const CalcOptions& o, double jd_tt, double m[9]) {
        auto a = sidereal_anchor(o, jd_tt);
        if (!a)
            return a.error();
        const double a0 = a.value().mean0_deg / kRad2Deg;
        // ICRF to the mean ecliptic and equinox of t0.
        const EpochFrames& f0 = frames_at(a.value().t0_jtdb, false, o.precession);
        double ecl0[9];
        rot1(f0.eps_mean, ecl0);
        matmul(ecl0, f0.pb, ecl0);
        if (o.sidereal_plane == SiderealPlane::EclipticOfAnchor) {
            // Longitude counted from A0 on that ecliptic: rot3(A0) moves
            // longitudes by -A0.
            double r[9];
            rot3(a0, r);
            matmul(r, ecl0, m);
            return {};
        }
        // The invariable plane: rows x, y, n with n the pole and x its
        // ascending node on the ICRF equator (any in-plane x would do: the
        // zero point below fixes the longitude origin).
        const double* n = frames::kInvariablePoleIcrf;
        double x[3] = {-n[1], n[0], 0.0}; // k x n
        const double xn = std::sqrt(x[0] * x[0] + x[1] * x[1]);
        x[0] /= xn;
        x[1] /= xn;
        const double y[3] = {n[1] * x[2] - n[2] * x[1], n[2] * x[0] - n[0] * x[2],
                             n[0] * x[1] - n[1] * x[0]};
        const double inv[9] = {x[0], x[1], x[2], y[0], y[1], y[2], n[0], n[1], n[2]};
        // The zero point: longitude A0 on the ecliptic of t0, carried into
        // ICRF, then projected onto the plane; its in-plane angle is theta0.
        const double z_ecl[3] = {std::cos(a0), std::sin(a0), 0.0};
        double z_icrf[3], z_inv[3];
        apply_transpose(ecl0, z_ecl, z_icrf);
        apply(inv, z_icrf, z_inv);
        const double theta0 = std::atan2(z_inv[1], z_inv[0]);
        double r[9];
        rot3(theta0, r);
        matmul(r, inv, m);
        return {};
    }

    // Body barycentric position at jd_tdb - tau. The subtraction is done
    // with its exact rounding error recovered (TwoSum) and applied through
    // the velocity: a JD double near the present only resolves ~40 us,
    // which would otherwise put ~1 m of noise into every retarded position.
    Result<void> retarded(int id, double jd_tdb, double tau, double out[6], int* origin) {
        const double s = jd_tdb - tau;
        const double bp = s - jd_tdb;
        const double err = (jd_tdb - (s - bp)) + (-tau - bp);
        auto r = body_barycentric(id, s, out, origin);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] += out[3 + i] * err;
        return {};
    }

    // Observer->body vector in the output frame (km), for one TT epoch.
    // `tau` on entry is a starting guess for the light time (0 when none);
    // the solution does not depend on it beyond the 1e-15-day tolerance.
    Result<void> vector_at(int id, double jd_tt, const CalcOptions& o, double out[3], double& tau,
                           int& origin) {
        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double obs[6];
        auto r = observer(o, jd_tt, jd_tdb, obs);
        if (!r)
            return r;

        double tgt[6];
        double p[3];
        if (!o.light_time || !(tau >= 0.0 && tau < 1.0))
            tau = 0.0;
        r = retarded(id, jd_tdb, tau, tgt, &origin);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            p[i] = tgt[i] - obs[i];
        if (o.light_time) {
            // Newton's method on f(tau) = |p(tau)| - c tau, with p's rate
            // from the body's velocity: quadratic convergence, two or three
            // ephemeris reads from a cold start and one or two from a
            // nearby epoch's tau.
            for (int it = 0; it < 12; ++it) {
                const double d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                const double dp = -(p[0] * tgt[3] + p[1] * tgt[4] + p[2] * tgt[5]) / d;
                const double step =
                    (d - apparent::kLightKmPerDay * tau) / (apparent::kLightKmPerDay - dp);
                tau += step;
                r = retarded(id, jd_tdb, tau, tgt, nullptr);
                if (!r)
                    return r;
                for (int i = 0; i < 3; ++i)
                    p[i] = tgt[i] - obs[i];
                if (std::fabs(step) < 1e-15)
                    break;
            }
        }

        // Deflection is honoured for every observer that is not the Sun
        // itself (3.5a): the barycentre sits ~0.005 AU from the Sun's
        // centre, close enough to deflect, and it is applied there.
        const bool observer_is_sun = o.center == Center::Heliocentric ||
                                     (o.center == Center::Body && o.center_body == body::kSun);
        if (o.deflection && !observer_is_sun && id != body::kSun) {
            double sun[6];
            r = sun_at(jd_tdb, sun);
            if (!r)
                return r;
            const double sun_to_body[3] = {tgt[0] - sun[0], tgt[1] - sun[1], tgt[2] - sun[2]};
            const double sun_to_obs[3] = {obs[0] - sun[0], obs[1] - sun[1], obs[2] - sun[2]};
            apparent::light_deflection(p, sun_to_body, sun_to_obs, apparent::kSunGmOverC2Km, p);
        }
        if (o.aberration)
            apparent::aberration(p, obs + 3, p);

        return to_output(jd_tt, o, p, out);
    }

    // The ecliptic an orbit point is defined on: the mean ecliptic of date,
    // whatever the output frame, as a rotation from ICRF. Protocol v4
    // 3.5a (the 2026-09-18 amendment): a frame gives the coordinates a point
    // is expressed in and does not change which point it is, so a node asked
    // in J2000 or ICRF is the node of date, rotated.
    void orbit_ecliptic(double jd_tt, const CalcOptions& o, double m[9]) {
        const EpochFrames& f = frames_at(jd_tt, false, o.precession);
        double e[9];
        rot1(f.eps_mean, e);
        matmul(e, f.pb, m);
    }

    // Barycentric position (km, ICRF) of a node or apsis of body `id`. `dt`
    // (days) is a correction to the instant, applied through the focus's
    // velocity: the rounding error of a retarded time, which a JD double
    // cannot hold (computed_point_vector_at).
    Result<void> orbit_point(int id, OrbitPoint point, OrbitElements elements, double jd_tt,
                             double jd_tdb, const CalcOptions& o, double out[3], double dt = 0.0) {
        if (elements == OrbitElements::Mean)
            return mean_orbit_point(id, point, jd_tt, jd_tdb, o, out, dt);
        if (elements == OrbitElements::Interpolated)
            return natural_apsis(id, point, jd_tdb, out, dt);
        if (id == body::kSun || id == body::kSolarSystemBary)
            return make_error(ErrorCode::ArgumentError, "the Sun has no heliocentric orbit");
        const int center = id == body::kMoon ? body::kEarth : body::kSun;
        double b[6], c[6];
        auto r = body_barycentric(id, jd_tdb, b, nullptr);
        if (!r)
            return r;
        r = center == body::kSun ? sun_at(jd_tdb, c) : source->barycentric(center, jd_tdb, c);
        if (!r)
            return r;
        const double au3 = kAuKm * kAuKm * kAuKm;
        const double mu =
            (gm_or_builtin(source.get(), center) + gm_or_builtin(source.get(), id)) * au3;
        double m[9];
        orbit_ecliptic(jd_tt, o, m);
        double rel[3], vel[3], x[3], v[3];
        for (int i = 0; i < 3; ++i) {
            rel[i] = b[i] - c[i];
            vel[i] = b[3 + i] - c[3 + i];
        }
        apply(m, rel, x);
        apply(m, vel, v);
        const double h[3] = {x[1] * v[2] - x[2] * v[1], x[2] * v[0] - x[0] * v[2],
                             x[0] * v[1] - x[1] * v[0]};
        const double h2 = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
        const double rn = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (!(mu > 0.0) || !(h2 > 0.0) || !(rn > 0.0))
            return make_error(ErrorCode::ArgumentError, "no orbit (degenerate state)");
        const double vxh[3] = {v[1] * h[2] - v[2] * h[1], v[2] * h[0] - v[0] * h[2],
                               v[0] * h[1] - v[1] * h[0]};
        double ev[3];
        for (int i = 0; i < 3; ++i)
            ev[i] = vxh[i] / mu - x[i] / rn;
        const double semi_latus = h2 / mu;
        double hi[3], ei[3], rel_point[3];
        apply_transpose(m, h, hi);
        apply_transpose(m, ev, ei);
        r = conic_point(hi, ei, semi_latus, point, m, rel_point);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] = c[i] + c[3 + i] * dt + rel_point[i];
        return {};
    }

    // A node or apsis of the conic with angular momentum direction hi and
    // eccentricity vector ei (both ICRF) and semi-latus rectum semi_latus
    // (km), on the ecliptic m (a rotation from ICRF); out is relative to the
    // conic's focus, ICRF km.
    Result<void> conic_point(const double hi[3], const double ei[3], double semi_latus,
                             OrbitPoint point, const double m[9], double out[3]) {
        double h[3], ev[3];
        apply(m, hi, h);
        apply(m, ei, ev);
        const double h2 = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
        double u[3] = {0.0, 0.0, 0.0};
        switch (point) {
        case OrbitPoint::AscendingNode:
        case OrbitPoint::DescendingNode: {
            // The node line: z x h, toward the ascending node.
            const double nn = std::hypot(h[0], h[1]);
            if (!(nn > 1e-12 * std::sqrt(h2)))
                return make_error(ErrorCode::ArgumentError,
                                  "the orbit lies in the ecliptic: nodes are undefined");
            const double sign = point == OrbitPoint::AscendingNode ? 1.0 : -1.0;
            u[0] = -sign * h[1] / nn;
            u[1] = sign * h[0] / nn;
            u[2] = 0.0;
            break;
        }
        case OrbitPoint::Perihelion:
        case OrbitPoint::Aphelion: {
            const double e = std::sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]);
            if (!(e > 1e-12))
                return make_error(ErrorCode::ArgumentError,
                                  "the orbit is circular: apsides are undefined");
            if (point == OrbitPoint::Aphelion && !(e < 1.0))
                return make_error(ErrorCode::ArgumentError,
                                  "the orbit is open: the aphelion is undefined");
            const double sign = point == OrbitPoint::Perihelion ? 1.0 : -1.0;
            for (int i = 0; i < 3; ++i)
                u[i] = sign * ev[i] / e;
            break;
        }
        }
        // r = p / (1 + e cos nu), with e cos nu = e . u along direction u.
        const double denom = 1.0 + (ev[0] * u[0] + ev[1] * u[1] + ev[2] * u[2]);
        if (!(denom > 1e-12))
            return make_error(ErrorCode::ArgumentError, "the open orbit does not reach this node");
        const double dist = semi_latus / denom;
        const double pe[3] = {dist * u[0], dist * u[1], dist * u[2]};
        apply_transpose(m, pe, out);
        return {};
    }

    // Orbit normal and eccentricity vector, in the elements' own ecliptic
    // frame, from e, the longitude of perihelion, sin i and the node.
    static void mean_conic(double e, double varpi, double sin_i, double node, double normal[3],
                           double ev[3]) {
        const double cos_i = std::sqrt(std::max(0.0, 1.0 - sin_i * sin_i));
        const double w = varpi - node;
        const double cn = std::cos(node), sn = std::sin(node), cw = std::cos(w), sw = std::sin(w);
        normal[0] = sin_i * sn;
        normal[1] = -sin_i * cn;
        normal[2] = cos_i;
        ev[0] = e * (cn * cw - sn * sw * cos_i);
        ev[1] = e * (sn * cw + cn * sw * cos_i);
        ev[2] = e * sw * sin_i;
    }

    Result<void> mean_orbit_point(int id, OrbitPoint point, double jd_tt, double jd_tdb,
                                  const CalcOptions& o, double out[3], double dt = 0.0) {
        double m[9];
        orbit_ecliptic(jd_tt, o, m);
        double normal[3], ev[3], hi[3], ei[3], rel[3], focus[6];
        double semi_latus = 0.0;
        if (id == body::kMoon) {
            double phi[14];
            frames::fundamental_arguments(jd_tt, phi);
            const double node = phi[13];                     // Omega
            const double varpi = phi[11] + phi[13] - phi[9]; // L - l, with L = F + Omega
            const double e = kMoonMeanEccentricity;
            mean_conic(e, varpi, std::sin(kMoonMeanInclinationDeg / kRad2Deg), node, normal, ev);
            semi_latus = kMoonMeanDistanceKm * (1.0 - e * e);
            // Referred to the mean ecliptic and equinox of date.
            const EpochFrames& f = frames_at(jd_tt, false, o.precession);
            double date[9], e1[9];
            rot1(f.eps_mean, e1);
            matmul(e1, f.pb, date);
            apply_transpose(date, normal, hi);
            apply_transpose(date, ev, ei);
            auto r = source->barycentric(body::kEarth, jd_tdb, focus);
            if (!r)
                return r;
        } else {
            int key = id;
            if (id == 1 || id == 2)
                key = id * 100 + 99;
            else if (id == body::kEarth)
                key = body::kEarthMoonBary;
            else if (id >= 499 && id <= 999 && id % 100 == 99)
                key = id / 100;
            const MeanElementsFit* fit = nullptr;
            for (const MeanElementsFit& f : kMeanElements)
                if (f.id == key)
                    fit = &f;
            if (!fit)
                return make_error(ErrorCode::NotFound,
                                  "mean elements exist for the Moon and the major planets only");
            const double T = (jd_tdb - kJ2000) / 36525.0;
            const auto poly = [T](const double c[3]) { return c[0] + T * (c[1] + T * c[2]); };
            const double a = poly(fit->a), hh = poly(fit->h), kk = poly(fit->k);
            const double pp = poly(fit->p), qq = poly(fit->q);
            const double e = std::hypot(hh, kk);
            const double sin_i = std::min(1.0, std::hypot(pp, qq));
            mean_conic(e, std::atan2(hh, kk), sin_i, std::atan2(pp, qq), normal, ev);
            semi_latus = a * (1.0 - e * e) * kAuKm;
            // Referred to the J2000 ecliptic of the fit.
            double j2000[9], e1[9];
            rot1(eps_j2000, e1);
            matmul(e1, bias, j2000);
            apply_transpose(j2000, normal, hi);
            apply_transpose(j2000, ev, ei);
            auto r = sun_at(jd_tdb, focus);
            if (!r)
                return r;
        }
        auto r = conic_point(hi, ei, semi_latus, point, m, rel);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] = focus[i] + focus[3 + i] * dt + rel[i];
        return {};
    }

    // The natural lunar apogee and perigee (docs/ORBIT-POINTS.md, "The natural
    // apsides"): the published definition is an interpolation between the
    // Moon's actual passages through apogee and perigee. A passage is a zero of
    // r.v, the geometric Earth-Moon distance at a maximum (apogee) or minimum
    // (perigee); the Moon's geocentric place there, in the mean ecliptic of
    // J2000, is a node. Longitude is the mean apse plus the deviation model
    // (natural_apsides.hpp: a function of the Sun's elongation from the mean
    // apse, fitted to every passage in DE440) plus the residual at the nodes,
    // interpolated; latitude and distance are interpolated directly. The
    // interpolation is a cubic Hermite whose slopes at each node are those of
    // the quartic through it and its two neighbours on each side, so the curve
    // and its rate are continuous.
    struct ApsisPassage {
        double t;   // TDB
        double res; // rad: longitude less mean apse and model
        double lat; // rad
        double r;   // km
    };
    std::vector<ApsisPassage> apsis_passages[2]; // [0] perigees, [1] apogees
    double apsis_lo = NAN, apsis_hi = NAN;       // the TDB span scanned

    // The Moon relative to the Earth, geometric: ICRF km, km/day.
    Result<void> moon_geocentric(double t, double s[6]) {
        double m[6], e[6];
        auto r = source->barycentric(body::kMoon, t, m);
        if (!r)
            return r;
        r = source->barycentric(body::kEarth, t, e);
        if (!r)
            return r;
        for (int i = 0; i < 6; ++i)
            s[i] = m[i] - e[i];
        return {};
    }

    Result<double> moon_radial(double t) {
        double s[6];
        auto r = moon_geocentric(t, s);
        if (!r)
            return r.error();
        return s[0] * s[3] + s[1] * s[4] + s[2] * s[5];
    }

    // Every apsis passage in [a, b] (TDB): r.v sampled daily (the passages are
    // ~14 days apart), each sign change refined by the Illinois method to
    // 1e-9 day.
    Result<void> scan_apsides(double a, double b) {
        auto fa = moon_radial(a);
        if (!fa)
            return fa.error();
        double ta = a, ya = fa.value();
        while (ta < b) {
            const double tb = std::min(ta + 1.0, b);
            auto fb = moon_radial(tb);
            if (!fb)
                return fb.error();
            const double yb = fb.value();
            if ((ya > 0.0) != (yb > 0.0)) {
                double lo = ta, hi = tb, flo = ya, fhi = yb;
                int side = 0;
                for (int it = 0; it < 200 && hi - lo > 1e-9; ++it) {
                    const double t = (lo * fhi - hi * flo) / (fhi - flo);
                    auto ft = moon_radial(t);
                    if (!ft)
                        return ft.error();
                    if ((ft.value() > 0.0) == (flo > 0.0)) {
                        lo = t;
                        flo = ft.value();
                        if (side == -1)
                            fhi /= 2.0;
                        side = -1;
                    } else {
                        hi = t;
                        fhi = ft.value();
                        if (side == 1)
                            flo /= 2.0;
                        side = 1;
                    }
                }
                const double t = 0.5 * (lo + hi);
                double s[6];
                auto r = moon_geocentric(t, s);
                if (!r)
                    return r;
                double m[9], e1[9], v[3];
                rot1(eps_j2000, e1);
                matmul(e1, bias, m);
                apply(m, s, v);
                const double rr = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                // r.v falling through zero is a maximum of the distance.
                const bool apogee = ya > 0.0;
                double lon_model = 0.0;
                r = apsis_model(t, apogee, lon_model);
                if (!r)
                    return r;
                apsis_passages[apogee].push_back(
                    {t, std::remainder(std::atan2(v[1], v[0]) - lon_model, 2.0 * M_PI),
                     std::asin(v[2] / rr), rr});
            }
            ta = tb;
            ya = yb;
        }
        return {};
    }

    // Mean apse plus the deviation model at t (TDB), mean ecliptic of J2000.
    Result<void> apsis_model(double t, bool apogee, double& lon) {
        double sun[6], earth[6];
        auto r = sun_at(t, sun);
        if (!r)
            return r;
        r = source->barycentric(body::kEarth, t, earth);
        if (!r)
            return r;
        double g[3], m[9], e1[9], v[3];
        for (int i = 0; i < 3; ++i)
            g[i] = sun[i] - earth[i];
        rot1(eps_j2000, e1);
        matmul(e1, bias, m);
        apply(m, g, v);
        const double ref = natural_apsides::mean_apse(t, apogee);
        lon = ref + natural_apsides::model(
                        apogee ? natural_apsides::kApogeeModel : natural_apsides::kPerigeeModel,
                        std::remainder(std::atan2(v[1], v[0]) - ref, 2.0 * M_PI));
        return {};
    }

    // Passages covering [t - 100 d, t + 100 d]: at least three of each kind on
    // each side of t. A nearby request extends the span scanned; a far one
    // starts it again.
    Result<void> ensure_apsides(double t) {
        const double lo = t - 100.0, hi = t + 100.0;
        if (std::isfinite(apsis_lo) && lo >= apsis_lo && hi <= apsis_hi)
            return {};
        const bool near = std::isfinite(apsis_lo) && lo < apsis_hi + 365.0 &&
                          hi > apsis_lo - 365.0 && apsis_hi - apsis_lo < 50.0 * 365.25;
        if (!near) {
            apsis_passages[0].clear();
            apsis_passages[1].clear();
            apsis_lo = apsis_hi = NAN;
            auto r = scan_apsides(lo, hi);
            if (!r)
                return r;
            apsis_lo = lo;
            apsis_hi = hi;
        } else {
            if (lo < apsis_lo) {
                auto r = scan_apsides(lo, apsis_lo);
                if (!r)
                    return r;
                apsis_lo = lo;
            }
            if (hi > apsis_hi) {
                auto r = scan_apsides(apsis_hi, hi);
                if (!r)
                    return r;
                apsis_hi = hi;
            }
        }
        for (auto& v : apsis_passages) {
            std::sort(v.begin(), v.end(),
                      [](const ApsisPassage& x, const ApsisPassage& y) { return x.t < y.t; });
            // A passage on a seam between two scans is found twice.
            v.erase(std::unique(v.begin(), v.end(),
                                [](const ApsisPassage& x, const ApsisPassage& y) {
                                    return std::fabs(x.t - y.t) < 1e-6;
                                }),
                    v.end());
        }
        return {};
    }

    Result<void> natural_apsis(int id, OrbitPoint point, double jd_tdb, double out[3], double dt) {
        if (id != body::kMoon || (point != OrbitPoint::Perihelion && point != OrbitPoint::Aphelion))
            return make_error(ErrorCode::ArgumentError,
                              "the interpolated method is defined for the Moon's apogee and "
                              "perigee only");
        auto r = ensure_apsides(jd_tdb);
        if (!r)
            return r;
        const std::vector<ApsisPassage>& v = apsis_passages[point == OrbitPoint::Aphelion];
        const auto it = std::upper_bound(v.begin(), v.end(), jd_tdb,
                                         [](double t, const ApsisPassage& p) { return t < p.t; });
        const long k = long(it - v.begin()) - 1; // v[k].t <= jd_tdb < v[k+1].t
        if (k < 2 || k + 3 >= long(v.size()))
            return make_error(ErrorCode::ArgumentError,
                              "too near the ephemeris's coverage limits: the interpolation "
                              "needs three apsis passages on each side");
        // Nodes k-2 .. k+3.
        double x[6], y[3][6];
        for (int j = 0; j < 6; ++j) {
            const ApsisPassage& p = v[size_t(k - 2 + j)];
            x[j] = p.t;
            y[0][j] = p.res;
            y[1][j] = p.lat;
            y[2][j] = p.r;
        }
        // The slope at x[m] of the quartic through x[m-2] .. x[m+2].
        const auto slope = [&](const double* yy, int m) {
            double d = 0.0;
            for (int j = m - 2; j <= m + 2; ++j) {
                double lj = 0.0;
                if (j == m) {
                    for (int i = m - 2; i <= m + 2; ++i)
                        if (i != m)
                            lj += 1.0 / (x[m] - x[i]);
                } else {
                    double num = 1.0, den = 1.0;
                    for (int i = m - 2; i <= m + 2; ++i) {
                        if (i != j && i != m)
                            num *= x[m] - x[i];
                        if (i != j)
                            den *= x[j] - x[i];
                    }
                    lj = num / den;
                }
                d += yy[j] * lj;
            }
            return d;
        };
        const double h = x[3] - x[2], s = (jd_tdb - x[2]) / h;
        const double h00 = (2.0 * s - 3.0) * s * s + 1.0, h10 = ((s - 2.0) * s + 1.0) * s;
        const double h01 = (3.0 - 2.0 * s) * s * s, h11 = (s - 1.0) * s * s;
        double q[3];
        for (int c = 0; c < 3; ++c)
            q[c] =
                h00 * y[c][2] + h10 * h * slope(y[c], 2) + h01 * y[c][3] + h11 * h * slope(y[c], 3);
        double lon_model = 0.0;
        r = apsis_model(jd_tdb, point == OrbitPoint::Aphelion, lon_model);
        if (!r)
            return r;
        const double lon = lon_model + q[0];
        const double ecl[3] = {q[2] * std::cos(q[1]) * std::cos(lon),
                               q[2] * std::cos(q[1]) * std::sin(lon), q[2] * std::sin(q[1])};
        double m[9], e1[9], rel[3], earth[6];
        rot1(eps_j2000, e1);
        matmul(e1, bias, m);
        apply_transpose(m, ecl, rel);
        r = source->barycentric(body::kEarth, jd_tdb, earth);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] = earth[i] + earth[3 + i] * dt + rel[i];
        return {};
    }

    // Observer -> a point that is computed rather than read from the
    // ephemeris -- an orbit point, a body from elements -- in the output
    // frame (km), with the corrections a body gets. `at(tt, tdb, dt, pt)`
    // gives the point's barycentric ICRF position (km) at the instant tdb + dt
    // (tt for what moves slowly: frames, mean elements). jd_tdb - tau is
    // rounded to a JD double (~40 us near the present), and dt carries the
    // exact rounding error back, as retarded() does for a body. Without it a Moon point, whose
    // focus is the Earth at 30 km/s, jittered by ~1 m: 0.6 mas topocentric, where tau varies with
    // the site.
    template <class PointFn>
    Result<void> computed_point_vector_at(double jd_tt, const CalcOptions& o, PointFn&& at,
                                          bool point_is_sun, double out[3], double& tau) {
        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double obs[6], pt[3];
        auto r = observer(o, jd_tt, jd_tdb, obs);
        if (!r)
            return r;
        if (!o.light_time || !(tau >= 0.0 && tau < 1.0))
            tau = 0.0;
        // jd_tdb - tau, with its rounding error recovered (TwoSum).
        const auto retarded_at = [&](double t_lt, double pt_out[3]) {
            const double s = jd_tdb - t_lt;
            const double bp = s - jd_tdb;
            const double err = (jd_tdb - (s - bp)) + (-t_lt - bp);
            return at(jd_tt - t_lt, s, err, pt_out);
        };
        r = retarded_at(tau, pt);
        if (!r)
            return r;
        double p[3];
        for (int i = 0; i < 3; ++i)
            p[i] = pt[i] - obs[i];
        if (o.light_time) {
            // These points move slowly against light -- an orbit point with
            // its orbit's precession, a hypothetical planet on a period of
            // centuries -- so the light-time equation contracts by the
            // point's radial speed over c each pass, and a fixed-point
            // iteration needs no derivative where a body uses Newton's
            // method. Three passes are already at roundoff; the loop stops
            // on the step.
            for (int it = 0; it < 8; ++it) {
                const double d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                const double next = d / apparent::kLightKmPerDay;
                const double step = next - tau;
                tau = next;
                r = retarded_at(tau, pt);
                if (!r)
                    return r;
                for (int i = 0; i < 3; ++i)
                    p[i] = pt[i] - obs[i];
                if (std::fabs(step) < 1e-15)
                    break;
            }
        }
        // Deflection is honoured for every observer that is not the Sun
        // itself (3.5a): the barycentre sits ~0.005 AU from the Sun's
        // centre, close enough to deflect, and it is applied there.
        const bool observer_is_sun = o.center == Center::Heliocentric ||
                                     (o.center == Center::Body && o.center_body == body::kSun);
        if (o.deflection && !observer_is_sun && !point_is_sun) {
            double sun[6];
            r = sun_at(time::tdb_from_tt(jd_tt - tau), sun);
            if (!r)
                return r;
            const double sun_to_point[3] = {pt[0] - sun[0], pt[1] - sun[1], pt[2] - sun[2]};
            const double sun_to_obs[3] = {obs[0] - sun[0], obs[1] - sun[1], obs[2] - sun[2]};
            apparent::light_deflection(p, sun_to_point, sun_to_obs, apparent::kSunGmOverC2Km, p);
        }
        if (o.aberration)
            apparent::aberration(p, obs + 3, p);
        return to_output(jd_tt, o, p, out);
    }

    // Observer -> orbit point vector in the output frame (km). A node exists
    // to be compared against apparent body positions, so it gets the
    // corrections they get. The observer-velocity term is the large one --
    // 21 arcsec on Jupiter's node against 0.005 arcsec of light time; see
    // docs/ORBIT-POINTS.md.
    Result<void> orbit_point_vector_at(int id, OrbitPoint point, OrbitElements elements,
                                       double jd_tt, const CalcOptions& o, double out[3],
                                       double& tau) {
        return computed_point_vector_at(
            jd_tt, o,
            [&](double t, double t_tdb, double dt, double pt[3]) {
                return orbit_point(id, point, elements, t, t_tdb, o, pt, dt);
            },
            id == body::kSun, out, tau);
    }

    // ICRF -> the mean ecliptic and equinox of an epoch.
    static void ecliptic_of(const EpochFrames& f, double out[9]) {
        double e1[9];
        rot1(f.eps_mean, e1);
        matmul(e1, f.pb, out);
    }

    // A body from polynomial elements (v4 kind 4): its barycentric ICRF
    // position (km) at jd_tt. docs/HYPOTHETICALS.md has the conventions.
    Result<void> elements_point(const PolynomialElements& el, double jd_tt, const CalcOptions& o,
                                double out[3]) {
        const double T = (jd_tt - el.epoch_jd_tt) / 36525.0;
        const auto poly = [&](const double c[5]) { return elements::evaluate(c, el.n_terms, T); };
        Elements ke;
        ke.a = poly(el.semi_major_axis);
        ke.e = poly(el.eccentricity);
        if (!(ke.a > 0.0) || !(ke.e >= 0.0 && ke.e < 1.0))
            return make_error(ErrorCode::ArgumentError,
                              "the elements do not describe a bound orbit at this instant");
        ke.argp = poly(el.arg_perihelion) / kRad2Deg;
        ke.node = poly(el.ascending_node) / kRad2Deg;
        ke.inc = poly(el.inclination) / kRad2Deg;
        ke.mean_anom = std::fmod(elements::mean_anomaly_deg(el, jd_tt), 360.0) / kRad2Deg;
        // Only the position is used, which does not depend on mu; the rates
        // are those of the returned position, as for any body.
        auto state = elements_to_state(elements::kGaussK * elements::kGaussK, ke);
        if (!state)
            return state.error();
        const Vec3& q = state.value().pos;
        const double ecl[3] = {q.x * kAuKm, q.y * kAuKm, q.z * kAuKm};

        // The elements' plane is the mean ecliptic and equinox of their
        // equinox epoch; the engine's own precession carries it to the ICRF.
        double eq = kJ2000;
        switch (el.equinox) {
        case ElementEquinox::J2000:
            eq = kJ2000;
            break;
        case ElementEquinox::B1950:
            eq = kB1950;
            break;
        case ElementEquinox::J1900:
            eq = kJ1900;
            break;
        case ElementEquinox::OfDate:
            eq = jd_tt;
            break;
        case ElementEquinox::Explicit:
            eq = el.equinox_jd_tt;
            break;
        }
        double date_frame[9], rel[3];
        const double* to_ecliptic = element_frame;
        if (el.equinox == ElementEquinox::OfDate) {
            ecliptic_of(frames_at(eq, false, o.precession), date_frame);
            to_ecliptic = date_frame;
        } else if (!(element_frame_jd == eq && element_frame_model == o.precession)) {
            ecliptic_of(frames_at(eq, false, o.precession), element_frame);
            element_frame_jd = eq;
            element_frame_model = o.precession;
        }
        apply_transpose(to_ecliptic, ecl, rel);

        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double centre[6];
        auto r = el.origin == ElementOrigin::Earth
                     ? source->barycentric(body::kEarth, jd_tdb, centre)
                     : sun_at(jd_tdb, centre);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] = centre[i] + rel[i];
        return {};
    }

    Result<void> elements_vector_at(const PolynomialElements& el, double jd_tt,
                                    const CalcOptions& o, double out[3], double& tau) {
        return computed_point_vector_at(
            jd_tt, o,
            // The rounding error is dropped: an element body's focus is the
            // Sun (~0.01 km/s), and its own motion over ~40 us is under a
            // metre at planetary distances.
            [&](double t, double, double, double pt[3]) { return elements_point(el, t, o, pt); },
            false, out, tau);
    }

    // Observer -> catalog object vector in the output frame (km).
    // A catalog star's straight line: its barycentric position (km, ICRF) at
    // jd_tdb, its place at the catalog epoch moved by its space velocity, which
    // is the proper motion across the line of sight at the parallax distance
    // and the radial velocity along it (only with a parallax: without a
    // distance a velocity along the line of sight has no meaning).
    static void star_line_km(const stars::Object& star, double jd_tdb, double pos[3]) {
        const double a = star.ra_deg / kRad2Deg, d = star.dec_deg / kRad2Deg;
        const double ca = std::cos(a), sa = std::sin(a), cd = std::cos(d), sd = std::sin(d);
        const double u[3] = {cd * ca, cd * sa, sd};
        const double east[3] = {-sa, ca, 0.0};
        const double north[3] = {-sd * ca, -sd * sa, cd};
        constexpr double kMasPerRad = 206264806.24709636;
        const bool has_parallax = star.parallax_mas > 0.0;
        const double dist_au = has_parallax ? kMasPerRad / star.parallax_mas : kStarNoParallaxAu;
        const double pm_a = star.pm_ra_mas_yr / kMasPerRad / 365.25; // rad/day
        const double pm_d = star.pm_dec_mas_yr / kMasPerRad / 365.25;
        const double rv = has_parallax ? star.rv_km_s * 86400.0 / kAuKm : 0.0; // AU/day
        const double dt = jd_tdb - (kJ2000 + (star.epoch_jyear - 2000.0) * 365.25);
        for (int i = 0; i < 3; ++i) {
            const double v = dist_au * (pm_a * east[i] + pm_d * north[i]) + rv * u[i];
            pos[i] = (dist_au * u[i] + v * dt) * kAuKm;
        }
    }

    // A catalog star's barycentric position (km, ICRF) at jd_tdb: its straight
    // line, and for a binary the orbit the line misses (kBinaryOrbits):
    // - a barycentre line misses all of the star's offset;
    // - a component's line holds the offset and its rate at the catalog epoch,
    //   so only the rest is added;
    // - a secondary is its primary's position plus the relative orbit, at its
    //   own catalog distance.
    void star_barycentric_km(const stars::Object& star, double jd_tdb, double pos[3]) {
        star_line_km(star, jd_tdb, pos);
        const BinaryOrbit* b = star.hip ? binary_orbit(star.hip) : nullptr;
        if (!b)
            return;
        const double r = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
        double e1, n1; // arcsec
        if (b->line == BinaryLine::Secondary) {
            auto primary = star_by_hip(b->partner_hip);
            const BinaryOrbit* pb = primary ? binary_orbit(b->partner_hip) : nullptr;
            if (!pb)
                return;
            star_barycentric_km(stars::at(primary.value()), jd_tdb,
                                pos); // the primary, orbit included
            const double pr = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
            for (int i = 0; i < 3; ++i)
                pos[i] *= r / pr; // at this star's distance
            // The relative orbit: this star's offset less the primary's.
            double es, ns, ep, np;
            binary_offset(*b, jd_tdb, es, ns);
            binary_offset(*pb, jd_tdb, ep, np);
            e1 = es - ep;
            n1 = ns - np;
        } else {
            binary_offset(*b, jd_tdb, e1, n1);
            if (b->line == BinaryLine::Component) {
                const double epoch_jd = kJ2000 + (star.epoch_jyear - 2000.0) * 365.25;
                double e0, n0, ep, np, em, nm;
                binary_offset(*b, epoch_jd, e0, n0);
                binary_offset(*b, epoch_jd + 1.0, ep, np);
                binary_offset(*b, epoch_jd - 1.0, em, nm);
                const double dt = jd_tdb - epoch_jd;
                e1 -= e0 + (ep - em) / 2.0 * dt;
                n1 -= n0 + (np - nm) / 2.0 * dt;
            }
        }
        // East and north at the date, at the line's direction (the primary's,
        // for a secondary): position angles are measured from the north of
        // the date, and the catalog's own ephemeris agrees only in this
        // plane (STARS.md, "Binary stars"). The catalog position's plane
        // would turn the orbit by the proper motion's d(RA) sin(Dec): 0.065
        // deg for alpha Cen by 2025.
        const double a = std::atan2(pos[1], pos[0]);
        const double d = std::atan2(pos[2], std::sqrt(pos[0] * pos[0] + pos[1] * pos[1]));
        const double east[3] = {-std::sin(a), std::cos(a), 0.0};
        const double north[3] = {-std::sin(d) * std::cos(a), -std::sin(d) * std::sin(a),
                                 std::cos(d)};
        for (int i = 0; i < 3; ++i)
            pos[i] += r * (e1 * east[i] + n1 * north[i]) / 206264.80624709636;
    }

    Result<void> star_vector_at(const stars::Object& star, double jd_tt, const CalcOptions& o,
                                double out[3]) {
        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double obs[6];
        auto r = observer(o, jd_tt, jd_tdb, obs);
        if (!r)
            return r;
        double pos[3];
        star_barycentric_km(star, jd_tdb, pos);
        double p[3] = {pos[0] - obs[0], pos[1] - obs[1], pos[2] - obs[2]};
        // Deflection is honoured for every observer that is not the Sun
        // itself (3.5a): the barycentre sits ~0.005 AU from the Sun's
        // centre, close enough to deflect, and it is applied there.
        const bool observer_is_sun = o.center == Center::Heliocentric ||
                                     (o.center == Center::Body && o.center_body == body::kSun);
        if (o.deflection && !observer_is_sun) {
            double sun[6];
            r = sun_at(jd_tdb, sun);
            if (!r)
                return r;
            const double sun_to_body[3] = {pos[0] - sun[0], pos[1] - sun[1], pos[2] - sun[2]};
            const double sun_to_obs[3] = {obs[0] - sun[0], obs[1] - sun[1], obs[2] - sun[2]};
            // Skip a direction within the solar disc, where the formula's
            // 1 + q.e denominator vanishes and the star is not visible.
            const double qn =
                std::sqrt(sun_to_body[0] * sun_to_body[0] + sun_to_body[1] * sun_to_body[1] +
                          sun_to_body[2] * sun_to_body[2]);
            const double en =
                std::sqrt(sun_to_obs[0] * sun_to_obs[0] + sun_to_obs[1] * sun_to_obs[1] +
                          sun_to_obs[2] * sun_to_obs[2]);
            const double qe = (sun_to_body[0] * sun_to_obs[0] + sun_to_body[1] * sun_to_obs[1] +
                               sun_to_body[2] * sun_to_obs[2]) /
                              (qn * en);
            if (1.0 + qe > 1e-9)
                apparent::light_deflection(p, sun_to_body, sun_to_obs, apparent::kSunGmOverC2Km, p);
        }
        if (o.aberration)
            apparent::aberration(p, obs + 3, p);
        return to_output(jd_tt, o, p, out);
    }

    // Spherical coordinates and rates from a vector and its central
    // difference; vec(jd, out, tau) fills one output-frame vector (km).
    template <typename VectorFn>
    Result<void> position(double jd_tt, const CalcOptions& o, double& tau, VectorFn&& vec,
                          Position& pos) {
        double v[3];
        auto r = vec(jd_tt, v, tau);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            pos.xyz_au[i] = v[i] / kAuKm;
        if (o.speed) {
            const double tp = jd_tt + kSpeedStepDays, tm = jd_tt - kSpeedStepDays;
            // The stencil starts its light-time solve from the centre's tau.
            double vp[3], vm[3], tau_p = tau, tau_m = tau;
            r = vec(tp, vp, tau_p);
            if (!r)
                return r;
            r = vec(tm, vm, tau_m);
            if (!r)
                return r;
            const double span = (tp - tm) * kAuKm; // actual, rounded, step
            for (int i = 0; i < 3; ++i)
                pos.vel_au_day[i] = (vp[i] - vm[i]) / span;
        }
        const double* x = pos.xyz_au;
        const double* dx = pos.vel_au_day;
        const double rho2 = x[0] * x[0] + x[1] * x[1];
        const double r2 = rho2 + x[2] * x[2];
        const double rho = std::sqrt(rho2), rr = std::sqrt(r2);
        double lon = std::atan2(x[1], x[0]) * kRad2Deg;
        if (lon < 0.0)
            lon += 360.0;
        if (lon >= 360.0)
            lon -= 360.0;
        pos.lon_deg = lon;
        pos.lat_deg = std::atan2(x[2], rho) * kRad2Deg;
        pos.dist_au = rr;
        if (o.speed && rho > 0.0) {
            pos.lon_speed = (x[0] * dx[1] - x[1] * dx[0]) / rho2 * kRad2Deg;
            pos.lat_speed =
                (dx[2] * rho2 - x[2] * (x[0] * dx[0] + x[1] * dx[1])) / (r2 * rho) * kRad2Deg;
            pos.dist_speed = (x[0] * dx[0] + x[1] * dx[1] + x[2] * dx[2]) / rr;
        }
        // Never a success with a non-finite answer, whatever produced it: an
        // input the checks above missed arrives here as NaN or infinity, and a
        // caller must see an error, not coordinates (the speeds block above
        // skips on NaN, which would leave rates of 0 beside a NaN position).
        bool finite =
            std::isfinite(pos.lon_deg) && std::isfinite(pos.lat_deg) && std::isfinite(pos.dist_au);
        for (int i = 0; i < 3; ++i)
            finite = finite && std::isfinite(x[i]) && (!o.speed || std::isfinite(dx[i]));
        if (!finite || (o.speed && !(std::isfinite(pos.lon_speed) && std::isfinite(pos.lat_speed) &&
                                     std::isfinite(pos.dist_speed))))
            return make_error(ErrorCode::ArgumentError,
                              "numerical failure: the answer is not finite");
        return {};
    }

    double ut1_to_tt(double jd_ut1) const {
        // Delta T is a function of TT; one fixed-point pass is ample
        // (dDeltaT/dt is ~1e-8, so the argument error is sub-microsecond).
        const double jd_tt = jd_ut1 + delta_t_seconds(jd_ut1) / 86400.0;
        return jd_ut1 + delta_t_seconds(jd_tt) / 86400.0;
    }

    // An ICRF vector (km) into the requested output frame and zodiac.
    Result<void> to_output(double jd_tt, const CalcOptions& o, const double p[3], double out[3]) {
        if (o.sidereal != SiderealMode::Tropical &&
            o.sidereal_plane != SiderealPlane::EclipticOfDate) {
            if (o.coords != Coords::Ecliptic)
                return make_error(ErrorCode::ArgumentError,
                                  "a fixed sidereal plane needs ecliptic coordinates");
            double m[9];
            auto r = fixed_sidereal_matrix(o, jd_tt, m);
            if (!r)
                return r;
            apply(m, p, out);
            return {};
        }
        const bool need_nut = o.frame == Frame::TrueOfDate;
        double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        switch (o.frame) {
        case Frame::ICRF:
            if (o.coords == Coords::Ecliptic) {
                rot1(eps_j2000, m);
            }
            break;
        case Frame::J2000:
            if (o.coords == Coords::Ecliptic) {
                rot1(eps_j2000, m);
                matmul(m, bias, m);
            } else {
                std::memcpy(m, bias, sizeof m);
            }
            break;
        case Frame::MeanOfDate: {
            const EpochFrames& f = frames_at(jd_tt, need_nut, o.precession);
            if (o.coords == Coords::Ecliptic) {
                rot1(f.eps_mean, m);
                matmul(m, f.pb, m);
            } else {
                std::memcpy(m, f.pb, sizeof m);
            }
            break;
        }
        case Frame::TrueOfDate: {
            const EpochFrames& f = frames_at(jd_tt, need_nut, o.precession);
            if (o.coords == Coords::Ecliptic) {
                // The ecliptic does not nutate; the equinox slides by dpsi.
                double a[9], b[9];
                rot3(-f.dpsi, a);
                rot1(f.eps_mean, b);
                matmul(a, b, m);
                matmul(m, f.pb, m);
            } else {
                std::memcpy(m, f.npb, sizeof m);
            }
            break;
        }
        }
        // Sidereal zodiac: rotate the output ecliptic's longitude zero
        // point west by the ayanamsha (equatorial output: the same
        // rotation about the ecliptic pole, expressed through the
        // frame's ecliptic-to-equator obliquity).
        if (o.sidereal != SiderealMode::Tropical) {
            auto s = sidereal_shift(o, jd_tt);
            if (!s)
                return s.error();
            double a[9], t[9];
            // rot3(theta) moves longitudes by -theta, so a positive
            // ayanamsha rotates the zero point west as required.
            rot3(s.value() / kRad2Deg, a);
            if (o.coords == Coords::Ecliptic) {
                matmul(a, m, m);
            } else {
                double eps = 0.0;
                switch (o.frame) {
                case Frame::TrueOfDate: {
                    const EpochFrames& f = frames_at(jd_tt, true, o.precession);
                    eps = f.eps_mean + f.deps;
                    break;
                }
                case Frame::MeanOfDate:
                    eps = frames_at(jd_tt, false, o.precession).eps_mean;
                    break;
                case Frame::J2000:
                case Frame::ICRF:
                    eps = eps_j2000;
                    break;
                }
                double b[9];
                rot1(eps, b);
                matmul(a, b, t);
                rot1(-eps, b);
                matmul(b, t, t);
                matmul(t, m, m);
            }
        }
        apply(m, p, out);
        return {};
    }
};

Engine::Engine() = default;
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Result<Engine> Engine::open(const std::string& path) {
    char magic[8] = {};
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return make_error(ErrorCode::IoError, "cannot open " + path);
        f.read(magic, sizeof magic);
    }
    Engine e;
    e.impl_ = std::make_unique<Impl>();
    if (std::memcmp(magic, "DAF/SPK ", 8) == 0) {
        auto s = spk::SpkFile::open(path);
        if (!s)
            return s.error();
        e.impl_->source = std::make_unique<SpkSource>(std::move(s).value());
    } else {
        auto d = de::DeFile::open(path);
        if (!d)
            return d.error();
        e.impl_->source = std::make_unique<DeSource>(std::move(d).value());
    }
    e.impl_->perturbers.attach(e.impl_->source.get());
    frames::frame_bias_matrix(e.impl_->bias);
    e.impl_->eps_j2000 = frames::mean_obliquity(kJ2000);
    auto shipped = hypotheticals::parse(hypotheticals::shipped(), "data/hypotheticals.jsonl");
    if (!shipped)
        return shipped.error(); // a build defect: the file is checked by the tests
    e.impl_->define_hypotheticals(std::move(shipped).value());
    return e;
}

Result<CalcResult> Engine::calc(int id, double jd_tt, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    if (!std::isfinite(jd_tt))
        return make_error(ErrorCode::ArgumentError, "non-finite time");
    if ((o.center == Center::Geocentric || o.center == Center::Topocentric) && id == body::kEarth)
        return make_error(ErrorCode::ArgumentError, "body is the observer (Earth)");
    if (o.center == Center::Heliocentric && id == body::kSun)
        return make_error(ErrorCode::ArgumentError, "body is the observer (Sun)");
    if (o.center == Center::Barycentric && id == body::kSolarSystemBary)
        return make_error(ErrorCode::ArgumentError, "body is the observer (barycentre)");
    if (o.center == Center::Body && id == o.center_body)
        return make_error(ErrorCode::ArgumentError, "body is the observer (center body)");

    CalcResult res;
    double tau = 0.0;
    int origin = kFromEphemeris;
    bool first = true;
    auto r = impl_->position(
        jd_tt, o, tau,
        [&](double jd, double out[3], double& t) -> Result<void> {
            int stencil_origin;
            auto v = impl_->vector_at(id, jd, o, out, t, first ? origin : stencil_origin);
            first = false;
            return v;
        },
        res.pos);
    if (!r)
        return r.error();

    res.provenance.source = origin == kFromCatalog && !impl_->overlay_source_.empty()
                                ? std::string_view(impl_->overlay_source_)
                                : std::string_view(impl_->source->description);
    res.provenance.denum = impl_->source->denum;
    res.provenance.light_time_days = tau;
    if (o.sidereal != SiderealMode::Tropical) {
        auto s = impl_->sidereal_shift(o, jd_tt);
        if (!s)
            return s.error();
        res.ayanamsa_deg = s.value();
    }
    if (origin != kFromEphemeris && o.sigma)
        res.sigma_arcsec = impl_->sigma_arcsec(id, o, jd_tt, tau);
    return res;
}

Result<void> Engine::add_catalog(const std::string& path) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    auto r = catalog::Reader::open(path);
    if (!r)
        return r.error();

    // Stream and CRC-verify the whole container before the engine owns it,
    // so a corrupt file fails add_catalog cleanly. The name index is built
    // on the first lookup().
    auto fe = r.value().for_each([](const catalog::Record&, const catalog::Names&) {});
    if (!fe)
        return fe.error();

    impl_->catalogs.push_back(std::make_unique<catalog::Reader>(std::move(r).value()));
    impl_->name_indexes.emplace_back();
    // A newer catalog may carry revised elements for bodies already
    // integrated: drop the memoized trajectories and uncertainty tracks,
    // they rebuild lazily.
    impl_->small_bodies.clear();
    impl_->record_cache.clear();
    impl_->sigma_tracks.clear();
    impl_->refresh_overlay_source();
    return {};
}

Result<void> Engine::add_perturbers(const std::string& path) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    auto f = spk::SpkFile::open(path);
    if (!f)
        return f.error();
    auto src = std::make_unique<AsteroidSource>(std::move(f).value(), impl_->source.get());
    if (src->bodies().empty())
        return make_error(ErrorCode::FormatError,
                          path + " carries no heliocentric numbered-asteroid segments");
    std::vector<int> massive;
    for (int id : src->bodies())
        if (gm_or_builtin(src.get(), id) > 0.0)
            massive.push_back(id);
    if (massive.empty())
        return make_error(ErrorCode::FormatError,
                          path + ": no body with a known mass (DE MAnnnn constant or built-in)");
    const size_t n_massive = massive.size();
    impl_->asteroids = std::move(src);
    impl_->perturbers.attach_asteroids(impl_->asteroids.get(), std::move(massive));
    impl_->perturber_note_ =
        " + " + std::to_string(n_massive) + " " + impl_->asteroids->description;
    impl_->refresh_overlay_source();
    // Every trajectory integrated so far lacked these masses.
    impl_->small_bodies.clear();
    impl_->record_cache.clear();
    impl_->sigma_tracks.clear();
    return {};
}

void Engine::release_small_bodies() {
    if (!impl_)
        return;
    impl_->small_bodies.clear();
    impl_->record_cache.clear();
    impl_->sigma_tracks.clear();
}

Result<int> Engine::lookup(std::string_view name) const {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    const std::string key = Impl::lowercase(name);
    const uint64_t h = Impl::name_hash(key);
    // Newest catalog first; within one catalog the highest SPK-ID carrying
    // the name wins (as a later record would have overwritten it).
    for (size_t i = impl_->catalogs.size(); i-- > 0;) {
        if (auto r = impl_->ensure_name_index(i); !r)
            return r.error();
        const auto& entries = impl_->name_indexes[i].entries;
        auto lo = std::lower_bound(entries.begin(), entries.end(), std::make_pair(h, uint64_t(0)));
        uint64_t best = 0;
        for (auto it = lo; it != entries.end() && it->first == h; ++it) {
            auto rec = impl_->catalogs[i]->lookup(it->second);
            if (!rec)
                return rec.error();
            const catalog::Names n =
                catalog::record_names(impl_->catalogs[i]->name_pool(), rec.value());
            if (Impl::lowercase(n.pdes) == key ||
                (!n.name.empty() && Impl::lowercase(n.name) == key))
                best = std::max(best, it->second);
        }
        if (best != 0)
            return int(best);
    }
    return make_error(ErrorCode::NotFound, "no loaded catalog answers '" + key + "'");
}

Result<Engine::BodyNames> Engine::names(int spkid) const {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    auto r = impl_->find_record_at(spkid);
    if (!r.ok())
        return r.error();
    if (!r.value())
        return make_error(ErrorCode::NotFound,
                          "no loaded catalog carries body " + std::to_string(spkid));
    const auto [rec, catalog_i] = r.value().value();
    const catalog::Names n = catalog::record_names(impl_->catalogs[catalog_i]->name_pool(), rec);
    BodyNames out;
    out.designation = n.pdes;
    out.name = n.name;
    return out;
}

Result<CalcResult> Engine::calc_ut(int id, double jd_ut1, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    return calc(id, impl_->ut1_to_tt(jd_ut1), o);
}

Result<CalcResult> Engine::calc_orbit_point(int id, OrbitPoint point, OrbitElements elements,
                                            double jd_tt, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    if (!std::isfinite(jd_tt))
        return make_error(ErrorCode::ArgumentError, "non-finite time");
    if (int(point) < 0 || int(point) > int(OrbitPoint::Aphelion) || int(elements) < 0 ||
        int(elements) > int(OrbitElements::Interpolated))
        return make_error(ErrorCode::ArgumentError, "unknown orbit point or elements");
    CalcResult res;
    double tau = 0.0;
    auto r = impl_->position(
        jd_tt, o, tau,
        [&](double jd, double out[3], double& t) {
            return impl_->orbit_point_vector_at(id, point, elements, jd, o, out, t);
        },
        res.pos);
    if (!r)
        return r.error();
    res.provenance.source = impl_->source->description;
    res.provenance.denum = impl_->source->denum;
    res.provenance.light_time_days = tau;
    if (o.sidereal != SiderealMode::Tropical) {
        auto s = impl_->sidereal_shift(o, jd_tt);
        if (!s)
            return s.error();
        res.ayanamsa_deg = s.value();
    }
    return res;
}

Result<CalcResult> Engine::calc_elements(const PolynomialElements& el, double jd_tt,
                                         const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    if (!std::isfinite(jd_tt))
        return make_error(ErrorCode::ArgumentError, "non-finite time");
    if (el.n_terms < 1 || el.n_terms > 5)
        return make_error(ErrorCode::ArgumentError, "elements take 1 to 5 polynomial terms");
    if (int(el.equinox) < 0 || int(el.equinox) > int(ElementEquinox::Explicit) ||
        int(el.origin) < 0 || int(el.origin) > int(ElementOrigin::Earth))
        return make_error(ErrorCode::ArgumentError, "unknown element equinox or centre");
    if (!std::isfinite(el.epoch_jd_tt))
        return make_error(ErrorCode::ArgumentError, "non-finite element epoch");
    // The protocol's canonical encoding (A.16): an explicit equinox names a
    // date, and no other equinox carries one. Enforced here too, so that a
    // caller who leaves the field uninitialised finds out from the engine and
    // not from the wire.
    if (el.equinox == ElementEquinox::Explicit
            ? !(std::isfinite(el.equinox_jd_tt) && el.equinox_jd_tt != 0.0)
            : el.equinox_jd_tt != 0.0)
        return make_error(ErrorCode::ArgumentError,
                          "equinox_jd_tt is a finite date for an explicit equinox, else zero");
    CalcResult res;
    double tau = 0.0;
    auto r = impl_->position(
        jd_tt, o, tau,
        [&](double jd, double out[3], double& t) {
            return impl_->elements_vector_at(el, jd, o, out, t);
        },
        res.pos);
    if (!r)
        return r.error();
    res.provenance.source = "two-body orbital elements";
    res.provenance.light_time_days = tau;
    if (o.sidereal != SiderealMode::Tropical) {
        auto s = impl_->sidereal_shift(o, jd_tt);
        if (!s)
            return s.error();
        res.ayanamsa_deg = s.value();
    }
    return res;
}

Result<CalcResult> Engine::calc_elements_ut(const PolynomialElements& el, double jd_ut1,
                                            const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    return calc_elements(el, impl_->ut1_to_tt(jd_ut1), o);
}

Result<void> Engine::add_hypotheticals(const std::string& path) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return make_error(ErrorCode::IoError, "cannot open " + path);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto bodies = hypotheticals::parse(text, path);
    if (!bodies)
        return bodies.error();
    impl_->define_hypotheticals(std::move(bodies).value());
    return {};
}

std::vector<std::string> Engine::hypothetical_tokens() const {
    return impl_ ? impl_->hypothetical_order : std::vector<std::string>{};
}

const hypotheticals::Body* Engine::hypothetical(std::string_view token) const {
    if (!impl_)
        return nullptr;
    std::string key(token);
    for (char& c : key)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    auto it = impl_->hypothetical_by_token.find(key);
    return it == impl_->hypothetical_by_token.end() ? nullptr : it->second;
}

Result<CalcResult> Engine::calc_hypothetical(std::string_view token, double jd_tt,
                                             const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    const hypotheticals::Body* b = hypothetical(token);
    if (!b)
        return make_error(ErrorCode::NotFound,
                          "hypothetical body \"" + std::string(token) + "\" is not defined");
    auto r = calc_elements(b->elements, jd_tt, o);
    if (!r)
        return r;
    r.value().provenance.source = b->set;
    return r;
}

Result<CalcResult> Engine::calc_hypothetical_ut(std::string_view token, double jd_ut1,
                                                const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    return calc_hypothetical(token, impl_->ut1_to_tt(jd_ut1), o);
}

Result<CalcResult> Engine::calc_star(size_t star_index, double jd_tt, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    if (!std::isfinite(jd_tt))
        return make_error(ErrorCode::ArgumentError, "non-finite time");
    if (star_index >= stars::count())
        return make_error(ErrorCode::NotFound, "no catalog star " + std::to_string(star_index));
    const stars::Object& star = stars::at(star_index);
    CalcResult res;
    double tau = 0.0;
    auto r = impl_->position(
        jd_tt, o, tau,
        [&](double jd, double out[3], double&) { return impl_->star_vector_at(star, jd, o, out); },
        res.pos);
    if (!r)
        return r.error();
    static constexpr std::string_view kSources[] = {"Hipparcos new reduction (van Leeuwen 2007)",
                                                    "Yale Bright Star Catalogue (1991)",
                                                    "SIMBAD (CDS)"};
    res.provenance.source = kSources[int(star.astrometry)];
    if (o.sidereal != SiderealMode::Tropical) {
        auto s = impl_->sidereal_shift(o, jd_tt);
        if (!s)
            return s.error();
        res.ayanamsa_deg = s.value();
    }
    return res;
}

Result<CalcResult> Engine::calc_star_ut(size_t star_index, double jd_ut1, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    return calc_star(star_index, impl_->ut1_to_tt(jd_ut1), o);
}

Result<CalcResult> Engine::calc_orbit_point_ut(int id, OrbitPoint point, OrbitElements elements,
                                               double jd_ut1, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    return calc_orbit_point(id, point, elements, impl_->ut1_to_tt(jd_ut1), o);
}

void Engine::set_delta_t_model(const time::DeltaTModel* model) {
    if (impl_) {
        impl_->forget_observers(); // topocentric sites rotate with UT1
        impl_->delta_t = model;
    }
}

std::string_view Engine::source() const {
    return impl_ ? std::string_view(impl_->source->description) : std::string_view();
}

} // namespace prometheia
