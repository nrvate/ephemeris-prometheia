# Segments

A segment is a Chebyshev fit of one body's trajectory over one interval: the
coefficients of x, y and z, plus the residuals someone measured. A consumer
that holds a segment evaluates any instant inside it, position and velocity,
at polynomial cost and without the ephemeris. A window a client would
otherwise receive as tens of thousands of sampled rows becomes a few dozen
segments.

`prometheia::segments` (`include/prometheia/segments.hpp`) is engine-level and
knows nothing about any wire format. It takes a sampler — anything that yields
a position, and a velocity when asked, at a TT instant — and returns segments. prometheiad
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

Each interval is probed at a high degree, 16 by default (`max_degree`), and
cut back to the degree it needs. The probe is an interpolation at the
Chebyshev nodes of the first kind, which costs exactly `degree + 1` sampler
calls and is near-minimax. Cutting a high-degree interpolant back to degree
d beats interpolating at d + 1 nodes: the dropped coefficients are in hand, and
the aliasing an interpolation at few nodes carries is gone. Where the target
cannot be met at `max_degree`, the interval is halved and each half fitted in
turn. Splitting stops at `min_half_span_days`, and a span needing more than
`max_segments` is an error rather than a silent truncation. Through the C API
the floor is half a span of 0.0125 day. A close encounter turns the direction
in about an hour: a floor of 0.05 day stopped at 0.01″ through Apophis's 2029
flyby, where the month now fits at 0.001″ in 15 segments. Only a fit that
misses its target splits, so this moves no fit that already succeeded.

The degree is not found by trying every degree in turn. A Chebyshev series of a
smooth function decays geometrically, so the probe shows what truncating at any
degree would cost — the coefficients above it that are already in hand, plus
the geometric continuation beyond the last one. The fitter reads the needed
degree off that estimate and cuts the probe there. Where the estimate wants
more than the probe has, the interval is probed again at that degree, the
first probe's samples kept as check points.

The estimate is never trusted. It only chooses where to start; the residuals a
segment publishes are always measured, and if the measurement comes in over
target the degree is raised one at a time — from the same samples, at no cost
in sampler calls — and past the probe's own degree, the interval is probed
higher. A series whose last coefficients are not decaying but are far below the
target (a slow body at degree 16, where they sit at the rounding floor) is not
out of reach: only a series that is both not decaying and still large is.
Such an interval, or one the estimate puts more than 8 degrees beyond
`max_degree`, is split. Anything nearer is tried at `max_degree` and measured,
because the estimate is pessimistic: a version that split at the estimate's
word cut the Sun's months at 0.001″ more often than a measurement shows they
need.

**What a fit costs.** Four kinds of call are avoided, each measured:

- **Rates.** The sampler is asked for a position only (`vel_au_day` is NULL)
  except at the few points where the rate residual is measured. An engine rate
  is a central difference and costs three times a position (4.6 µs against
  1.7 µs). A month of the grid below used to ask for 85–332 rates, one with every
  sample; it asks for 9–21.
- **A second look at a sample.** Every instant sampled in an interval is kept
  and judged at every degree tried, so raising the degree never samples the
  check set again. A neighbour starts from the rate at the end its predecessor
  ended on, and the two halves of a split share the middle.
- **A probe too low.** The next interval is probed two degrees above the last
  one's, so a month of a body that needs degree 13 is probed at 15 on its second
  half rather than 6. The first is probed at `max_degree`, or at
  `probe_degree`.
- **A check set bought for an interval that cannot pass.** The check set is
  sampled ends first, then the middle, and stops at the first point over target
  when the interval can still be raised or split.

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

Direction and distance are measured on 4 × (degree + 1) + 1 points, about four
for every node the fit used and at none of them: at the nodes the fit is exact
by construction and a check there measures nothing. (The probe's own nodes are
among the points as well when the segment was cut back from a higher degree;
there they are check points like any other.)

- **Where the points are.** At t = −cos(π i / N), i = 0..N: the extrema of a
  Chebyshev polynomial of order N, evenly spaced in the angle in which a
  Chebyshev series swings at a steady pace, which the next step relies on.
  Alone they measured no better than evenly spaced points did (Pluto at 0.1″,
  random to declared: 1.085, against 1.056).
