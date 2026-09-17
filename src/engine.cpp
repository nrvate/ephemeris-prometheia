// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::Engine: ephemeris sources, the apparent-place pipeline and
// frame output. See include/prometheia/engine.hpp and docs/ENGINE.md.
#include "prometheia/engine.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "prometheia/apparent.hpp"
#include "prometheia/de.hpp"
#include "prometheia/spk.hpp"

namespace prometheia {
namespace {

constexpr double kAuKm = 149597870.7; // IAU 2012 Resolution B2 (exact)
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
    std::string description;
    int denum = 0;
};

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

} // namespace

struct Engine::Impl {
    std::unique_ptr<Source> source;
    const time::DeltaTModel* delta_t = nullptr;
    time::EspenakMeeusDeltaT default_delta_t;
    double bias[9];
    double eps_j2000 = 0.0;
    EpochFrames cache[3];
    int cache_next = 0;

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

    // Body barycentric position at jd_tdb - tau. The subtraction is done
    // with its exact rounding error recovered (TwoSum) and applied through
    // the velocity: a JD double near the present only resolves ~40 us,
    // which would otherwise put ~1 m of noise into every retarded position.
    Result<void> retarded(int id, double jd_tdb, double tau, double out[6]) {
        const double s = jd_tdb - tau;
        const double bp = s - jd_tdb;
        const double err = (jd_tdb - (s - bp)) + (-tau - bp);
        auto r = source->barycentric(id, s, out);
        if (!r)
            return r;
        for (int i = 0; i < 3; ++i)
            out[i] += out[3 + i] * err;
        return {};
    }

    // Observer->body vector in the output frame (km), for one TT epoch.
    Result<void> vector_at(int id, double jd_tt, const CalcOptions& o, double out[3], double& tau) {
        const double jd_tdb = time::tdb_from_tt(jd_tt);
        double obs[6];
        auto r = observer(o, jd_tt, jd_tdb, obs);
        if (!r)
            return r;

        double tgt[6];
        double p[3];
        tau = 0.0;
        r = retarded(id, jd_tdb, 0.0, tgt);
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
                r = retarded(id, jd_tdb, tau, tgt);
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
    auto r = impl_->vector_at(id, jd_tt, o, v, tau);
    if (!r)
        return r.error();

    Position& pos = res.pos;
    for (int i = 0; i < 3; ++i)
        pos.xyz_au[i] = v[i] / kAuKm;
    if (o.speed) {
        const double tp = jd_tt + kSpeedStepDays, tm = jd_tt - kSpeedStepDays;
        double vp[3], vm[3], unused;
        r = impl_->vector_at(id, tp, o, vp, unused);
        if (!r)
            return r.error();
        r = impl_->vector_at(id, tm, o, vm, unused);
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

    res.provenance.source = impl_->source->description;
    res.provenance.denum = impl_->source->denum;
    res.provenance.light_time_days = tau;
    return res;
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
