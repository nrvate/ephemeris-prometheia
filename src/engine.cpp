// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::Engine: ephemeris sources, the apparent-place pipeline and
// frame output. See include/prometheia/engine.hpp and docs/ENGINE.md.
#include "prometheia/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

#include "prometheia/apparent.hpp"
#include "prometheia/catalog.hpp"
#include "prometheia/de.hpp"
#include "prometheia/forces.hpp"
#include "prometheia/kepler.hpp"
#include "prometheia/memo.hpp"
#include "prometheia/spk.hpp"

namespace prometheia {
namespace {

constexpr double kAuKm = 149597870.7; // IAU 2012 Resolution B2 (exact)
// Obliquity defining JPL's J2000 ecliptic frame (SBDB elements, Horizons,
// SPICE ECLIPJ2000): the IAU 1976 value, applied to the ICRF without bias.
constexpr double kJplEclipticObliquityArcsec = 84381.448;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kJ2000 = 2451545.0;
// Earth rotation rate in rad per UT1 day (the ERA rate, Circular 179).
constexpr double kEarthRotationRadPerDay = kTwoPi * 1.00273781191135448;
// Central-difference half step for rates (days).
constexpr double kSpeedStepDays = 1e-3;

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
    return 0.0;
}

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
        if (!ok_ || i >= entries_.size()) {
            for (int k = 0; k < 6; ++k)
                out[k] = 0.0;
            return;
        }
        eval(i, t, out);
    }

    size_t count() const override { return entries_.size(); }
    const double* mus() const override { return mus_.data(); }
    bool ok() const override { return ok_; }
    long sun_index() const override { return sun_index_; }

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
                    sun_index_ = long(entries_.size());
                entries_.push_back(Entry{id, {}});
                mus_.push_back(gm_or_builtin(source_, id));
            }
        }
        if (entries_.empty()) {
            fail("the ephemeris carries none of the perturbing masses at JD " + std::to_string(t) +
                 " (outside its coverage?)");
            return;
        }
        lo_ = hi_ = t; // no samples yet; extend() seeds both directions
        extend(kBlockDays);
        extend(-kBlockDays);
    }

    // Grows coverage by one block in the given direction (positive =
    // forward). The block boundary sample is shared with the previous
    // block (or, on the very first block, is the probe epoch itself).
    void extend(double days) {
        const double from = days > 0.0 ? hi_ : lo_;
        const double to = from + days;
        const int n = kBlockSamples;
        const double step = days / double(n);
        for (size_t b = 0; b < entries_.size(); ++b) {
            std::vector<TrajSample> pts;
            pts.reserve(size_t(n) + 1);
            for (int i = 0; i <= n; ++i) {
                const double tt = from + step * double(i);
                double st[6];
                auto r = source_->barycentric(entries_[b].id, tt, st);
                if (!r) {
                    fail(r.error().message);
                    return;
                }
                TrajSample p;
                p.t = tt;
                p.px = st[0] / kAuKm;
                p.py = st[1] / kAuKm;
                p.pz = st[2] / kAuKm;
                p.vx = st[3] / kAuKm;
                p.vy = st[4] / kAuKm;
                p.vz = st[5] / kAuKm;
                pts.push_back(p);
            }
            append_block(b, days, std::move(pts));
        }
        if (days > 0.0)
            hi_ = to;
        else
            lo_ = to;
    }

    // pts holds the new block: ascending in time for a forward block,
    // descending for a backward one, and always including the boundary
    // epoch shared with the existing samples (or the probe epoch, when
    // this is the very first block).
    void append_block(size_t b, double days, std::vector<TrajSample> pts) {
        std::vector<TrajSample>& s = entries_[b].samples;
        if (days < 0.0) {
            // Ascending again: far end first.
            std::reverse(pts.begin(), pts.end());
            // pts = [to, ..., lo_]: the shared boundary is the last sample.
            if (s.empty()) {
                s = std::move(pts);
            } else {
                s.insert(s.begin(), pts.begin(), pts.end() - 1);
            }
        } else {
            // pts = [hi_, ..., to]: the shared boundary is the first sample.
            s.insert(s.end(), s.empty() ? pts.begin() : pts.begin() + 1, pts.end());
        }
    }

    void eval(size_t i, double t, double out[6]) const {
        const std::vector<TrajSample>& s = entries_[i].samples;
        if (s.empty()) {
            for (int k = 0; k < 6; ++k)
                out[k] = 0.0;
            return;
        }
        if (t <= s.front().t) {
            copy_sample(s.front(), out);
            return;
        }
        if (t >= s.back().t) {
            copy_sample(s.back(), out);
            return;
        }
        size_t lo = 0, hi = s.size();
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (s[mid].t <= t)
                lo = mid;
            else
                hi = mid;
        }
        const TrajSample& a = s[lo];
        const TrajSample& b = s[lo + 1];
        const double dt = b.t - a.t;
        const double u = (t - a.t) / dt;
        const double u2 = u * u, u3 = u2 * u;
        const double h00 = 2 * u3 - 3 * u2 + 1;
        const double h10 = u3 - 2 * u2 + u;
        const double h01 = -2 * u3 + 3 * u2;
        const double h11 = u3 - u2;
        out[0] = h00 * a.px + h10 * dt * a.vx + h01 * b.px + h11 * dt * b.vx;
        out[1] = h00 * a.py + h10 * dt * a.vy + h01 * b.py + h11 * dt * b.vy;
        out[2] = h00 * a.pz + h10 * dt * a.vz + h01 * b.pz + h11 * dt * b.vz;
        const double d00 = (6 * u2 - 6 * u) / dt;
        const double d10 = 3 * u2 - 4 * u + 1;
        const double d01 = (-6 * u2 + 6 * u) / dt;
        const double d11 = 3 * u2 - 2 * u;
        out[3] = d00 * a.px + d10 * a.vx + d01 * b.px + d11 * b.vx;
        out[4] = d00 * a.py + d10 * a.vy + d01 * b.py + d11 * b.vy;
        out[5] = d00 * a.pz + d10 * a.vz + d01 * b.pz + d11 * b.vz;
    }

    static void copy_sample(const TrajSample& p, double out[6]) {
        out[0] = p.px;
        out[1] = p.py;
        out[2] = p.pz;
        out[3] = p.vx;
        out[4] = p.vy;
        out[5] = p.vz;
    }

    struct Entry {
        int id; // the NAIF id this table samples
        std::vector<TrajSample> samples;
    };

    Source* source_ = nullptr;
    bool ok_ = true;
    long sun_index_ = -1;
    bool built_ = false;
    std::string error_;
    double lo_ = 0.0, hi_ = 0.0;
    std::vector<Entry> entries_;
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
class SigmaTracks {
public:
    struct Column {
        double sigma; // 1-sigma of the element (catalog units)
        double h;     // finite-difference step used for it
        std::unique_ptr<WindowMemo<BarycentricForce>> plus, minus;
    };