- **Peaks between the points.** A local maximum of the residual among the points
  is read as the top of the parabola through it and its two neighbours, at most
  15% above the highest point. Four points to a half swing miss a peak by up to
  8% and the parabola by far less (estimates, for a pure sinusoid). It moved
  the worst random-instant to declared ratio at 0.1″ from 1.085 (Pluto) to
  1.012, with the same points. The fit's own decision uses the same number, so
  a segment that reads over its target on a peak is raised or split. With the
  reading removed, `segments_de440_a_month_costs_what_the_docs_say` fails.
- **Memoised.** The points are samples kept for the interval, so a degree
  raised from the same probe judges the same instants, and a probe taken higher
  keeps the old ones.

**Both interval endpoints are in the check set.** This is not a detail. A
Chebyshev interpolant's error oscillates and peaks near ±1, and the error of
its *derivative* peaks there much more sharply. Measured on the Moon over a
30-day span, a check set of interior points alone reported
`errRateArcsecPerDay` as 1.48 where an independent sample of random instants
found 3.6 — an under-report of two to three times, in the direction that
flatters the server. With the endpoints in the set the same fit declares 4.65.

**The rate is measured where it peaks, not everywhere.** A rate costs three
positions, and a rate at every check point was most of what a fit cost. The
rate residual is measured at the two ends, the two points beside each end and
every quarter of the interval: 9 points, where the check set has 4(degree + 1) +
1. The derivative's error peaks at the ends; the rest guard against a sampler
whose rates are noisy, which no end reaches. Over 27 cases (the grid below, 3,600
random instants each) the rate at a random instant reached 0.90 to 1.00 of its
segment's declared rate, with the instants where the engine's own rate jumps set
aside (see "Cost"). Without the ends and the points beside them the same fits
declare far too little: the Moon's rate 0.078″/day where random instants find
0.398, the Sun's 0.026 where they find 0.12, Pluto's 0.027 where they find 0.135
(0.1″, three years);
`segments_rates_are_asked_only_where_the_derivative_peaks` and
`segments_de440_declared_residuals_bound_the_truth` fail on it.

A declared residual is a promise to whoever consumes the segment. It must
bound what an outsider measures, so where the two disagree the fitter must be
the pessimist. `tests/test_segments.cpp` enforces exactly that: it measures
the fits again at 200 random instants per body and fails if the declared
numbers do not cover what it finds, allowing the declared value no more than
a few percent of slack (a random instant can land marginally worse than the
nearest check point; two to three times worse is a defect, one percent is
sampling).

## Cost

Geocentric apparent place from DE440, on one core, DE440 already resident,
measured 2026-11-09 on an Intel i7-8700K in a Release build with other work
loading the machine (load average 5 to 10 on 12 threads): the times are CPU
time and run perhaps 1.5 times what an idle machine gives, so read the ratios
rather than the milliseconds. "Before" is the fitter of commit b0b8f85, run the
same way the same day. Windows are warm: the frame work underneath — chiefly the
nutation nodes, one per half day — is computed once for a window and shared by
everything else that touches it (docs/FRAMES.md), and is paid by whoever asks
first. It is not in these times.

Thirty days — a chart window or an animation:

| Body    | Target  | Segments | Degree | Declared            | Sampler calls, before → after (with rates) |
|---------|---------|----------|--------|---------------------|---------------------------------------------|
| Moon    | 0.1″    | 2        | 12     | 0.018″, 0.42″/day   | 162 → 166 (162 → 17)                        |
| Sun     | 0.1″    | 1        | 9      | 0.027″, 0.15″/day   | 103 → 64 (103 → 9)                          |
| Jupiter | 0.001″  | 1        | 15     | 0.00024″, 0.003″/day | 99 → 88 (99 → 9)                            |

A year — a transit scan:

| Body    | Target  | Segments | Degree | Declared           | Sampler calls, before → after (with rates) | Time, before → after |
|---------|---------|----------|--------|--------------------|---------------------------------------------|----------------------|
| Moon    | 1″      | 16       | 12–16  | 0.39″, 8.5″/day    | 1,659 → 1,599 (1,659 → 129)                 | 11.4 → 4.8 ms        |
| Moon    | 0.1″    | 21       | 9–16   | 0.068″, 1.7″/day   | 2,274 → 1,925 (2,274 → 169)                 | 17.0 → 5.7 ms        |
| Sun     | 0.1″    | 8        | 13–16  | 0.042″, 0.16″/day  | 1,666 → 798 (1,666 → 65)                    | 9.1 → 2.2 ms         |
| Jupiter | 0.1″    | 12       | 6–16   | 0.024″, 0.25″/day  | 3,459 → 1,008 (3,459 → 97)                  | 18.9 → 2.9 ms        |

