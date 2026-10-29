// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia_segments_roots (docs/C_API.md, "Roots on segments") against an
// oracle that never sees a segment: the same function built from
// prometheia_calc_object, sampled on a uniform grid, every extremum between
// two grid points located, and each sign change bisected to 1e-9 d. Then the
// contract's edges: resume and counting against one call bit for bit, the
// half-open window, month handles back to back, targets placed on the
// steps at a coarse fit's bounds, forced grazes at stations, the refusals,
// and one call from several threads. Needs DE440 (PROMETHEIA_DE440, else
// ephe/linux_p1550p2650.440); SKIP when it is absent.
//
// c_segment_roots_report (doctest::skip) prints the time errors against
// err_days that docs/SEGMENTS.md quotes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <prometheia/prometheia.h>

#include <doctest/doctest.h>

namespace {

constexpr double kJ2026 = 2461041.5; // 2026-01-01 TT, near enough
constexpr double kYear = 365.25;
constexpr double kTolDays = 1e-8;    // asked of every call here
constexpr double kOracleDays = 1e-9; // the oracle's bisection
constexpr double kDeg = 180.0 / 3.14159265358979323846;

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

std::string de440_path() {
    return env_or("PROMETHEIA_DE440",
                  std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440");
}

prometheia_engine* open_engine() {
    if (::access(de440_path().c_str(), R_OK) != 0)
        return nullptr;
    prometheia_engine* e = nullptr;
    if (prometheia_engine_open(de440_path().c_str(), &e, nullptr) != PROMETHEIA_OK)
        return nullptr;
    return e;
}

bool is_position(int component) {
    return component == PROMETHEIA_SEGMENTS_LON || component == PROMETHEIA_SEGMENTS_LAT;
}

// One fitted object, with what the oracle needs to compute it afresh.
struct Fitted {
    prometheia_object object;
    prometheia_options opts;
    prometheia_segments* seg = nullptr;
    const prometheia_segments_layout* layout = nullptr;
};

// The fits of one test, closed with it.
class Fits {
public:
    explicit Fits(prometheia_engine* e) : e_(e) {}
    ~Fits() {
        for (const auto& f : all_)
            prometheia_segments_close(f->seg);
    }
    Fits(const Fits&) = delete;
    Fits& operator=(const Fits&) = delete;

    const Fitted* fit(int body, int coords, double from, double to, double tol_arcsec,
                      int sidereal = PROMETHEIA_SIDEREAL_TROPICAL) {
        auto f = std::make_unique<Fitted>();
        f->object = {PROMETHEIA_OBJECT_BODY, body, nullptr};
        prometheia_options_init(&f->opts);
        f->opts.sigma = 0;
        f->opts.coords = coords;
        f->opts.sidereal = sidereal;
        prometheia_error err;
        REQUIRE_MESSAGE(prometheia_segments_fit(e_, &f->object, from, to, &f->opts, tol_arcsec,
                                                &f->seg, &err) == PROMETHEIA_OK,
                        err.message);
        f->layout = prometheia_segments_get_layout(f->seg);
        all_.push_back(std::move(f));
        return all_.back().get();
    }

private:
    prometheia_engine* e_;
    std::vector<std::unique_ptr<Fitted>> all_;
};

using Term = std::vector<const Fitted*>; // handles back to back

struct Spec {
    std::string name;
    Term a, b; // b empty: no second term
    double ca, cb;
    int component;
    double target, target_rate, target_jd_tt, modulus;
    double from, to;
    double step;                                 // the oracle's grid, days
    std::vector<std::pair<Term, double>> more{}; // terms past two: roots_sum
};

// Every term of a Spec with its coefficient.
std::vector<std::pair<Term, double>> terms_of(const Spec& s) {
    std::vector<std::pair<Term, double>> out{{s.a, s.ca}};
    if (!s.b.empty())
        out.emplace_back(s.b, s.cb);
    out.insert(out.end(), s.more.begin(), s.more.end());
    return out;
}

// A Spec as prometheia_segments_sum, pointing into this object's arrays.
class Sum {
public:
    explicit Sum(const Spec& s) : Sum(terms_of(s), s) {}
    Sum(const std::vector<std::pair<Term, double>>& terms, const Spec& s) {
        handles_.reserve(terms.size());
        for (const auto& [term, c] : terms) {
            std::vector<const prometheia_segments*> h;
            for (const Fitted* f : term)
                h.push_back(f->seg);
            handles_.push_back(std::move(h));
            terms_.push_back({nullptr, 0, c});
        }
        for (size_t i = 0; i < terms_.size(); ++i) {
            terms_[i].handles = handles_[i].data();
            terms_[i].count = int(handles_[i].size());
        }
        std::memset(&f_, 0, sizeof f_);
        f_.terms = terms_.data();
        f_.nterms = int(terms_.size());
        f_.component = s.component;
        f_.target = s.target;
        f_.target_rate = s.target_rate;
        f_.target_jd_tt = s.target_jd_tt;
        f_.modulus = s.modulus;
    }
    Sum(const Sum&) = delete;
    Sum& operator=(const Sum&) = delete;
    const prometheia_segments_sum* get() const { return &f_; }
    prometheia_segments_sum& edit() { return f_; }
    prometheia_segments_term& term(size_t i) { return terms_[i]; }

private:
    std::vector<std::vector<const prometheia_segments*>> handles_;
    std::vector<prometheia_segments_term> terms_;
    prometheia_segments_sum f_;
};

// A Spec as the C struct, which points into this object's arrays.
class Function {
public:
    explicit Function(const Spec& s) {
        for (const Fitted* f : s.a)
            a_.push_back(f->seg);
        for (const Fitted* f : s.b)
            b_.push_back(f->seg);
        std::memset(&f_, 0, sizeof f_);
        f_.a = a_.data();
        f_.na = int(a_.size());
        f_.b = b_.empty() ? nullptr : b_.data();
        f_.nb = int(b_.size());
        f_.ca = s.ca;
        f_.cb = s.cb;
        f_.component = s.component;
        f_.target = s.target;
        f_.target_rate = s.target_rate;
        f_.target_jd_tt = s.target_jd_tt;
        f_.modulus = s.modulus;
    }
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;
    const prometheia_segments_function* get() const { return &f_; }
    prometheia_segments_function& edit() { return f_; }

private:
    std::vector<const prometheia_segments*> a_, b_;
    prometheia_segments_function f_;
};

// The contract's band: per term |c| x its largest tolerance (positions,
// deg) or largest measured rate residual (speeds, deg/day).
double band_of(const Spec& s) {
    auto term = [&](const Term& t) {
        double worst = 0.0;
        for (const Fitted* f : t)
            worst = std::max(worst, is_position(s.component) ? f->layout->tolerance_arcsec
                                                             : f->layout->err_rate_arcsec_per_day);
        return worst / 3600.0;
    };
    double band = 0.0;
    for (const auto& [t, c] : terms_of(s))
        band += std::abs(c) * term(t);
    return band;
}

// The modulus the contract applies: LON's 0 means 360; other components
// have none.
double modulus_of(const Spec& s) {
    if (s.component != PROMETHEIA_SEGMENTS_LON)
        return 0.0;
    return s.modulus > 0.0 ? s.modulus : 360.0;
}

// Into (-M/2, M/2].
double wrap(double v, double m) {
    if (m <= 0.0)
        return v;
    double r = v - m * std::floor(v / m);
    if (r > m / 2)
        r -= m;
    return r;
}

double component_of(const prometheia_result& r, int component) {
    switch (component) {
    case PROMETHEIA_SEGMENTS_LON:
        return r.lon_deg;
    case PROMETHEIA_SEGMENTS_LAT:
        return r.lat_deg;
    case PROMETHEIA_SEGMENTS_LON_SPEED:
        return r.lon_speed;
    default:
        return r.lat_speed;
    }
}

struct OracleRoot {
    double jd;
    double slope;
};

// The function from prometheia_calc_object alone. Every handle of a term is
// the same object and options, so the first one stands for the term.
class Oracle {
public:
    Oracle(prometheia_engine* e, const Spec& s)
        : e_(e), s_(s), m_(modulus_of(s)), terms_(terms_of(s)) {}

    // f unwrapped and df/dt.
    void at(double t, double* value, double* rate) const {
        double v = 0.0, r = 0.0;
        for (const auto& [handles, c] : terms_)
            term(handles.front(), c, t, &v, &r);
        *value = v - (s_.target + s_.target_rate * (t - s_.target_jd_tt));
        *rate = r - s_.target_rate;
    }

    // Roots in [from, to).
    std::vector<OracleRoot> roots() const {
        std::vector<OracleRoot> out;
        const long n = static_cast<long>(std::ceil((s_.to - s_.from) / s_.step));
        Sample prev = sample(s_.from);
        for (long k = 1; k <= n; ++k) {
            const Sample cur = sample(k == n ? s_.to : s_.from + k * s_.step);
            scan(prev, cur, out);
            prev = cur;
        }
        while (!out.empty() && out.back().jd >= s_.to)
            out.pop_back();
        return out;
    }

private:
    struct Sample {
        double t, g, rate;
    };

    void term(const Fitted* f, double c, double t, double* v, double* r) const {
        prometheia_result res;
        REQUIRE(prometheia_calc_object(e_, &f->object, t, &f->opts, &res, nullptr) ==
                PROMETHEIA_OK);
        *v += c * component_of(res, s_.component);
        if (is_position(s_.component)) {
            *r += c * (s_.component == PROMETHEIA_SEGMENTS_LON ? res.lon_speed : res.lat_speed);
        } else {
            const double h = 1e-3;
            prometheia_result lo, hi;
            REQUIRE(prometheia_calc_object(e_, &f->object, t - h, &f->opts, &lo, nullptr) ==
                    PROMETHEIA_OK);
            REQUIRE(prometheia_calc_object(e_, &f->object, t + h, &f->opts, &hi, nullptr) ==
                    PROMETHEIA_OK);
            *r += c * (component_of(hi, s_.component) - component_of(lo, s_.component)) / (2 * h);
        }
    }

    Sample sample(double t) const {
        double v, r;
        at(t, &v, &r);
        return {t, wrap(v, m_), r};
    }

    // An extremum between two samples splits the step, so a pair of roots
    // closer than the grid is still seen.
    void scan(const Sample& p0, const Sample& p1, std::vector<OracleRoot>& out) const {
        if (p0.rate != 0.0 && p1.rate != 0.0 && (p0.rate > 0) != (p1.rate > 0)) {
            double lo = p0.t, hi = p1.t;
            const bool rising = p0.rate > 0;
            while (hi - lo > kOracleDays) {
                const double mid = 0.5 * (lo + hi);
                if (mid <= lo || mid >= hi)
                    break;
                double v, r;
                at(mid, &v, &r);
                ((r > 0) == rising ? lo : hi) = mid;
            }
            const Sample pe = sample(0.5 * (lo + hi));
            crossing(p0, pe, out);
            crossing(pe, p1, out);
            return;
        }
        crossing(p0, p1, out);
    }

    void crossing(const Sample& p0, const Sample& p1, std::vector<OracleRoot>& out) const {
        if (m_ > 0.0 && std::abs(p1.g - p0.g) > m_ / 2)
            return; // a wrap, never a root
        if ((p0.g < 0) == (p1.g < 0))
            return;
        double lo = p0.t, hi = p1.t;
        const bool below = p0.g < 0;
        while (hi - lo > kOracleDays) {
            const double mid = 0.5 * (lo + hi);
            if (mid <= lo || mid >= hi)
                break;
            ((sample(mid).g < 0) == below ? lo : hi) = mid;
        }
        const double t = 0.5 * (lo + hi);
        out.push_back({t, sample(t).rate});
    }

    prometheia_engine* e_;
    const Spec& s_;
    double m_;
    std::vector<std::pair<Term, double>> terms_;
};

std::vector<prometheia_segments_root> roots_of(const prometheia_segments_function* f, double from,
                                               double to, double tol_days = kTolDays) {
    std::vector<prometheia_segments_root> out(4096);
    int count = -1, unresolved = -1;
    double resume = 0.0;
    prometheia_error err;
    const prometheia_status st = prometheia_segments_roots(
        f, from, to, tol_days, out.data(), int(out.size()), &count, &resume, &unresolved, &err);
    REQUIRE_MESSAGE(st == PROMETHEIA_OK, err.message);
    CHECK(unresolved == 0); // every root found
    REQUIRE(count >= 0);
    REQUIRE(count < int(out.size()));
    CHECK(std::isnan(resume));
    out.resize(count);
    return out;
}

std::vector<prometheia_segments_root> roots_of(const prometheia_segments_sum* f, double from,
                                               double to, double tol_days = kTolDays) {
    std::vector<prometheia_segments_root> out(4096);
    int count = -1, unresolved = -1;
    double resume = 0.0;
    prometheia_error err;
    const prometheia_status st = prometheia_segments_roots_sum(
        f, from, to, tol_days, out.data(), int(out.size()), &count, &resume, &unresolved, &err);
    REQUIRE_MESSAGE(st == PROMETHEIA_OK, err.message);
    CHECK(unresolved == 0); // every root found
    REQUIRE(count >= 0);
    REQUIRE(count < int(out.size()));
    CHECK(std::isnan(resume));
    out.resize(count);
    return out;
}

// Two terms or fewer through prometheia_segments_roots, more through
// prometheia_segments_roots_sum.
std::vector<prometheia_segments_root> roots_of(const Spec& s) {
    if (!s.more.empty()) {
        const Sum f(s);
        return roots_of(f.get(), s.from, s.to);
    }
    const Function f(s);
    return roots_of(f.get(), s.from, s.to);
}

// Bit for bit, field by field (the struct has padding).
bool same(const prometheia_segments_root& x, const prometheia_segments_root& y) {
    return std::memcmp(&x.jd_tt, &y.jd_tt, sizeof(double)) == 0 &&
           std::memcmp(&x.slope, &y.slope, sizeof(double)) == 0 &&
           std::memcmp(&x.err_days, &y.err_days, sizeof(double)) == 0 && x.tangent == y.tangent;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

struct Measured {
    int oracle = 0, roots = 0, tangent = 0;
    std::vector<double> dt;       // days, each crossing against the oracle's
    std::vector<double> err_days; // as the root declared it
};

// The roots against the oracle's: each crossing within its err_days of a
// distinct oracle root, with its sign of slope, and err_days the contract's
// band/|slope| + tol_days; each oracle root within reach of a reported
// root. A tangent root needs no oracle partner (the truth may graze without
// crossing), but an oracle root may pair with it.
Measured compare(prometheia_engine* e, const Spec& s) {
    INFO(s.name);
    const std::vector<prometheia_segments_root> got = roots_of(s);
    const std::vector<OracleRoot> want = Oracle(e, s).roots();
    const double band = band_of(s);
    // Positions: the band bounds the angle, and longitude carries 1/cos(lat)
    // of it. Speeds: the band is a residual measured at check points, not a
    // bound.
    const double k = is_position(s.component) ? 1.1 : 2.0;
    const double slack = 4 * kOracleDays;
    auto nearest = [](const auto& v, double t, auto jd_of) {
        size_t best = v.size();
        for (size_t i = 0; i < v.size(); ++i)
            if (best == v.size() || std::abs(jd_of(v[i]) - t) < std::abs(jd_of(v[best]) - t))
                best = i;
        return best;
    };

    Measured m;
    m.oracle = int(want.size());
    m.roots = int(got.size());
    std::vector<int> taken(want.size(), 0);
    for (size_t i = 0; i < got.size(); ++i) {
        const prometheia_segments_root& r = got[i];
        INFO("root " << i << " at " << r.jd_tt << ", err_days " << r.err_days);
        CHECK(r.jd_tt >= s.from);
        CHECK(r.jd_tt < s.to);
        CHECK(std::isfinite(r.err_days));
        CHECK(r.err_days >= kTolDays);
        if (i > 0)
            CHECK(r.jd_tt > got[i - 1].jd_tt);
        if (r.tangent) {
            ++m.tangent;
            continue;
        }
        const double declared = band / std::abs(r.slope) + kTolDays;
        CHECK(std::abs(r.err_days - declared) <= 1e-9 * declared);
        const size_t j = nearest(want, r.jd_tt, [](const OracleRoot& o) { return o.jd; });
        REQUIRE_MESSAGE(j < want.size(), "a crossing where the oracle has none");
        const double dt = std::abs(r.jd_tt - want[j].jd);
        CHECK_MESSAGE(dt <= k * r.err_days + slack,
                      "dt " << dt << " d, oracle slope " << want[j].slope);
        CHECK((r.slope > 0) == (want[j].slope > 0));
        CHECK(taken[j] == 0);
        ++taken[j];
        m.dt.push_back(dt);
        m.err_days.push_back(r.err_days);
    }
    for (const OracleRoot& o : want) {
        INFO("oracle root at " << o.jd << ", slope " << o.slope);
        const size_t i =
            nearest(got, o.jd, [](const prometheia_segments_root& r) { return r.jd_tt; });
        REQUIRE_MESSAGE(i < got.size(), "an oracle root with no root reported");
        const double dt = std::abs(got[i].jd_tt - o.jd);
        CHECK(dt <= (got[i].tangent ? 2 : 1) * k * got[i].err_days + slack);
    }
    return m;
}

// The specs that compare(), the report and the threads case share.
std::vector<Spec> specs(prometheia_engine* e, Fits& fits) {
    const double y0 = kJ2026, y1 = kJ2026 + kYear;
    const int ecl = PROMETHEIA_COORDS_ECLIPTIC, equ = PROMETHEIA_COORDS_EQUATORIAL;
    const Fitted* moon = fits.fit(PROMETHEIA_MOON, ecl, y0, y1, 0.001);
    const Fitted* sun = fits.fit(PROMETHEIA_SUN, ecl, y0, y1, 0.001);
    const Fitted* moon_eq = fits.fit(PROMETHEIA_MOON, equ, y0, y1, 0.001);
    const Fitted* mercury = fits.fit(PROMETHEIA_MERCURY, ecl, y0, y1, 0.001);
    const Fitted* mars = fits.fit(PROMETHEIA_MARS, ecl, y0, y0 + 2 * kYear, 0.001);
    const int lon = PROMETHEIA_SEGMENTS_LON, lat = PROMETHEIA_SEGMENTS_LAT;
    const int lon_speed = PROMETHEIA_SEGMENTS_LON_SPEED;
    // The obliquity of date, IAU 2006, as a line through 2026.
    const double t26 = (y0 - 2451545.0) / 36525.0;
    const double eps = 23.4392794444 - 46.836769 / 3600.0 * t26;
    const double eps_rate = -46.836769 / 3600.0 / 36525.0;
    // A target moving at 1 deg/day from a degree behind the Sun: f creeps,
    // and turns twice a year.
    prometheia_result sun0;
    REQUIRE(prometheia_calc_object(e, &sun->object, y0, &sun->opts, &sun0, nullptr) ==
            PROMETHEIA_OK);
    return {
        {"Moon sign ingresses", {moon}, {}, 1, 0, lon, 0, 0, 0, 30, y0, y1, 0.05},
        {"Sun-Moon phases", {moon}, {sun}, 1, -1, lon, 0, 0, 0, 90, y0, y1, 0.05},
        {"Mercury stations", {mercury}, {}, 1, 0, lon_speed, 0, 0, 0, 0, y0, y1, 0.25},
        {"Mars stations", {mars}, {}, 1, 0, lon_speed, 0, 0, 0, 0, y0, y0 + 2 * kYear, 0.25},
        {"Moon declination 0", {moon_eq}, {}, 1, 0, lat, 0, 0, 0, 0, y0, y1, 0.05},
        {"Sun/Moon midpoint at 15, M=180",
         {moon},
         {sun},
         0.5,
         0.5,
         lon,
         15,
         0,
         0,
         180,
         y0,
         y1,
         0.05},
        {"Moon out of bounds north", {moon_eq}, {}, 1, 0, lat, eps, eps_rate, y0, 0, y0, y1, 0.05},
        {"Moon out of bounds south", {moon_eq}, {}, -1, 0, lat, eps, eps_rate, y0, 0, y0, y1, 0.05},
        {"Sun against a target at 1 deg/day",
         {sun},
         {},
         1,
         0,
         lon,
         sun0.lon_deg - 1.0,
         1.0,
         y0,
         0,
         y0,
         y1,
         0.25},
    };
}

// The twelve months of 2026 as separate fits, each ending where the next
// starts.
Term months(Fits& fits, int body, double tol_arcsec) {
    Term out;
    double edge[13];
    for (int k = 0; k <= 12; ++k)
        edge[k] = kJ2026 + k * (kYear / 12);
    for (int k = 0; k < 12; ++k)
        out.push_back(fits.fit(body, PROMETHEIA_COORDS_ECLIPTIC, edge[k], edge[k + 1], tol_arcsec));
    return out;
}

// Segment i's longitude, latitude and longitude rate at jd, by the
// documented layout and conversion: also at its right end, where eval would
// use segment i + 1.
struct Spherical {
    double lon, lat, lon_speed;
};

Spherical segment_at(const prometheia_segments_layout& l, int i, double jd) {
    const double u = (jd - l.mid_jd_tt[i]) / l.half_span_days[i];
    double p[3], v[3];
    for (int c = 0; c < 3; ++c)
        for (int s = 0; s < 2; ++s) {
            const double* cf = (s ? l.dcoef : l.coef) + (3 * i + c) * l.stride;
            double b1 = 0.0, b2 = 0.0;
            for (int k = l.stride - 1; k >= 1; --k) {
                const double b = 2.0 * u * b1 - b2 + cf[k];
                b2 = b1;
                b1 = b;
            }
            (s ? v : p)[c] = (u * b1 - b2 + cf[0]) / (s ? l.half_span_days[i] : 1.0);
        }
    const double rho2 = p[0] * p[0] + p[1] * p[1];
    double lon = std::atan2(p[1], p[0]) * kDeg;
    if (lon < 0.0)
        lon += 360.0;
    return {lon, std::atan2(p[2], std::sqrt(rho2)) * kDeg,
            (p[0] * v[1] - p[1] * v[0]) / rho2 * kDeg};
}

prometheia_segments_point eval_at(const Fitted* f, double jd) {
    prometheia_segments_point p;
    REQUIRE(prometheia_segments_eval(f->seg, jd, &p, nullptr) == PROMETHEIA_OK);
    return p;
}

// Stations of a fit, by bisection on its own lon_speed.
std::vector<double> stations(const Fitted* f) {
    std::vector<double> out;
    const double from = f->layout->jd_tt_start, to = f->layout->jd_tt_end;
    double t0 = from, v0 = eval_at(f, t0).lon_speed;
    for (double t1 = from + 0.5; t1 <= to; t1 += 0.5) {
        const double v1 = eval_at(f, t1).lon_speed;
        if ((v0 > 0) != (v1 > 0)) {
            double lo = t0, hi = t1;
            while (hi - lo > 1e-10 && 0.5 * (lo + hi) > lo && 0.5 * (lo + hi) < hi) {
                const double mid = 0.5 * (lo + hi);
                ((eval_at(f, mid).lon_speed > 0) == (v0 > 0) ? lo : hi) = mid;
            }
            out.push_back(0.5 * (lo + hi));
        }
        t0 = t1;
        v0 = v1;
    }
    return out;
}

} // namespace

TEST_CASE("c_segment_roots_against_calc") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        for (const Spec& s : specs(e, fits)) {
            INFO(s.name);
            const Measured m = compare(e, s);
            CHECK(m.oracle > 0);
            // A year of real motion grazes nothing within 0.001".
            CHECK(m.tangent == 0);
        }
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_resume") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const std::vector<Spec> all = specs(e, fits);
        for (const Spec& s : {all[0], all[1]}) {
            INFO(s.name);
            const Function f(s);
            const std::vector<prometheia_segments_root> once = roots_of(f.get(), s.from, s.to);
            REQUIRE(once.size() > 10);

            // Counting: capacity 0, out NULL.
            int count = -1;
            double resume = 0.0;
            int unresolved = -1;
            REQUIRE(prometheia_segments_roots(f.get(), s.from, s.to, kTolDays, nullptr, 0, &count,
                                              &resume, &unresolved, nullptr) == PROMETHEIA_OK);
            CHECK(unresolved == 0);
            CHECK(count == int(once.size()));

            // Capacity 1, resumed until the resume point is NaN.
            std::vector<prometheia_segments_root> looped;
            double start = s.from;
            for (int calls = 0; calls < 10000; ++calls) {
                prometheia_segments_root r[2];
                std::memset(r, 0xff, sizeof r);
                count = -1;
                REQUIRE(prometheia_segments_roots(f.get(), start, s.to, kTolDays, r, 1, &count,
                                                  &resume, nullptr, nullptr) == PROMETHEIA_OK);
                REQUIRE(count >= 0);
                REQUIRE(count <= 1);
                // Capacity is respected: the second slot is untouched.
                unsigned char untouched[sizeof r[1]];
                std::memset(untouched, 0xff, sizeof untouched);
                CHECK(std::memcmp(&r[1], untouched, sizeof untouched) == 0);
                looped.insert(looped.end(), r, r + count);
                if (std::isnan(resume))
                    break;
                CHECK(count == 1);
                CHECK(resume > start);
                // The first root not written is the next one of the big call.
                if (looped.size() < once.size())
                    CHECK(resume == once[looped.size()].jd_tt);
                start = resume;
            }
            REQUIRE(looped.size() == once.size());
            int differ = 0;
            for (size_t i = 0; i < once.size(); ++i)
                differ += !same(looped[i], once[i]);
            CHECK(differ == 0);

            // [start, end): a root at end is the next window's.
            const prometheia_segments_root& r = once[once.size() / 2];
            const std::vector<prometheia_segments_root> before = roots_of(f.get(), s.from, r.jd_tt);
            const std::vector<prometheia_segments_root> after = roots_of(f.get(), r.jd_tt, s.to);
            CHECK(before.size() == once.size() / 2);
            REQUIRE(!after.empty());
            CHECK(same(after.front(), r));
            CHECK(before.size() + after.size() == once.size());
        }
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_month_handles") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const double y0 = kJ2026, y1 = kJ2026 + kYear;
        const Fitted* year = fits.fit(PROMETHEIA_MOON, PROMETHEIA_COORDS_ECLIPTIC, y0, y1, 0.001);
        const Term moon = months(fits, PROMETHEIA_MOON, 0.001);
        const Term sun = months(fits, PROMETHEIA_SUN, 0.001);
        const Spec chunked{"Moon sign ingresses, 12 handles",
                           moon,
                           {},
                           1,
                           0,
                           PROMETHEIA_SEGMENTS_LON,
                           0,
                           0,
                           0,
                           30,
                           y0,
                           y1,
                           0.05};
        const Measured m = compare(e, chunked);
        CHECK(m.roots == m.oracle);
        CHECK(m.tangent == 0);
        Spec whole = chunked;
        whole.a = {year};
        const std::vector<prometheia_segments_root> one = roots_of(whole),
                                                    twelve = roots_of(chunked);
        REQUIRE(one.size() == twelve.size());
        for (size_t i = 0; i < one.size(); ++i)
            CHECK(std::abs(one[i].jd_tt - twelve[i].jd_tt) <= one[i].err_days + twelve[i].err_days);

        // Both terms in months, and a window across the joins.
        const Spec phases{"Sun-Moon phases, 12 handles each",
                          moon,
                          sun,
                          1,
                          -1,
                          PROMETHEIA_SEGMENTS_LON,
                          0,
                          0,
                          0,
                          90,
                          y0 + 20,
                          y1 - 20,
                          0.05};
        const Measured p = compare(e, phases);
        CHECK(p.roots == p.oracle);
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_on_bounds") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const double y0 = kJ2026, y1 = kJ2026 + kYear;
        const Fitted* moon = fits.fit(PROMETHEIA_MOON, PROMETHEIA_COORDS_ECLIPTIC, y0, y1, 1.0);
        const prometheia_segments_layout& l = *moon->layout;
        REQUIRE(l.count > 1);

        // The ingresses at 1": against the oracle, one each.
        const Spec ingress{"Moon sign ingresses at 1\"",
                           {moon},
                           {},
                           1,
                           0,
                           PROMETHEIA_SEGMENTS_LON,
                           0,
                           0,
                           0,
                           30,
                           y0,
                           y1,
                           0.05};
        const Measured m = compare(e, ingress);
        CHECK(m.roots == m.oracle);
        CHECK(m.tangent == 0);

        // A target on the step at every bound. The function is made
        // continuous through the middle of the step, so it passes halfway
        // once, at the bound. Longitude, fast and monotone, also passes the
        // step's two ends once each, close by, with one root in the window;
        // latitude and speed, which may turn within it (the speed near
        // apogee), are asked halfway only, for one root near the bound.
        int placed = 0;
        for (int component :
             {PROMETHEIA_SEGMENTS_LON, PROMETHEIA_SEGMENTS_LAT, PROMETHEIA_SEGMENTS_LON_SPEED}) {
            for (int i = 1; i < l.count; ++i) {
                const double b = l.bounds_jd_tt[i];
                const Spherical left = segment_at(l, i - 1, b);
                const prometheia_segments_point right = eval_at(moon, b);
                double lv = left.lon, rv = right.lon_deg;
                if (component == PROMETHEIA_SEGMENTS_LAT) {
                    lv = left.lat;
                    rv = right.lat_deg;
                } else if (component == PROMETHEIA_SEGMENTS_LON_SPEED) {
                    lv = left.lon_speed;
                    rv = right.lon_speed;
                }
                const double jump =
                    component == PROMETHEIA_SEGMENTS_LON ? std::remainder(rv - lv, 360.0) : rv - lv;
                std::vector<double> targets = {lv + 0.5 * jump};
                if (component == PROMETHEIA_SEGMENTS_LON) {
                    targets.push_back(lv);
                    targets.push_back(lv + jump);
                }
                for (double target : targets) {
                    INFO("component " << component << ", bound " << b << ", left " << lv
                                      << ", jump " << jump << ", target " << target);
                    Spec s = ingress;
                    s.component = component;
                    s.modulus = 0;
                    s.target = target;
                    const Function f(s);
                    const std::vector<prometheia_segments_root> got =
                        roots_of(f.get(), std::max(y0, b - 0.3), std::min(y1, b + 0.3), 1e-9);
                    int near = 0;
                    for (const auto& r : got)
                        near += std::abs(r.jd_tt - b) <= 0.01;
                    CHECK(near == 1);
                    if (component == PROMETHEIA_SEGMENTS_LON) {
                        REQUIRE(got.size() == 1);
                        CHECK(!got[0].tangent);
                        CHECK(std::abs(got[0].jd_tt - b) <= 1e-4);
                    }
                    ++placed;
                }
            }
        }
        CHECK(placed == 5 * (l.count - 1));
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_grazes") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const Fitted* bodies[] = {
            fits.fit(PROMETHEIA_MERCURY, PROMETHEIA_COORDS_ECLIPTIC, kJ2026, kJ2026 + kYear, 0.001),
            fits.fit(PROMETHEIA_MARS, PROMETHEIA_COORDS_ECLIPTIC, kJ2026, kJ2026 + 2 * kYear,
                     0.001)};
        int seen = 0;
        for (const Fitted* f : bodies) {
            const double band = f->layout->tolerance_arcsec / 3600.0;
            for (double ts : stations(f)) {
                const double ext = eval_at(f, ts).lon_deg;
                const double curv =
                    (eval_at(f, ts + 1e-3).lon_speed - eval_at(f, ts - 1e-3).lon_speed) / 2e-3;
                // frac > 0: beyond the extremum, where f does not reach.
                const double beyond = curv < 0 ? 1.0 : -1.0;
                const double reach = 2 * std::sqrt(2 * band / std::abs(curv)) + 1e-6;
                const double from = std::max(f->layout->jd_tt_start, ts - 3);
                const double to = std::min(f->layout->jd_tt_end, ts + 3);
                INFO("station " << ts << ", lon " << ext << ", curvature " << curv);
                Spec s{"graze", {f}, {}, 1, 0, PROMETHEIA_SEGMENTS_LON, 0, 0, 0, 0, from, to, 0};
                auto at = [&](double frac, double tol = kTolDays) {
                    s.target = ext + beyond * frac * band;
                    const Function fn(s);
                    return roots_of(fn.get(), from, to, tol);
                };

                // Three bands short: two plain crossings, one each way.
                std::vector<prometheia_segments_root> got = at(-3);
                REQUIRE(got.size() == 2);
                CHECK(!got[0].tangent);
                CHECK(!got[1].tangent);
                CHECK(got[0].slope * got[1].slope < 0);

                // Half a band short: a tangent pair, or one tangent root.
                got = at(-0.5);
                CHECK((got.size() == 1 || got.size() == 2));
                // f'' here from the fit's rate, by differences.
                const double declared = std::sqrt(2 * band / std::abs(curv)) + kTolDays;
                for (const auto& r : got) {
                    CHECK(r.tangent);
                    CHECK(std::abs(r.jd_tt - ts) <= reach);
                    CHECK(std::abs(r.err_days - declared) <= 0.05 * declared);
                }

                // Half a band beyond, not crossing: exactly one tangent root
                // at the extremum, its slope below any crossing's at the band.
                got = at(0.5);
                REQUIRE(got.size() == 1);
                CHECK(got[0].tangent);
                CHECK(std::abs(got[0].jd_tt - ts) <= reach);
                CHECK(std::abs(got[0].slope) < std::sqrt(2 * band * std::abs(curv)));
                CHECK(std::abs(got[0].err_days - declared) <= 0.05 * declared);

                // Three bands beyond: nothing.
                CHECK(at(3).empty());

                // A coarse tol_days moves a root, never the count or a flag.
                for (double frac : {-3.0, -0.5, 0.5, 3.0}) {
                    INFO("frac " << frac);
                    const std::vector<prometheia_segments_root> fine = at(frac, 1e-9),
                                                                coarse = at(frac, 0.5);
                    REQUIRE(coarse.size() == fine.size());
                    for (size_t i = 0; i < fine.size(); ++i)
                        CHECK(coarse[i].tangent == fine[i].tangent);
                }
                ++seen;
            }
        }
        CHECK(seen >= 6); // Mercury's of 2026, Mars's of 2027
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_refusals") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const double y0 = kJ2026;
        const int ecl = PROMETHEIA_COORDS_ECLIPTIC;
        const Fitted* moon = fits.fit(PROMETHEIA_MOON, ecl, y0, y0 + 30, 0.001);
        const Fitted* sun = fits.fit(PROMETHEIA_SUN, ecl, y0, y0 + 10, 0.001);
        const Fitted* sun_eq =
            fits.fit(PROMETHEIA_SUN, PROMETHEIA_COORDS_EQUATORIAL, y0, y0 + 30, 0.001);
        const Fitted* sun_lahiri =
            fits.fit(PROMETHEIA_SUN, ecl, y0, y0 + 30, 0.001, PROMETHEIA_SIDEREAL_LAHIRI);
        const Fitted* moon_next = fits.fit(PROMETHEIA_MOON, ecl, y0 + 30, y0 + 60, 0.001);
        const Fitted* moon_later = fits.fit(PROMETHEIA_MOON, ecl, y0 + 31, y0 + 60, 0.001);
        const Fitted* moon_overlap = fits.fit(PROMETHEIA_MOON, ecl, y0 + 29, y0 + 60, 0.001);
        const Fitted* moon_eq =
            fits.fit(PROMETHEIA_MOON, PROMETHEIA_COORDS_EQUATORIAL, y0 + 30, y0 + 60, 0.001);
        const Spec base{"", {moon}, {}, 1, 0, PROMETHEIA_SEGMENTS_LON, 0, 0, 0, 30, y0, y0 + 30, 0};
        prometheia_segments_root out[8];
        int count = 0;
        double resume = 0.0;
        prometheia_error err;
        auto call = [&](const Spec& s, double from, double to, double tol = kTolDays,
                        int capacity = 8) {
            const Function f(s);
            err.message[0] = '\0';
            return prometheia_segments_roots(f.get(), from, to, tol, out, capacity, &count, &resume,
                                             nullptr, &err);
        };
        REQUIRE(call(base, y0, y0 + 30) == PROMETHEIA_OK);
        CHECK(err.message[0] == '\0');