    explicit SigmaTracks(std::vector<Column> columns) : columns_(std::move(columns)) {}

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
    std::vector<Column> columns_;
};

} // namespace

struct Engine::Impl {
    std::unique_ptr<Source> source;
    const time::DeltaTModel* delta_t = nullptr;
    time::ObservedDeltaT default_delta_t;
    double bias[9];
    double eps_j2000 = 0.0;
    EpochFrames cache[3];
    int cache_next = 0;

    // Small-body overlay: EPM1 catalogs, newest wins. Positions are
    // integrated on demand from the catalog's osculating elements with
    // the barycentric force model and memoized per body.
    std::vector<std::unique_ptr<catalog::Reader>> catalogs;
    PerturberSet perturbers;
    BarycentricForce force{&perturbers};
    std::unordered_map<uint64_t, std::unique_ptr<WindowMemo<BarycentricForce>>> small_bodies;
    std::unordered_map<uint64_t, std::unique_ptr<SigmaTracks>> sigma_tracks;
    std::string overlay_source_; // provenance for catalog bodies

    // Designations and proper names of every loaded catalog, lowercased
    // ASCII, merged newest-wins; values are the SPK-IDs calc() takes.
    std::unordered_map<std::string, uint64_t> name_index;

    double delta_t_seconds(double jd_tt) const {
        return (delta_t ? delta_t : &default_delta_t)->delta_t_seconds(jd_tt);
    }