The declared figures moved with the change: the Sun's year was 8 segments of
degree 12–16 declaring 0.055″, and Jupiter's 13 of degree 6–12 declaring 0.077″.
A segment cut back from a degree-16 probe is more accurate than a
degree-d interpolation: Venus's year declares 0.000343″ where it declared
0.000470″, in the same 17 segments of degree 7–16.

The first fit over a time window costs more than the ones after it: the
frame work underneath —
chiefly the nutation nodes, one per half day — is computed once for a window
and shared by everything else that touches it (docs/FRAMES.md). A year of
window is about 15 ms, paid by whoever asks first; the nodes, 7 ms of it,
are kept by the corpus, so they are paid once for every body, engine and
thread, however long the scan (35 KB a year, capped at 128 MB). Month-chunk
fits (30.4375 days from J2000 TT, 0.001″, geocentric apparent, default
options) body by body over 1800–1820 on DE440, one thread (2026-11-09):

| Body | before | after |
|---|---:|---:|
| Sun (first) | 23.2 ms/body-year | 23.2 |
| Moon | 29.8 | 23.0 |
| Mars | 25.2 | 15.1 |
| Jupiter | 24.8 | 15.6 |
| Pluto | 21.1 | 11.7 |

With four threads, each on its own context of one corpus and a quarter of
the chunks, the five bodies took 6.4, 6.7, 5.7, 5.3 and 4.8 ms per
body-year before and 6.3, 5.4, 4.4, 4.2 and 3.2 after.

The sampler calls are the fit's whole cost; afterwards the client pays no
engine time at all, for any instant in the span. A year of the Moon is about
1,900 engine evaluations and 21 segments — against the tens of thousands of
object-instants a client spends sampling the same year uniformly, and unlike
that sampling it answers every instant between them.

**A month at a time, the way a client asks.** Ten years from 1800, in months of
30.4375 days from J2000 TT (the astromcp grid), each fitted on its own, with
the C API's aim of 0.8 × the tolerance, a segment floor of 0.0125 day and
degree 16. Calls a month (segments a month), before → after:

| Object   | 0.001″                  | 0.01″                  | 0.1″                  |
|----------|-------------------------|------------------------|-----------------------|
| Sun      | 222 (1.38) → 101 (1.30) | 149 (1.00) → 78 (1.01) | 85 (1.00) → 64 (1.00) |
| Moon     | 272 (2.45) → 214 (2.45) | 197 (2.00) → 180 (2.00) | 175 (2.00) → 169 (2.00) |
| Mercury  | 332 (1.93) → 145 (1.85) | 239 (1.44) → 111 (1.41) | 145 (1.17) → 86 (1.19) |
| Mars     | 226 (1.43) → 105 (1.36) | 162 (1.06) → 81 (1.07) | 105 (1.03) → 67 (1.04) |
| Jupiter  | 230 (1.54) → 119 (1.56) | 197 (1.23) → 93 (1.26) | 150 (1.19) → 76 (1.18) |
| Saturn   | 202 (1.43) → 111 (1.44) | 161 (1.09) → 85 (1.10) | 107 (1.02) → 67 (1.05) |
| Pluto    | 174 (1.30) → 101 (1.30) | 114 (1.00) → 78 (1.02) | 89 (1.00) → 63 (1.00) |
| Ceres    | 215 (1.34) → 100 (1.30) | 162 (1.01) → 79 (1.03) | 94 (1.00) → 64 (1.01) |
| Mean node | 167 (1.29) → 102 (1.32) | 110 (1.00) → 78 (1.02) | 100 (1.00) → 63 (1.00) |

Before, every call asked for a rate. After, a month asks for 9 to 21 (11 to
21 at 0.001″). Milliseconds a body-year, before → after:

