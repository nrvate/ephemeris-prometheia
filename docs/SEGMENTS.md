# Segments

A segment is a Chebyshev fit of one body's trajectory over one interval: the
coefficients of x, y and z, plus the residuals someone measured. A consumer
that holds a segment evaluates any instant inside it, position and velocity,
at polynomial cost and without the ephemeris. A window a client would
otherwise receive as tens of thousands of sampled rows becomes a few dozen
segments.

`prometheia::segments` (`include/prometheia/segments.hpp`) is engine-level and
knows nothing about any wire format. It takes a sampler — anything that yields
a position and velocity at a TT instant — and returns segments. prometheiad
translates at its boundary; see [SERVER.md](SERVER.md).

## What is fitted

The fit is in whatever frame and centre the sampler works in. A sampler built
on `Engine::calc` with apparent-place options produces segments of the
*apparent* place, light time and aberration already inside the coefficients,
which is what an astrology client wants and what makes the segment worth
sending. The fitter never re-interprets the vectors it is handed: it fits
three scalar functions of time and measures how well they came back.

There is also a **scalar** fit (`fit_scalar`), for a one-dimensional series
of time — the ayanamsa, in version 4's SEGDATA. It exists because the vector
fit's direction residual is an angle between directions and cannot see an
error in a scalar: the same machinery (node interpolation, the coefficient
tail as a degree estimate, a measured check set with both endpoints in it,
raising the degree, then splitting) is applied to the value itself, in the
value's own units. prometheiad fits an ayanamsa over the same lattice cells
and at the same rung as the body segments it rides with, converting the rung
with 3600; the sampler asks the engine for the Sun's own sidereal calc and
reads the ayanamsa out of the result (it is a frame quantity — the observer
does not matter, so the sampler observes from the geocentre).

## How the degree and the span are chosen

Each interval is interpolated at the Chebyshev nodes of the first kind, which
costs exactly `degree + 1` sampler calls and is near-minimax. Where the target
cannot be met at `max_degree`, the interval is halved and each half fitted in
turn. Splitting stops at `min_half_span_days`, and a span needing more than
`max_segments` is an error rather than a silent truncation.

The degree is not found by trying every degree in turn: that would fit and
check each interval four or five times over. A Chebyshev series of a smooth
function decays geometrically, so one cheap fit shows what truncating at any
degree would cost — the coefficients above it that are already in hand, plus
the geometric continuation beyond the last one. The fitter reads the needed
degree off that estimate, refits once at the degree it names, re-reads the
estimate from the better coefficients, and only then pays for the check set.
An interval whose series is not decaying at all, or is out of reach at any
allowed degree, is split without a check set being bought for it.

The estimate is never trusted. It only chooses where to spend the check set;
the residuals a segment publishes are always measured, and if the measurement
comes in over target the degree is raised and the interval re-measured. On the
Moon this turned 10,194 sampler calls for a year into 1,660, with the same
segments coming out.

Degrees are capped (16 by default) because a consumer sizes a buffer for them.
Raising the degree without bound also stops paying: past the point where the
trajectory's curvature over the interval is resolved, halving the interval
converges far faster.

## The residuals, and why the endpoints are in the check set

Three numbers accompany every segment, each the worst seen over a check set:

- `err_arcsec` — the angle between the fitted and sampled direction;
- `err_rel_dist` — the relative error in |r|;
- `err_rate_arcsec_per_day` — the analytic derivative of the fit against the
  sampler's own velocity, seen from the observer.

The check set is deliberately denser than the fit (four times the coefficient
count by default) and offset from the nodes, because at the nodes the fit is
exact by construction and a check there measures nothing.

**It also includes both interval endpoints.** This is not a detail. A
Chebyshev interpolant's error oscillates and peaks near ±1, and the error of
its *derivative* peaks there much more sharply. Measured on the Moon over a
30-day span, a check set of interior points alone reported
`errRateArcsecPerDay` as 1.48 where an independent sample of random instants
found 3.6 — an under-report of two to three times, in the direction that
flatters the server. With the endpoints in the set the same fit declares 4.65.