    const EpochFrames& frames_at(double jd_tt, bool need_nutation) {
        for (EpochFrames& f : cache) {
            if (f.jd_tt == jd_tt) {
                if (need_nutation && !f.have_nutation)
                    add_nutation(f);
                return f;
            }
        }
        EpochFrames& f = cache[cache_next];
        cache_next = (cache_next + 1) % 3;
        f.jd_tt = jd_tt;
        f.have_nutation = false;
        double p[9];
        frames::mean_equator_of_date_matrix(jd_tt, p);
        matmul(p, bias, f.pb);
        f.eps_mean = frames::mean_obliquity(jd_tt);
        if (need_nutation)
            add_nutation(f);
        return f;
    }

    static void add_nutation(EpochFrames& f) {
        frames::nutation(f.jd_tt, f.dpsi, f.deps);
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

    // Observer barycentric state (km, km/day) at the given epoch.
    Result<void> observer(const CalcOptions& o, double jd_tt, double jd_tdb, double out[6]) {
        switch (o.center) {
        case Center::Barycentric:
            for (int i = 0; i < 6; ++i)
                out[i] = 0.0;
            return {};
        case Center::Heliocentric:
            return source->barycentric(body::kSun, jd_tdb, out);
        case Center::Geocentric:
            return source->barycentric(body::kEarth, jd_tdb, out);
        case Center::Topocentric: {
            auto r = source->barycentric(body::kEarth, jd_tdb, out);
            if (!r)
                return r;
            const EpochFrames& f = frames_at(jd_tt, true);
            const double jd_ut1 = jd_tt - delta_t_seconds(jd_tt) / 86400.0;
            const double gast = frames::gast_rad(jd_ut1, jd_tt);
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
    Result<void> body_barycentric(int id, double jd_tdb, double out[6], bool* from_catalog) {
        auto r = source->barycentric(id, jd_tdb, out);
        if (r.ok()) {
            if (from_catalog)
                *from_catalog = false;
            return r;
        }
        if (r.error().code != ErrorCode::NotFound)
            return r;
        if (from_catalog)
            *from_catalog = true;
        return small_body_state(id, jd_tdb, out);
    }

    // Newest-catalog-wins record lookup for a body the planetary
    // ephemeris does not carry. Engaged only after it returned NotFound.
    Result<std::optional<catalog::Record>> find_record(int id) {
        for (auto it = catalogs.rbegin(); it != catalogs.rend(); ++it) {
            auto rr = (*it)->lookup(uint64_t(id));
            if (rr.ok())
                return std::optional<catalog::Record>(rr.value());
            if (rr.error().code != ErrorCode::NotFound)
                return rr.error();
        }
        return std::optional<catalog::Record>();
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
        if (!slot) {
            auto built = build_small_body(rec);
            if (!built)
                return built.error();
            slot = std::move(built).value();
        }
        const State s = slot->at(jd_tdb);
        if (!perturbers.ok())
            return make_error(ErrorCode::ArgumentError, perturbers.error());
        if (!std::isfinite(s.pos.x) || !std::isfinite(s.vel.x) || jd_tdb < slot->coverage_lo() ||
            jd_tdb > slot->coverage_hi())
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
    Result<std::unique_ptr<WindowMemo<BarycentricForce>>>
    build_small_body(const catalog::Record& rec) {
        const Elements el{rec.a_au,     rec.e,        rec.inc_rad,
                          rec.node_rad, rec.argp_rad, rec.mean_anom_rad};
        auto seed = seed_from_elements(el, rec.epoch_jtdb);
        if (!seed)
            return seed.error();
        auto memo = std::make_unique<WindowMemo<BarycentricForce>>(&force, IntegrateOptions{});
        memo->set_seed(seed.value(), rec.epoch_jtdb);
        return memo;
    }

    // The perturbed trajectories behind sigma_arcsec, built lazily on the
    // first query that needs them. FD steps are the element sigmas —
    // which also probes the propagation's nonlinearity at the 1-sigma
    // scale — with a relative floor against double-precision noise,
    // capped at half the distance to the singular element values
    // (a -> 0, e -> 1). A zero-sigma element contributes no column.
    Result<std::unique_ptr<SigmaTracks>> build_sigma_tracks(const catalog::Record& rec) {
        std::vector<SigmaTracks::Column> columns;
        if (!rec.has(catalog::RecordFlags::kSigmas))
            return std::make_unique<SigmaTracks>(std::move(columns));
        const double el0[6] = {rec.a_au,     rec.e,        rec.inc_rad,
                               rec.node_rad, rec.argp_rad, rec.mean_anom_rad};
        for (int k = 0; k < 6; ++k) {
            const double s = rec.sigmas[k];
            if (!(s > 0.0))
                continue;
            const double scale = k == 0 ? std::fabs(el0[0]) : 1.0;
            double h = std::max(s, 1e-8 * scale);
            if (k == 0)
                h = std::min(h, 0.5 * std::fabs(el0[0])); // a away from 0
            if (k == 1) {
                // e strictly between its singular values on both sides:
                // elliptic records stay elliptic, hyperbolic stay hyperbolic.
                if (el0[1] < 1.0)
                    h = std::min(h, 0.5 * std::min(el0[1], 1.0 - el0[1]));
                else
                    h = std::min(h, 0.5 * (el0[1] - 1.0));
            }
            if (!(h > 0.0))
                continue; // degenerate record (e.g. an exact circle with sigma_e)

            SigmaTracks::Column col;
            col.sigma = s;
            col.h = h;
            for (int sign : {+1, -1}) {
                double p[6];
                for (int i = 0; i < 6; ++i)
                    p[i] = el0[i];
                p[k] += sign * h;
                const Elements el{p[0], p[1], p[2], p[3], p[4], p[5]};
                auto seed = seed_from_elements(el, rec.epoch_jtdb);
                if (!seed)
                    return seed.error();
                auto memo =
                    std::make_unique<WindowMemo<BarycentricForce>>(&force, IntegrateOptions{});
                memo->set_seed(seed.value(), rec.epoch_jtdb);
                (sign > 0 ? col.plus : col.minus) = std::move(memo);
            }
            columns.push_back(std::move(col));
        }
        return std::make_unique<SigmaTracks>(std::move(columns));
    }

    // 1-sigma sky-plane uncertainty of a catalog body (arcsec), or
    // nullopt when the record publishes no sigmas, publishes only zeros
    // that degenerate away, or the perturbed tracks failed to reach the
    // epoch. The barycentric position covariance at the retarded epoch is
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
        if (!rec.has(catalog::RecordFlags::kSigmas))
            return std::nullopt;

        auto& tracks = sigma_tracks[uint64_t(id)];
        if (!tracks) {
            auto built = build_sigma_tracks(rec);
            if (!built.ok())
                return std::nullopt;
            tracks = std::move(built).value();
        }
        if (tracks->empty())
            return 0.0; // sigmas present but every one is zero

        const double jd_tdb = time::tdb_from_tt(jd_tt);
        const double t_ret = jd_tdb - tau;
        double cov[6];
        if (!tracks->covariance_at(t_ret, cov) || !perturbers.ok())
            return std::nullopt;

        // Line of sight and distance, ICRF (the covariance's frame).
        double body[6], obs[6];
        if (!small_body_state(id, t_ret, body).ok())
            return std::nullopt;
        if (!observer(o, jd_tt, jd_tdb, obs).ok())
            return std::nullopt;
        double u[3];
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
    Result<double> sidereal_shift(const CalcOptions& o, double jd_tt) const {
        std::optional<frames::Ayanamsa> aya;
        if (o.sidereal == SiderealMode::User) {
            if (!std::isfinite(o.sidereal_epoch_jtdb) || !std::isfinite(o.sidereal_ayanamsa_deg))
                return make_error(ErrorCode::ArgumentError, "sidereal User anchor is not finite");
            aya = frames::ayanamsa_anchored(o.sidereal_epoch_jtdb, o.sidereal_ayanamsa_deg, jd_tt);
        } else {
            aya = frames::ayanamsa(int(o.sidereal), jd_tt);
            if (!aya)
                return make_error(ErrorCode::ArgumentError, "unknown sidereal mode");
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
            return aya->mean_deg - frames::precession_in_longitude_deg(jd_tt);
        }
        return make_error(ErrorCode::ArgumentError, "unknown frame");
    }

    // Body barycentric position at jd_tdb - tau. The subtraction is done
    // with its exact rounding error recovered (TwoSum) and applied through
    // the velocity: a JD double near the present only resolves ~40 us,
    // which would otherwise put ~1 m of noise into every retarded position.
    Result<void> retarded(int id, double jd_tdb, double tau, double out[6], bool* from_catalog) {
        const double s = jd_tdb - tau;
        const double bp = s - jd_tdb;
        const double err = (jd_tdb - (s - bp)) + (-tau - bp);
        auto r = body_barycentric(id, s, out, from_catalog);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] += out[3 + i] * err;
        return {};
    }

    // Observer->body vector in the output frame (km), for one TT epoch.
    Result<void> vector_at(int id, double jd_tt, const CalcOptions& o, double out[3], double& tau,
                           bool& from_catalog) {
        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double obs[6];
        auto r = observer(o, jd_tt, jd_tdb, obs);
        if (!r)
            return r;

        double tgt[6];
        double p[3];
        tau = 0.0;
        r = retarded(id, jd_tdb, 0.0, tgt, &from_catalog);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            p[i] = tgt[i] - obs[i];
        if (o.light_time) {
            // Fixed-point iteration; contracts by ~v/c per pass.
            for (int it = 0; it < 10; ++it) {
                const double next =
                    std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / apparent::kLightKmPerDay;
                const bool done = std::fabs(next - tau) < 1e-15;
                tau = next;
                r = retarded(id, jd_tdb, tau, tgt, nullptr);
                if (!r)
                    return r;
                for (int i = 0; i < 3; ++i)
                    p[i] = tgt[i] - obs[i];
                if (done)
                    break;
            }
        }

        const bool observer_is_sun_or_bary =
            o.center == Center::Heliocentric || o.center == Center::Barycentric;
        if (o.deflection && !observer_is_sun_or_bary && id != body::kSun) {
            double sun[6];
            r = source->barycentric(body::kSun, jd_tdb, sun);
            if (!r)
                return r;
            const double sun_to_body[3] = {tgt[0] - sun[0], tgt[1] - sun[1], tgt[2] - sun[2]};
            const double sun_to_obs[3] = {obs[0] - sun[0], obs[1] - sun[1], obs[2] - sun[2]};
            apparent::light_deflection(p, sun_to_body, sun_to_obs, apparent::kSunGmOverC2Km, p);
        }
        if (o.aberration)
            apparent::aberration(p, obs + 3, p);

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
            const EpochFrames& f = frames_at(jd_tt, need_nut);
            if (o.coords == Coords::Ecliptic) {
                rot1(f.eps_mean, m);
                matmul(m, f.pb, m);
            } else {
                std::memcpy(m, f.pb, sizeof m);
            }
            break;
        }
        case Frame::TrueOfDate: {
            const EpochFrames& f = frames_at(jd_tt, need_nut);
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
                    const EpochFrames& f = frames_at(jd_tt, true);
                    eps = f.eps_mean + f.deps;
                    break;
                }
                case Frame::MeanOfDate:
                    eps = frames_at(jd_tt, false).eps_mean;
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

    CalcResult res;
    double v[3], tau = 0.0;
    bool from_catalog = false;
    auto r = impl_->vector_at(id, jd_tt, o, v, tau, from_catalog);
    if (!r)
        return r.error();

    Position& pos = res.pos;
    for (int i = 0; i < 3; ++i)
        pos.xyz_au[i] = v[i] / kAuKm;
    if (o.speed) {
        const double tp = jd_tt + kSpeedStepDays, tm = jd_tt - kSpeedStepDays;
        double vp[3], vm[3], unused_tau;
        bool unused_cat;
        r = impl_->vector_at(id, tp, o, vp, unused_tau, unused_cat);
        if (!r)
            return r.error();
        r = impl_->vector_at(id, tm, o, vm, unused_tau, unused_cat);
        if (!r)
            return r.error();
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

    res.provenance.source = from_catalog && !impl_->overlay_source_.empty()
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
    if (from_catalog && o.sigma)
        res.sigma_arcsec = impl_->sigma_arcsec(id, o, jd_tt, tau);
    return res;
}

Result<void> Engine::add_catalog(const std::string& path) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    auto r = catalog::Reader::open(path);
    if (!r)
        return r.error();

    // Index the new catalog's names (this streams and CRC-verifies the
    // whole container) before it is owned by the engine, so a corrupt
    // file fails add_catalog cleanly.
    std::unordered_map<std::string, uint64_t> names;
    auto fe = r.value().for_each([&](const catalog::Record& rec, const catalog::Names& n) {
        auto put = [&](std::string_view s) {
            if (s.empty())
                return;
            std::string key(s);
            for (char& c : key)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            names[std::move(key)] = rec.spkid;
        };
        put(n.pdes);
        put(n.name);
    });
    if (!fe)
        return fe.error();

    impl_->catalogs.push_back(std::make_unique<catalog::Reader>(std::move(r).value()));
    // A newer catalog may carry revised elements for bodies already
    // integrated: drop the memoized trajectories and uncertainty tracks,
    // they rebuild lazily.
    impl_->small_bodies.clear();
    impl_->sigma_tracks.clear();
    // The new catalog wins for any name an older one also carries.
    for (auto& kv : names)
        impl_->name_index[std::move(kv.first)] = kv.second;
    std::string counts;
    for (size_t i = impl_->catalogs.size(); i-- > 0;) {
        if (!counts.empty())
            counts += ", ";
        counts += std::to_string(impl_->catalogs[i]->record_count());
    }
    impl_->overlay_source_ =
        impl_->source->description + " + EPM1 catalog(s) [" + counts + " bodies]";
    return {};
}

Result<int> Engine::lookup(std::string_view name) const {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    std::string key(name);
    for (char& c : key)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    auto it = impl_->name_index.find(key);
    if (it == impl_->name_index.end())
        return make_error(ErrorCode::NotFound, "no loaded catalog answers '" + key + "'");
    return int(it->second);
}

Result<CalcResult> Engine::calc_ut(int id, double jd_ut1, const CalcOptions& o) {
    if (!impl_)
        return make_error(ErrorCode::ArgumentError, "engine is not open");
    // Delta T is a function of TT; one fixed-point pass is ample (dDeltaT/dt
    // is ~1e-8, so the argument error is sub-microsecond).
    double jd_tt = jd_ut1 + impl_->delta_t_seconds(jd_ut1) / 86400.0;
    jd_tt = jd_ut1 + impl_->delta_t_seconds(jd_tt) / 86400.0;
    return calc(id, jd_tt, o);
}

void Engine::set_delta_t_model(const time::DeltaTModel* model) {
    if (impl_)
        impl_->delta_t = model;
}

std::string_view Engine::source() const {
    return impl_ ? std::string_view(impl_->source->description) : std::string_view();
}

} // namespace prometheia