| Object   | 0.001″        | 0.01″         | 0.1″          |
|----------|---------------|---------------|---------------|
| Sun      | 23.1 → 11.7   | 18.6 → 10.7   | 13.6 → 9.6    |
| Moon     | 30.4 → 15.4   | 25.1 → 14.4   | 23.9 → 14.4   |
| Mercury  | 32.0 → 12.5   | 25.8 → 11.7   | 19.8 → 11.1   |
| Mars     | 24.5 → 11.3   | 20.5 → 10.6   | 15.9 → 10.2   |
| Jupiter  | 24.4 → 12.8   | 20.4 → 11.6   | 17.7 → 10.1   |
| Saturn   | 22.3 → 11.5   | 18.6 → 10.7   | 15.3 → 10.6   |
| Pluto    | 19.5 → 11.5   | 15.8 → 10.5   | 13.5 → 10.0   |
| Ceres    | 26.1 → 12.9   | 21.2 → 12.0   | 15.2 → 10.5   |
| Mean node | 33.6 → 16.6  | 24.3 → 14.9   | 24.6 → 13.2   |

- **The gain is smaller than the call counts say at the coarse end.** A month at
  0.1″ is 64 calls for a body that needs degree 9: the probe's 17 nodes and a
  check set of 41 are most of it, and the first interval of every call is probed
  at 16 because a call has no predecessor to learn from. The Moon is two
  segments a month at every tolerance, and the month is first probed whole.
- **Across the 27 cases** the calls fell by 1.03 to 2.3 times (1.7 on the
  whole) and the time by 1.3 to 2.6 times (1.8).
- **The contract held.** The check was 3,600 random instants an object and
  tolerance, each against its own segment's declaration. The worst direction
  ratio, random to declared, went from 1.118 (Moon, 0.001″) to 1.027; the rate
  ratio, from 2.475 to 0.997.
  - Rate ratios are taken without the instants where the engine's own rate
    jumps, found by comparing it with an exact-step five-point difference of the
    engine's positions (a step of 2⁻⁶ day) and setting the instant aside when
    they differ by more than 0.3 of the segment's declared rate, or 2e-5″/day.
    Before the change, 114 of the mean node's 3,600 instants at 0.001″ were set
    aside; after, 8. The jumps are the engine's (TDB quantization), not the
    fit's, and its rates are what the declaration is against.

**The fitter's own arithmetic.** With a sampler that costs nothing — a closed
form of a few `sin` and `cos` — what is left is the fitter. CPU time a month,
sampler excluded, the least of four runs:

| Sampler (a month, 0.8 × tolerance) | Calls a month, before → after | Fitter, before → after |
|---|---|---|
| a 27-day period, 0.1″ (needs splits)  | 337.5 → 152 | 56 → 18.5 µs |
| a one-year period, 0.001″             | 76 → 104    | 5.6 → 8.0 µs |
| a twelve-year period, 0.001″          | 76 → 104    | 6.9 → 8.5 µs |

What was removed: the cosines of the interpolation (3 n² a fit, evaluated again
at each refit, now n + 1 a degree, read from a table shared by the three
components and every probe); the derivative recurrence that built three vectors
at each check point; and the allocation of the vectors of each probe. What was added is a bigger probe: a body that needs degree 7 is probed
at 16, which costs 28 calls more than it did and about 2 µs of fitter, so on
a sampler that costs a microsecond or more the fit is still faster. The
interpolated coefficients are not bit for bit what they were: the table takes
the cosines at the 2n angles they come from rather than at j θ, and differs
from the direct sum by 4e-15 of the series' size (degree 16, measured), the
rounding of the old sums.

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
        // v is NULL when only the position is wanted: skip the rates.
        CalcOptions opts = calc_options;
        opts.speed = v != nullptr;
        auto c = engine.calc(body::kMoon, jd, opts);
        if (!c) return c.error();
        std::copy_n(c.value().pos.xyz_au, 3, p);
        if (v) std::copy_n(c.value().pos.vel_au_day, 3, v);
        return {};
    },
    jd_from, jd_to, o);