A declared residual is a promise to whoever consumes the segment. It must
bound what an outsider measures, so where the two disagree the fitter must be
the pessimist. `tests/test_segments.cpp` enforces exactly that: it measures
the fits again at 200 random instants per body and fails if the declared
numbers do not cover what it finds, allowing the declared value no more than
a few percent of slack (a random instant can land marginally worse than the
nearest check point; two to three times worse is a defect, one percent is
sampling).

## Cost

Geocentric apparent place from DE440, on one core, DE440 already resident.

Thirty days — a chart window or an animation:

| Body    | Target  | Segments | Degree | Declared            | Sampler calls |
|---------|---------|----------|--------|---------------------|---------------|
| Moon    | 0.1″    | 2        | 12     | 0.019″, 0.46″/day   | 162           |
| Sun     | 0.1″    | 1        | 9      | 0.036″, 0.19″/day   | 103           |
| Jupiter | 0.001″  | 1        | 15     | 0.0004″, 0.003″/day | 99            |

A year — a transit scan:

| Body    | Target  | Segments | Degree | Declared           | Sampler calls | Time  |
|---------|---------|----------|--------|--------------------|---------------|-------|
| Moon    | 1″      | 16       | 12–16  | 0.44″, 8.6″/day    | 1,660         | 35 ms |
| Moon    | 0.1″    | 21       | 9–16   | 0.068″, 1.7″/day   | 2,274         | 12 ms |
| Sun     | 0.1″    | 8        | 12–16  | 0.055″, 0.22″/day  | 1,666         | 6 ms  |
| Jupiter | 0.1″    | 12       | 6–16   | 0.098″, 0.54″/day  | 3,325         | 13 ms |

The first fit over a time window costs more than the ones after it, which is
the 35 ms against 12 ms above for the same body: the frame work underneath —
chiefly the nutation nodes, one per half day — is computed once for a window
and shared by everything else that touches it (docs/FRAMES.md). A year of
window is about 15 ms, paid by whoever asks first.

The sampler calls are the fit's whole cost; afterwards the client pays no
engine time at all, for any instant in the span. A year of the Moon is about
2,300 engine evaluations and 21 segments — against the tens of thousands of
object-instants a client spends sampling the same year uniformly, and unlike
that sampling it answers every instant between them.

## Scanning

Segments are not only a bandwidth saving. A client that searches for events —
ingresses, aspects forming, stations — samples uniformly and interpolates
linearly between adjacent samples, so its accuracy is set by its step rather
than by the ephemeris. With a segment it can root-find on the polynomial
instead, and the answer stops depending on the step.

That makes the fitter's real fitness test not smoothness but *where the roots
land*. `tests/test_segments.cpp` measures it: over 180 days, fitted at a
deliberately coarse one arcsecond, the Moon's 71 sign ingresses come out
within 0.57 seconds of time of where bisection on the engine itself puts them.
One arcsecond of Moon is about two seconds of time, so the roots are no worse
than the fit that produced them — which is the property a scanner needs and
the reason `err_rate_arcsec_per_day` is published: a station is a root of the
rate, found the same way on the analytic derivative.

## Using it

```cpp
segments::FitOptions o;
o.target_err_arcsec = 0.1;
auto r = segments::fit(
    [&](double jd, double p[3], double v[3]) -> Result<void> {
        auto c = engine.calc(body::kMoon, jd, calc_options);
        if (!c) return c.error();
        std::copy_n(c.value().pos.xyz_au, 3, p);
        std::copy_n(c.value().pos.vel_au_day, 3, v);
        return {};
    },
    jd_from, jd_to, o);
```

Segments come back contiguous and in time order: each one's end is the next
one's start, the first starts at `jd_from` and the last ends at `jd_to`.
