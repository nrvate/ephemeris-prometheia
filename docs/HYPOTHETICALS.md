# Hypothetical bodies and bodies from elements

Some bodies are not in any ephemeris: the eight transneptunian points of the
Hamburg School of astrology, Transpluto, the planets predicted before Neptune
and Pluto were found, fictitious moons of the Earth. Each is defined by a set
of orbital elements and nothing else. Prometheia computes them two ways, which
are the ephemeris protocol v4's object kinds 3 and 4:

- **From elements the caller supplies** — `Engine::calc_elements` and a
  `PolynomialElements`, the protocol's kind 4. The answer is a function of the
  elements alone, so two servers given the same elements must agree.
- **By name** — one of the protocol's A.15 tokens (`cupido` … `waldemath`), the
  protocol's kind 3. The server supplies the elements and the answer names the
  set it used, because for these bodies the element set *is* the definition.
  See [Named bodies](#named-bodies).

## The element set

Six orbital elements, each a polynomial of 1 to 5 terms in

    T = (t_TT − epoch) / 36525        Julian centuries of TT from the epoch

| element | unit |
|---|---|
| mean anomaly M | degrees |
| semi-major axis a | AU |
| eccentricity e | — |
| argument of perihelion ω | degrees |
| longitude of the ascending node Ω | degrees |
| inclination i | degrees |

The orbit is about an **origin**, the Sun or the Earth (the protocol's *centre*,
A.21, renamed here so it cannot be confused with the observer's centre), and the
elements are referred to the **mean ecliptic and equinox** of one of five
epochs (protocol A.16):

| equinox | the mean ecliptic and equinox of |
|---|---|
| J2000 | J2000.0, JD 2451545.0 TT |
| B1950 | B1950.0, JD 2433282.42345905 TT |
| J1900 | J1900.0, JD 2415020.0 TT |
| of date | the instant the elements are evaluated at |
| explicit | a Julian date the caller gives |

Two conventions here are choices, stated so that nobody has to rediscover them:

- **B1950 is a dynamical equinox, not the FK4 catalogue frame.** The elements
  are rotated to the ICRF with the engine's precession model from the epoch
  B1950.0. No FK4 E-terms and no FK4 → FK5 equinox correction are applied;
  those belong to star catalogues, not to fictitious orbits.
- **"Of date" is the date of the evaluated position.** When light time
  retards the body to t − τ, its elements are evaluated at t − τ and referred
  to the mean ecliptic of t − τ. The body's own plane moves with its own time.

The motion is **pure two-body Keplerian about the origin**. There are no
perturbations, and the body is massless.

## Mean anomaly and mean motion

This section is the specification the implementation is written from. Its
rules are those of the protocol owner's normative text for version 4
(approved 2026-09-18), stated as behaviour.

**M's own coefficients decide what M means** — whether any beyond the
constant term is nonzero, not how many terms the set carries.

- If **every coefficient of M beyond the constant term is zero**, the constant
  is the mean anomaly *at the epoch*, and the body advances along its orbit at
  the mean motion n:

      M(t) = M₀ + n · (t − epoch)           (t − epoch in days)

- If **any coefficient of M beyond the constant term is nonzero**, M's
  polynomial evaluated at T *is* the mean anomaly of date, in full, and no mean
  motion is added:

      M(t) = M₀ + M₁·T + M₂·T² + …

The rule lets both conventional spellings of an element set mean what their
authors intended: a classical set quoting "M at epoch" moves on its Kepler
orbit, and a fitted set quoting "M(T)" is not given a second, double-counted
motion.

**Why nonzero, and not the term count.** `n_terms` is one count shared by all
six elements: that is how the protocol carries them. A body whose node drifts
secularly while its M is quoted at epoch must be sent with `n_terms` = 2 and
M's second coefficient zero. A rule keyed on the count would read that M as the
whole mean anomaly, and the body would stop moving along its orbit. That is
wrong by all of its motion, and nothing on the wire would flag it. A rule keyed
on whether a term was *written* fails the same way whenever a set is padded.
Only a nonzero test makes two encodings of the same body answer the same, so
zero-padding is meaningless by construction. That property matters to any
format with per-element coefficient lists, including the JSON element files
below. (An earlier approved wording keyed on the count; it was corrected on
2026-09-18, before release, when an implementation could not write a test for
its a(t) clause.)

**The mean motion comes from the Gaussian gravitational constant, not from
the loaded ephemeris.**

    k = 0.01720209895 rad/day   (IAU 1976)

    n = k / a(t)^1.5             rad/day, for a Sun-centred orbit

- **a is evaluated at the instant**, a(t) from its own polynomial, not taken
  at the epoch. This only matters when a has T-terms.
- **For an Earth-centred orbit**, the body is a massless satellite of the
  Earth, so n is divided by the square root of the Sun-to-Earth mass ratio,
  the Earth alone and not the Earth–Moon system:

      n_Earth = n / sqrt(M_sun / M_earth),   M_sun / M_earth = 332946.050895

The Gaussian constant makes an answer a function of the elements alone, so the
same elements give the same position whichever planetary ephemeris a server
has loaded. That is the property kind 4 exists to have. It is also the constant
these element sets were fitted with. The difference from DE440's GM of the Sun
is 1.7 × 10⁻¹⁰ relative, about 4 × 10⁻⁵″ per century on a Cupido-like orbit.
It matters for determinism, not accuracy.

The two constants live in one place, `include/prometheia/elements.hpp`, and
must match the protocol text exactly, since a server that uses its own best
value would disagree with one that uses the protocol's.

## Corrections

A body from elements takes the corrections exactly as a body does, when the
caller asks for them: light time, solved through the same two-body motion;
gravitational deflection by the Sun, skipped for an observer at the Sun's
centre; and annual aberration. The light-time equation is solved by
fixed-point iteration, as for an orbit point
([ORBIT-POINTS.md](ORBIT-POINTS.md)): a body this slow contracts the equation
by its radial speed over c each pass.

For the distant transneptunian points, the magnitudes follow the same pattern
orbit points showed. Light time retards a body by several hours of a motion of
about a degree a year, so it is worth a few arcseconds at most. Aberration is
worth up to the full 20.5″ constant. Rates are those of the returned position,
by the same central difference every body uses.

## Element files

Named bodies are defined in **JSON Lines** files: one body per line, each line
a JSON object, blank lines ignored. The format is Prometheia's own. It mirrors
the protocol's kind-4 fields one to one and borrows nothing from any other
program's element file.

    {"token":"example","name":"Example","set":"Worked example","citation":"docs/HYPOTHETICALS.md","epoch":2415020.0,"equinox":"J1900","origin":"sun","M":[123.4],"a":[41.0],"e":[0.0],"w":[0.0],"node":[0.0],"i":[0.0]}

| field | required | meaning |
|---|---|---|
| `token` | yes | the name a request uses: an A.15 token (`cupido` …) or any other lowercase identifier (a server MAY serve more) |
| `name` | no | the display name, e.g. `Cupido` |
| `set` | yes | the element set's short name; an answer's source string carries it, because for these bodies the set *is* the definition |
| `citation` | yes | where the numbers come from, precisely enough to check |
| `epoch` | yes | the elements' epoch, JD TT |
| `equinox` | yes | `"J2000"`, `"B1950"`, `"J1900"`, `"of date"`, or a number: an explicit equinox, JD TT |
| `origin` | no | the body the orbit is about: `"sun"` (the default) or `"earth"` — the protocol's *centre*, named apart from the observer's |
| `M`, `a`, `e`, `w`, `node`, `i` | yes | each a list of 1–5 coefficients of T⁰, T¹, …: mean anomaly (deg), semi-major axis (AU), eccentricity, argument of perihelion (deg), ascending node (deg), inclination (deg) |

The lists may differ in length. The body's term count is the longest, and
shorter lists are padded with zeros, which under the mean-anomaly rule above
changes nothing.

A token defined again, later in the same file or in a file added later, wins
over the earlier definition, as small-body catalogs do. An operator's file can
therefore override any shipped default without editing it.

## Named bodies

The element set this library ships is `data/hypotheticals.jsonl`, compiled
in. Every entry comes from a source clean under the data policy in
[DESIGN.md](DESIGN.md), which for these bodies means the original
publication. Each entry is transcribed from a page image, not from OCR, and
wherever the source prints a position it derived from its own elements, a
test (`shipped_bodies_reproduce_their_sources_own_check_positions`) holds
the transcription to it.

| token | source | status |
|---|---|---|
| `neptune-leverrier` | Le Verrier, *Comptes rendus* 23 (1846), p. 432 | shipped |
| `neptune-adams`, `pluto-lowell`, `isis-transpluto` | — | not planned (maintainer, 2026-09-18) |
| `cupido` … `poseidon` (the eight Hamburg points) | the Hamburg School's elements as distributed in Swiss's `seorbel.txt`, copy shipped with Astrolog (Witte and Sieggrün's planets, refined by Neely) | shipped; a recorded exception, [DESIGN.md](DESIGN.md) "Exposures" |
| the others in A.15 | — | no clean source found; served only from an operator's element file |

**The Hamburg points.** Eight one-term element sets at epoch J1900.0,
referred to the equinox of J1900, each moving at the Gaussian mean motion
from its semi-major axis, which is how the source means them. The maintainer
chose them so that a Uranian chart from Prometheia matches Astrolog and the
Swiss-based programs it will be compared with. They do match `swetest`, to
0.00076″ at 1900, 2000 and 2100 (`the_shipped_hamburg_points_match_swetest`).
Answers name the set "Hamburg School (Swiss Ephemeris seorbel.txt)", because
the set is the definition. A practitioner who works from another set
supplies it in an element file, and it wins.

**Le Verrier's Neptune.** He prints a = 36.154, e = 0.10761, longitude of
perihelion 284°45′ and mean longitude 318°47′ at 1847 January 1, "from the
equinox of 1 January 1847", with no inclination. Transcribed as follows:
- the orbit in the ecliptic (i = 0, Ω = 0), so ω is the longitude of
  perihelion and M = L − ϖ = 34°02′;
- the equinox as an explicit mean equinox at the epoch (the protocol's
  elements carry no true equinox);
- one term, so the body moves at the Gaussian mean motion. That gives a
  period of 217.392 years against his printed 217.387.

The source gives no hour for the epoch. 0h is taken, and half a day either
way is 8″ of this body's motion. His own derived position at the epoch is
326°32′ and 33.06; the transcription gives 326°31.3′ and 33.080. That
residual is a little larger than the rounding of the printed elements
explains. It is recorded, not tuned away: this is his orbit as printed.

**Not planned.** Adams's and Lowell's predicted planets and Transpluto
will not ship. The maintainer judged them historical curiosities nobody
needs (2026-09-18). Adams's and Lowell's papers are public domain and would
also each have needed an editorial choice the source doesn't make. Anyone
who wants them can define them in an element file.

**The others** (Nibiru, Harrington, Pickering's planets, Vulcan,
White Moon, Proserpina, Waldemath) have no clean numeric source found yet.
An operator who uses them supplies them in an element file.