```

Segments come back contiguous and in time order: each one's end is the next
one's start, the first starts at `jd_from` and the last ends at `jd_to`.

## Through the C API

`prometheia_segments_fit` ([C_API.md](C_API.md), "Segments") fits any
object, with the options of a position call, to a tolerance in arcseconds.
It aims at 0.8 × the tolerance, cuts the span where a second planetary file
takes over, and refuses a tolerance it cannot reach. The Python binding
calls it `Context.fit_segments` ([PYTHON.md](PYTHON.md)).

**Measured 2026-11-09** on an Intel i7-8700K, one thread, in a Release build
(`tests/test_c_segments.cpp`, `c_segments_report`), with other work loading
the machine (load average about 5). The data was DE440 with `sample-100.epm`
and `sb441-n16-de440span.bsp`. Each fit covers a year from 2026-01-01 at a
0.001″ tolerance, in apparent geocentric ecliptic of date unless named.
"Measured" is the worst angular separation from `prometheia_calc_object`
over 10,000 random instants, both ends included. "Eval" is one
`prometheia_segments_eval` at a random instant, averaged over a million. The
fit times are one cold run each, the Sun's first, so they include the frame
work for the year (the Sun 13.3 ms against 11.7 warm in the grid above); the
same run of b0b8f85 had them at 12–48 ms (topocentric 140–192).

| object | segments | degree | fit | kB | declared | measured | rate error | eval |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| Sun | 16 | 11–15 | 13.3 ms | 12.8 | 0.000766″ | 0.000766″ | 0.00812″/d | 188 ns |
| Moon | 32 | 11–15 | 13.3 ms | 25.5 | 0.000334″ | 0.000334″ | 0.04625″/d | 197 ns |
| Mercury | 23 | 6–16 | 6.5 ms | 18.3 | 0.000347″ | 0.000347″ | 0.01374″/d | 184 ns |
| Venus | 17 | 7–16 | 4.8 ms | 13.5 | 0.000343″ | 0.000340″ | 0.00454″/d | 204 ns |
| Mars | 17 | 7–16 | 4.1 ms | 13.5 | 0.000354″ | 0.000355″ | 0.00427″/d | 194 ns |
| Jupiter | 20 | 6–16 | 4.9 ms | 15.9 | 0.000338″ | 0.000338″ | 0.01325″/d | 205 ns |
| Saturn | 17 | 7–16 | 4.1 ms | 13.5 | 0.000277″ | 0.000279″ | 0.00579″/d | 184 ns |
| Uranus | 21 | 6–16 | 4.4 ms | 16.7 | 0.000625″ | 0.000379″ | 0.05396″/d | 183 ns |
| Neptune | 17 | 11–16 | 4.0 ms | 13.5 | 0.000595″ | 0.000592″ | 0.01313″/d | 176 ns |
| Pluto | 16 | 11–16 | 3.6 ms | 12.8 | 0.000281″ | 0.000281″ | 0.00369″/d | 181 ns |
| Ceres | 16 | 11–16 | 9.2 ms | 12.8 | 0.000298″ | 0.000298″ | 0.00423″/d | 182 ns |
| Pallas | 16 | 11–15 | 5.3 ms | 12.8 | 0.000265″ | 0.000266″ | 0.00313″/d | 187 ns |
| Juno | 16 | 11–16 | 5.3 ms | 12.8 | 0.000360″ | 0.000360″ | 0.00486″/d | 183 ns |
| Vesta | 16 | 11–16 | 6.5 ms | 12.8 | 0.000284″ | 0.000286″ | 0.00347″/d | 188 ns |
| Moon mean node | 16 | 10–16 | 10.4 ms | 12.8 | 0.000355″ | 0.000356″ | 0.00306″/d | 180 ns |
| Moon osculating node | 32 | 12–16 | 19.2 ms | 25.5 | 0.000137″ | 0.000136″ | 0.00372″/d | 189 ns |
| Mars, Lahiri | 16 | 6–16 | 6.4 ms | 12.8 | 0.000586″ | 0.000586″ | 0.00342″/d | 156 ns |
| Moon, topocentric | 256 | 15–15 | 80.9 ms | 204.0 | 0.000101″ | 0.000087″ | 0.04347″/d | 235 ns |
| Mars, topocentric | 157 | 11–16 | 40.3 ms | 125.1 | 0.000787″ | 0.000780″ | 0.15542″/d | 218 ns |

- **Every case holds the tolerance.** The worst is Mars, topocentric, at
  0.000780″ against 0.001″.
- **The declared residual can be beaten.** The measured error passes the
  declared one in five cases, by up to 0.7% (Saturn: 0.000279″ against
  0.000277″; Vesta, 0.000286″ against 0.000284″). Before, it was seven cases by
  up to 2.8%. That is the margin the 0.8 aim leaves room for: the declaration is
  measured on the check set, not everywhere.
- **Topocentric segments are short.** The site's daily circle puts a
  diurnal term in every topocentric position. At 0.001″ a year takes 256
  segments for the Moon, one about every 1.4 days, and 157 for Mars, against
  16 to 32 from the geocentre (before: 257 for both).
  - The fit costs 6 to 10 times as much (40–81 ms a year, from 140–192), and
    the coefficients take 125–204 kB.
  - The 4096-segment cap bounds a topocentric fit at 0.001″ to about 16 years
    for the Moon and 26 for Mars, and a geocentric Moon to about 128 years.
    All are estimates, from these rates.
- **The Moon** needs 32 segments a year at 0.001″, as many as its
  osculating node. Its fit costs about what the Sun's does (13 ms), because
  the segments are shorter rather than of higher degree.
- **Evaluation** costs 150–240 ns, the search for the segment included.
  The fit's cost is paid once per span; after it, no instant costs any
  engine time.
- **Where a second planetary file takes over**, the engine's answer steps:
  0.34″ for the Moon between DE441 and DE440 at 1550. A fit across that
  instant gets a bound at the observer's handover and another one light
  time later. It holds 0.001″ up to both bounds from either side
  (`c_segments_steps_and_refusals`). With the cuts disabled, the same fit
  is refused, at 0.34″.
- **No other source steps.** A catalog body has one record, the newest
  catalog's, for all instants. A hypothetical has one definition. Stars
  and mean elements are analytic, and the interpolated apsides are a C1
  Hermite. A file that runs out mid-span is no step: the engine refuses
  there, and the fit fails as a whole. **A feature that adds records valid
  for a range of dates must teach the fit their boundaries in the same
  change** (`segment_cuts` in `src/c_api.cpp`), or a step no sample lands
  near becomes a silent error. Astrolog's review asked for this rule to be
  written down (2026-10-28).

## Roots

`prometheia_segments_roots` ([C_API.md](C_API.md), "Roots on segments")
finds every instant in a window where a function of one or two fitted
objects passes a target: f = ca·Xa + cb·Xb − target, the longitude taken
modulo M. It is the scanner the section above describes, done once, in
C++, for every caller (`src/segment_roots.cpp`). astromcp asked for it, for
its ingresses, returns, stations, aspect windows and declination searches.

**The method.**
- **Pieces.** The window is cut into pieces, each where one segment of
  each term overlaps. Each piece is searched whole, whatever the window, so
  with the same handles, target and `tol_days` a root is the same double
  from any window that holds it. Anything else that changes f can move a
  root within its bracket ("Windows", below).
- **Continuity first.** At a bound each term's two sides may differ by up
  to twice its tolerance. Half that step is spread linearly over the
  segment on each side, so each term is continuous, and so is f. Without
  this, a root at a bound is found twice or not at all.
- **Interpolation.** On a piece, f is interpolated at the 17 Chebyshev
  points of degree 16. A longitude is first unwrapped on each cell: from
  one point to the next it moves by the step its own rate predicts, so the
  interpolated f is continuous and never sees a wrap.
  - The levels are the targets plus whole moduli, target + k·M. Those a run
    passes are counted from its unwrapped ends, which is why a wrap is
    never a root.
  - A cell is halved until the series' last two coefficients are below 1%
    of the band, or below 1e-13 of the series' size, whichever is larger.
    The second floor is rounding. It ends the halving when the band is near
    0, which a speed's declared rate error can be for a smooth handle.
- **Monotone runs.** A Chebyshev series cannot vanish where |c₀| exceeds
  the sum of the other coefficients. That bound, applied to the
  interpolant's derivative, finds cells where f is monotone. Applied to
  the second derivative, it finds cells with exactly one extremum, which is
  then located on the derivative's series. Any other cell is halved.
- **Counting and solving.** Between two consecutive breaks f is monotone.
  - The levels it passes are counted from its values at the two ends, as
    integers, so the count is exact and the same from both sides of a
    shared end.
  - Each level is then solved for on f itself, never on the interpolant,
    with a bracketed secant (Illinois, with bisection when it stalls) to
    `tol_days`.
  - A root exactly at the end of a run belongs to that run, never to the
    next.
- **Limits.** Every call ends.
  - A cell is not halved below 1e-6 day, nor more than 40 times, nor past
    16,384 cells in one piece. The narrowest cell is fixed: `tol_days` is
    only the precision of each root, so a coarse `tol_days` examines f as
    finely as a fine one. Each root is also solved to at least 1e-6 day, so
    its slope and tangent flag, read at the root, are the same whatever
    `tol_days` was asked. The six Mercury station grazes below give the same
    roots and flags at `tol_days` 0.5 as at 1e-9.
  - A cell at a limit (a pole crossing in an equatorial longitude fit, say)
    keeps its ends. Its roots are counted from them, so a pair inside it
    that cancels may be lost, but no root is invented.
  - The call reports how many such cells met the window (`*unresolved`), so
    a search that found every root can be told from one that may not
    have.
    `tests/test_segment_roots_limits.cpp` forces a limit: no halving, and
    separately a narrowest cell wider than a piece. On 60 days of the
    Moon's latitude the count is then above 0, while the same window at the
    default limits counts 0 and finds all four nodes. With the counter
    disabled, the test fails.
- **Order.** Roots strictly increase. Two levels solved to a coarse
  `tol_days` could share an instant; the later one is then moved to the
  next double. So a resume, which starts at a root and includes it, never
  writes one twice.
- **Grazes.** Near an extremum within the band of the level, the fits
  cannot tell one root from two or none. Crossings there are flagged
  `tangent`, and so is an extremum that comes within the band without
  crossing; that one is reported as a root of its own. The rule is local:
  a crossing is tangent when slope²/(2|f″|) is within the band, so the
  answer never depends on what the window holds.

**Measured 2026-10-28** on an Intel i7-8700K, one thread, Release build,
with the machine loaded (load average about 2.6).
- **The data:** DE440 with `prometheia_segments_fit` at 0.001″, apparent
  geocentric, ecliptic of date. The declination cases use the equatorial
  fit. The year is 2026.
- **Truth** is bisection on `prometheia_calc_object` to 60 halvings, from a
  grid of 0.05–0.5 days.
- **Solve precision:** `tol_days` 1e-6.
- **"Worst"** is the worst time difference from truth. **Ratio** is the
  worst of that difference over the root's own `err_days`.

| search | roots | missed / extra | worst | ratio | time |
|---|---:|---:|---:|---:|---:|
| Moon sign ingresses (M = 30), a year | 160 | 0 / 0 | 0.042 s | 0.48 | 0.85 ms |
| the same, 12 month handles back to back | 160 | 0 / 0 | 0.043 s | 0.48 | 0.81 ms |
| the same, fitted at 1″ | 160 | 0 / 0 | 0.76 s | 0.43 | 0.81 ms |
| Moon–Sun quarters (M = 90), separate fits | 50 | 0 / 0 | 0.042 s | 0.47 | 0.77 ms |
| Sun/Moon midpoint at 15° (M = 180) | 15 | 0 / 0 | 0.039 s | 0.44 | 0.54 ms |
| Moon declination at ± the obliquity, moving | 27 + 26 | 0 / 0 | 0.043 s | 0.46 | 0.48 ms |
| Mercury stations (speed = 0) | 6 | 0 / 0 | 0.63 s | 0.03 | 0.16 ms |
| Mars stations, 2026–2028 | 2 | 0 / 0 | 3.3 s | 0.18 | 0.20 ms |
| Jupiter–Saturn, 8th harmonic (M = 45), 2019–2022 | 3 | 0 / 0 | 0.022 s | 0.03 | 0.74 ms |
| Pluto back to its 2026-05-01 longitude | 2 | 0 / 0 | 0.77 s | 0.08 | 0.14 ms |
| Neptune, the same | 2 | 0 / 0 | 0.049 s | 0.05 | 0.12 ms |
| Saturn, the same | 2 | 0 / 0 | 0.012 s | 0.03 | 0.12 ms |
| Moon ingresses, topocentric (72.9° E, 19.1° N), 90 days | 39 | 0 / 0 | 0.043 s | 0.48 | 0.97 ms |
| Sun/Moon midpoint = Mars (3 terms, M = 180) | 13 | 0 / 0 | 0.024 s | 0.26 | 0.72 ms |
| Sun/Moon midpoint = Venus/Jupiter midpoint (4 terms, M = 180) | 14 | 0 / 0 | 0.032 s | 0.34 | 0.98 ms |
| Moon topocentric latitude at 2.5°, 90 days | 9 | 0 / 0 | 0.033 s | 0.35 | 2.5 ms |

- **No root was missed or doubled.** A resumed search with `capacity` 1
  returned the same 160 Moon ingresses, bit for bit, as one call.
- **Roots on the bounds.** In a Moon fit at 1″, a root was placed exactly on
  each of the 15 bounds by setting the target halfway across the step
  there. This was done for longitude, latitude and longitude speed; the
  steps were up to 0.28″, 0.02″ and 9.7″/day.
  - Every bound gave exactly one root.
  - With the continuity correction removed, every one of the 45 was missed.
- **Grazes**, at each of Mercury's six stations in 2026 (band 0.001″):
  - a target reached by half the band: two crossings 0.002–0.004 days
    apart, both tangent;
  - a target half a band beyond the extremum: one tangent root at the
    station;
  - three bands beyond: nothing;
  - reached by three bands: two ordinary crossings, 9 to 14 minutes apart.
- **An independent check**, `tests/test_c_segment_roots.cpp`, was written
  from the contract above without reading the search's code. Its oracle
  builds the same f from `prometheia_calc_object`. It locates every extremum
  between two grid points, so a close pair is not missed, and bisects each
  sign change to 1e-9 day.
  - **Agreement.** Eleven searches ran at `tol_days` 1e-8:
    - the Moon's ingresses on one handle, on twelve month handles and on
      the 1″ fit;
    - the quarters and the midpoint;
    - the declination at 0 and at ± the moving obliquity;
    - the Mercury and Mars stations;
    - the Sun against a target moving at 1°/day.

    Every one had 0 missed and 0 extra roots.
    `err_days` was band/|slope| + `tol_days` to 1e-9 relative, and
    `*unresolved` was 0.
  - **Error.** The worst time difference was 0.457 of the root's `err_days`
    (Moon ingresses, 12 month handles), with medians of 0.02–0.08.
  - **Contract edges.** Counting with `capacity` 0 gives the same total as
    one call. A root at `jd_tt_end` belongs to the next window. Four
    threads on the same handles get the same doubles.
  - **Windows.** `c_segment_roots_windows_report` asked 47,870 windows for
    1,456,398 roots: Moon ingresses, phases, the Sun's whole degrees and a
    moving target over 2026, at `tol_days` 1e-6 and 1e-8. Half the windows
    were random and half began and ended on a segment bound, a handle join
    or a root, or one ulp beside one. All were bit-identical to the whole
    year's call. A solve stops once its bracket is `tol_days` wide and
    returns the middle, so the same crossing asked another way can land
    elsewhere in that bracket, up to one width from another answer:

    | The same crossing, asked another way | `tol_days` | Roots moved | Worst | Of `err_days` |
    |---|---|---|---|---|
    | Only the handles of the window, not all twelve | 1e-6 | 325 of 855 | 0.069 s | 0.78 |
    | The same | 1e-9 | 250 of 855 | 0.0003 s | 0.18 |
    | A moving target anchored at the window's start | 1e-6 | 2 of 13 | 0.030 s | 0.34 |
    | `tol_days` 1e-9 against 1e-6 | | 101 of 160 | 0.043 s | 0.48 |
    | The target one ulp higher | 1e-6 | 10 of 161 | 0.043 s | 0.48 |

    At a fine `tol_days` what is left is f itself: a shorter list has no
    join to spread at its ends. At 1e-11, all 172 roots that moved were in
    the list's first or last segment, and the 465 elsewhere stayed bit for
    bit. Cutting the search to the window at either end
    fails `c_segment_roots_any_window`; the cut at the window's end passes
    the resume test.
  - **Bounds.** On the 1″ fit, a longitude target at either end of the step
    at a bound, as well as halfway, gives one root within 1e-4 day of the
    bound. The step's ends are asked of the longitude only: the latitude
    and speed may turn within the window (the speed near apogee), so a
    target at the step's end is not passed near the bound. That is correct
    for a function made continuous through the middle of the step.
  - **Faults injected.** Eleven faults were put into the search, one at a
    time, and the check failed on each:
    - no continuity correction;
    - the tangent test 100 times too wide;
    - no root at an extremum that touches;
    - `tol_days` left out of `err_days`;
    - a touching root's `err_days` doubled;
    - the resume point 1e-6 day late;
    - a closed window;
    - the slope's sign flipped;
    - the band halved;
    - solving and the narrowest cell set by `tol_days`;
    - `*unresolved` one too high.
- **Time** is a millisecond a year or less. It is spent mostly evaluating
  the handles at 17 points per cell; no engine time is involved.