        // COVERAGE: the window outside a term's span, either term.
        CHECK(call(base, y0 + 40, y0 + 50) == PROMETHEIA_ERROR_COVERAGE);
        CHECK(call(base, y0 - 1, y0 + 5) == PROMETHEIA_ERROR_COVERAGE);
        CHECK(call(base, y0 + 25, y0 + 30.5) == PROMETHEIA_ERROR_COVERAGE);
        CHECK(std::strlen(err.message) > 0);
        Spec two = base;
        two.b = {sun};
        two.cb = -1;
        CHECK(call(two, y0, y0 + 10) == PROMETHEIA_OK);
        CHECK(call(two, y0, y0 + 20) == PROMETHEIA_ERROR_COVERAGE);
        Spec joined = base;
        joined.a = {moon, moon_next};
        CHECK(call(joined, y0 + 20, y0 + 40) == PROMETHEIA_OK);
        CHECK(call(joined, y0 + 50, y0 + 61) == PROMETHEIA_ERROR_COVERAGE);

        // ARGUMENT: handles not back to back.
        for (const Term& t :
             {Term{moon, moon_later}, Term{moon, moon_overlap}, Term{moon_next, moon}}) {
            Spec s = base;
            s.a = t;
            CHECK(call(s, y0 + 20, y0 + 25) == PROMETHEIA_ERROR_ARGUMENT);
        }
        // Fitted differently: coordinates, zodiac, across terms and within one.
        for (const Fitted* other : {sun_eq, sun_lahiri}) {
            two.b = {other};
            CHECK(call(two, y0, y0 + 10) == PROMETHEIA_ERROR_ARGUMENT);
            CHECK(std::strlen(err.message) > 0);
        }
        joined.a = {moon, moon_eq};
        CHECK(call(joined, y0 + 20, y0 + 25) == PROMETHEIA_ERROR_ARGUMENT);
        // The window, the tolerance, the capacity.
        CHECK(call(base, y0 + 5, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(call(base, y0 + 6, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(call(base, std::nan(""), y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        for (double tol : {0.0, -1e-8, std::nan(""), double(INFINITY)})
            CHECK(call(base, y0, y0 + 5, tol) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(call(base, y0, y0 + 5, kTolDays, -1) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(call(base, y0, y0 + 5, kTolDays, 0) == PROMETHEIA_OK); // counting
        CHECK(count > 0);
        // Both coefficients 0.
        Spec zero = base;
        zero.ca = 0;
        CHECK(call(zero, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        zero.b = {sun};
        CHECK(call(zero, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);

        // The modulus: 360 ca/M and 360 cb/M whole, LON only, not negative.
        Spec mid = base;
        mid.b = {sun};
        mid.ca = mid.cb = 0.5;
        mid.modulus = 180;
        CHECK(call(mid, y0, y0 + 5) == PROMETHEIA_OK);
        mid.modulus = 360;
        CHECK(call(mid, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        mid.modulus = 0; // 0 is 360
        CHECK(call(mid, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        Spec odd = base;
        odd.modulus = 7;
        CHECK(call(odd, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        odd.modulus = -30;
        CHECK(call(odd, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);
        for (int component : {PROMETHEIA_SEGMENTS_LAT, PROMETHEIA_SEGMENTS_LON_SPEED,
                              PROMETHEIA_SEGMENTS_LAT_SPEED}) {
            Spec other = base;
            other.component = component;
            CHECK(call(other, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT); // modulus 30
            other.modulus = 0;
            CHECK(call(other, y0, y0 + 5) == PROMETHEIA_OK);
        }
        Spec bad = base;
        bad.component = 4;
        CHECK(call(bad, y0, y0 + 5) == PROMETHEIA_ERROR_ARGUMENT);

        // NULLs and empty terms.
        Function f(base);
        CHECK(prometheia_segments_roots(nullptr, y0, y0 + 5, kTolDays, out, 8, &count, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, nullptr, 8, &count, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, out, 8, nullptr, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, out, 8, &count, nullptr,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        f.edit().na = 0;
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, out, 8, &count, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        f.edit().na = 1;
        f.edit().a = nullptr;
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, out, 8, &count, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
        const prometheia_segments* none[1] = {nullptr};
        f.edit().a = none;
        CHECK(prometheia_segments_roots(f.get(), y0, y0 + 5, kTolDays, out, 8, &count, &resume,
                                        nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_threads") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const std::vector<Spec> all = specs(e, fits);
        std::vector<std::unique_ptr<Function>> fs;
        std::vector<std::vector<prometheia_segments_root>> want;
        for (const Spec& s : all) {
            fs.push_back(std::make_unique<Function>(s));
            want.push_back(roots_of(fs.back()->get(), s.from, s.to));
        }
        // Every thread runs every spec on the same handles; doctest's
        // assertions stay on this thread.
        std::vector<std::vector<std::vector<prometheia_segments_root>>> got(4);
        std::vector<int> failures(4, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (size_t i = 0; i < all.size(); ++i) {
                    std::vector<prometheia_segments_root> r(4096);
                    int count = 0;
                    double resume = 0.0;
                    failures[t] +=
                        prometheia_segments_roots(fs[i]->get(), all[i].from, all[i].to, kTolDays,
                                                  r.data(), int(r.size()), &count, &resume, nullptr,
                                                  nullptr) != PROMETHEIA_OK;
                    r.resize(std::max(count, 0));
                    got[t].push_back(std::move(r));
                }
            });
        for (auto& th : threads)
            th.join();
        for (int t = 0; t < 4; ++t) {
            CHECK(failures[t] == 0);
            REQUIRE(got[t].size() == want.size());
            for (size_t i = 0; i < want.size(); ++i) {
                REQUIRE(got[t][i].size() == want[i].size());
                int differ = 0;
                for (size_t k = 0; k < want[i].size(); ++k)
                    differ += !same(got[t][i][k], want[i][k]);
                CHECK(differ == 0);
            }
        }
    }
    prometheia_engine_close(e);
}

// Sums of more than two terms (prometheia_segments_roots_sum): a midpoint
// against a body and against a midpoint, modulo 180, where every branch of
// each midpoint is a root. The Sun crosses 0 Aries in March and the Moon
// every month, so each term's wrap falls inside the window.
TEST_CASE("c_segment_roots_sum_against_calc") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const double y0 = kJ2026, y1 = kJ2026 + kYear;
        const int ecl = PROMETHEIA_COORDS_ECLIPTIC, lon = PROMETHEIA_SEGMENTS_LON;
        const Fitted* sun = fits.fit(PROMETHEIA_SUN, ecl, y0, y1, 0.001);
        const Fitted* moon = fits.fit(PROMETHEIA_MOON, ecl, y0, y1, 0.001);
        const Fitted* mars = fits.fit(PROMETHEIA_MARS, ecl, y0, y1, 0.001);
        const Fitted* venus = fits.fit(PROMETHEIA_VENUS, ecl, y0, y1, 0.001);
        const Fitted* jupiter = fits.fit(PROMETHEIA_JUPITER, ecl, y0, y1, 0.001);
        const Spec three{"Sun/Moon midpoint = Mars",
                         {sun},
                         {moon},
                         0.5,
                         0.5,
                         lon,
                         0,
                         0,
                         0,
                         180,
                         y0,
                         y1,
                         0.05,
                         {{{mars}, -1.0}}};
        const Spec four{"Sun/Moon midpoint = Venus/Jupiter midpoint",
                        {sun},
                        {moon},
                        0.5,
                        0.5,
                        lon,
                        0,
                        0,
                        0,
                        180,
                        y0,
                        y1,
                        0.05,
                        {{{venus}, -0.5}, {{jupiter}, -0.5}}};
        for (const Spec& s : {three, four}) {
            const Measured m = compare(e, s);
            MESSAGE(s.name << ": " << m.roots << " roots, oracle " << m.oracle);
            CHECK(m.roots == m.oracle);
            CHECK(m.oracle > 5);
            CHECK(m.tangent == 0);
        }
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_sum_terms") {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        const double y0 = kJ2026, y1 = kJ2026 + 60;
        const int ecl = PROMETHEIA_COORDS_ECLIPTIC, lon = PROMETHEIA_SEGMENTS_LON;
        const int bodies[] = {PROMETHEIA_SUN,    PROMETHEIA_MOON,  PROMETHEIA_MERCURY,
                              PROMETHEIA_VENUS,  PROMETHEIA_MARS,  PROMETHEIA_JUPITER,
                              PROMETHEIA_SATURN, PROMETHEIA_URANUS};
        std::vector<const Fitted*> f;
        for (int b : bodies)
            f.push_back(fits.fit(b, ecl, y0, y1, 0.001));
        const Fitted *sun = f[0], *moon = f[1];
        const Spec base{"", {sun}, {}, 1, 0, lon, 0, 0, 0, 90, y0, y1, 0};
        using Terms = std::vector<std::pair<Term, double>>;
        auto roots = [&](const Terms& t, double modulus = 90) {
            Spec s = base;
            s.modulus = modulus;
            const Sum sum(t, s);
            return roots_of(sum.get(), y0, y1);
        };
        auto status = [&](const Terms& t, double modulus = 90) {
            Spec s = base;
            s.modulus = modulus;
            const Sum sum(t, s);
            prometheia_segments_root out[8];
            int count = 0;
            double resume = 0.0;
            prometheia_error err;
            return prometheia_segments_roots_sum(sum.get(), y0, y1, kTolDays, out, 8, &count,
                                                 &resume, nullptr, &err);
        };
        auto same_all = [](const std::vector<prometheia_segments_root>& x,
                           const std::vector<prometheia_segments_root>& y) {
            if (x.size() != y.size())
                return false;
            for (size_t i = 0; i < x.size(); ++i)
                if (!same(x[i], y[i]))
                    return false;
            return true;
        };

        // prometheia_segments_roots is the sum of its two terms.
        Spec phases = base;
        phases.a = {moon};
        phases.b = {sun};
        phases.cb = -1;
        const Function two(phases);
        const std::vector<prometheia_segments_root> wrapped = roots_of(two.get(), y0, y1);
        REQUIRE(wrapped.size() > 5);
        CHECK(same_all(wrapped, roots({{{moon}, 1.0}, {{sun}, -1.0}})));

        // Terms naming the same handles combine: half the Sun plus half the
        // Moon less the Sun is half the Moon less half the Sun.
        const auto merged = roots({{{sun}, 0.5}, {{moon}, 0.5}, {{sun}, -1.0}});
        REQUIRE(merged.size() > 3);
        CHECK(same_all(merged, roots({{{sun}, -0.5}, {{moon}, 0.5}})));
        // Nothing left: refused.
        CHECK(status({{{sun}, 1.0}, {{sun}, -1.0}}) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(status({{{sun}, 0.0}, {{moon}, 0.0}}) == PROMETHEIA_ERROR_ARGUMENT);
        // Each term's 360 c / M whole: a quarter of the Sun at M = 180 is not.
        CHECK(status({{{sun}, 0.25}, {{moon}, 0.75}}, 180) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(status({{{sun}, 0.5}, {{moon}, 0.25}}, 180) == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(status({{{sun}, 0.5}, {{moon}, 0.5}}, 180) == PROMETHEIA_OK);

        // Up to PROMETHEIA_SEGMENTS_MAX_TERMS terms, and at least one.
        Terms eight;
        for (const Fitted* x : f)
            eight.push_back({{x}, 1.0});
        REQUIRE(eight.size() == PROMETHEIA_SEGMENTS_MAX_TERMS);
        CHECK(status(eight, 0) == PROMETHEIA_OK);
        Terms nine = eight;
        nine.push_back({{moon}, 2.0});
        Spec s = base;
        {
            const Sum sum(nine, s);
            prometheia_segments_root out[8];
            int count = 0;
            double resume = 0.0;
            CHECK(prometheia_segments_roots_sum(sum.get(), y0, y1, kTolDays, out, 8, &count,
                                                &resume, nullptr,
                                                nullptr) == PROMETHEIA_ERROR_ARGUMENT);
        }
        Sum one({{{sun}, 1.0}}, s);
        prometheia_segments_root out[8];
        int count = 0;
        double resume = 0.0;
        auto call = [&]() {
            return prometheia_segments_roots_sum(one.get(), y0, y1, kTolDays, out, 8, &count,
                                                 &resume, nullptr, nullptr);
        };
        CHECK(call() == PROMETHEIA_OK);
        one.edit().nterms = 0;
        CHECK(call() == PROMETHEIA_ERROR_ARGUMENT);
        one.edit().nterms = 1;
        one.term(0).count = 0; // a term with no handles
        CHECK(call() == PROMETHEIA_ERROR_ARGUMENT);
        one.term(0).count = 1;
        const prometheia_segments* const* handles = one.term(0).handles;
        one.term(0).handles = nullptr;
        CHECK(call() == PROMETHEIA_ERROR_ARGUMENT);
        one.term(0).handles = handles;
        one.edit().terms = nullptr;
        CHECK(call() == PROMETHEIA_ERROR_ARGUMENT);
        CHECK(prometheia_segments_roots_sum(nullptr, y0, y1, kTolDays, out, 8, &count, &resume,
                                            nullptr, nullptr) == PROMETHEIA_ERROR_ARGUMENT);
    }
    prometheia_engine_close(e);
}

TEST_CASE("c_segment_roots_report" * doctest::skip()) {
    prometheia_engine* e = open_engine();
    if (!e) {
        MESSAGE("SKIP: DE440 not found");
        return;
    }
    {
        Fits fits(e);
        std::vector<Spec> all = specs(e, fits);
        const double y0 = kJ2026, y1 = kJ2026 + kYear;
        const Fitted* coarse = fits.fit(PROMETHEIA_MOON, PROMETHEIA_COORDS_ECLIPTIC, y0, y1, 1.0);
        all.push_back({"Moon sign ingresses at 1\"",
                       {coarse},
                       {},
                       1,
                       0,
                       PROMETHEIA_SEGMENTS_LON,
                       0,
                       0,
                       0,
                       30,
                       y0,
                       y1,
                       0.05});
        all.push_back({"Moon sign ingresses, 12 handles",
                       months(fits, PROMETHEIA_MOON, 0.001),
                       {},
                       1,
                       0,
                       PROMETHEIA_SEGMENTS_LON,
                       0,
                       0,
                       0,
                       30,
                       y0,
                       y1,
                       0.05});
        std::printf("tol_days %g (%.2f ms); dt = |root - oracle root|, crossings only\n", kTolDays,
                    kTolDays * 86400e3);
        std::printf("%-34s %6s %6s %4s %10s %10s %10s %10s %8s %8s\n", "case", "oracle", "roots",
                    "tan", "band", "err p50", "dt p50", "dt max", "dt/err", "dt/err");
        std::printf("%-34s %6s %6s %4s %10s %10s %10s %10s %8s %8s\n", "", "", "", "", "\" or \"/d",
                    "ms", "ms", "ms", "p50", "max");
        for (const Spec& s : all) {
            const Measured m = compare(e, s);
            std::vector<double> ratio;
            for (size_t i = 0; i < m.dt.size(); ++i)
                ratio.push_back(m.dt[i] / m.err_days[i]);
            std::printf("%-34s %6d %6d %4d %10.6f %10.4f %10.4f %10.4f %8.3f %8.3f\n",
                        s.name.c_str(), m.oracle, m.roots, m.tangent, band_of(s) * 3600.0,
                        percentile(m.err_days, 0.5) * 86400e3, percentile(m.dt, 0.5) * 86400e3,
                        percentile(m.dt, 1.0) * 86400e3, percentile(ratio, 0.5),
                        percentile(ratio, 1.0));
        }
    }
    prometheia_engine_close(e);
}
