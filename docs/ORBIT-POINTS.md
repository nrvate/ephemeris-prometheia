# Nodes, apsides and the correction bits

A node or an apsis is not a body. Nothing emits light from there, so "the
apparent place of Jupiter's ascending node" is a convention rather than an
observation, and a library has to choose one and say so.

Prometheia's choice: **the corrections apply to an orbit point exactly as they
apply to a body, when the caller asks for them.** `CalcOptions::light_time`,
`::deflection` and `::aberration` mean the same thing for `calc_orbit_point`
as for `calc`. A caller who wants the bare geometry passes
`CalcOptions::geometric()` and gets it exactly, at no cost.

The reason is what a node is *for*. It exists to be compared against the
positions of bodies — is the Moon on the node, where is the node in this
chart — and those positions are apparent. A node computed in a different
frame from the bodies it is compared with produces an error in the
comparison that nothing downstream can see or undo.

This was settled jointly with the Astrolog project for version 4 of the
ephemeris protocol; §3.5a of that spec carries the same rule, and the
protocol's per-object `corrApplied` byte reports which of the three a server
actually applied, so a client can tell when an engine could not.

## The magnitudes, and why they are worth knowing

Measured on DE440 at J2000.0, geocentric, ecliptic of date, in arcseconds
from the geometric point. `tests/test_engine_catalog.cpp` pins these.

| point | light time alone | with the observer's velocity |
|---|---|---|
| Jupiter ascending node (osculating) | 0.0003 | 20.837 |
| Jupiter ascending node (mean) | 0.006 | 20.843 |
| Saturn ascending node (osculating) | 0.001 | 20.145 |
| Jupiter perihelion (osculating) | 0.069 | 2.253 |
| Jupiter perihelion (mean) | 0.009 | 2.673 |
| Moon true node | 19.10 | 0.0029 |
| Moon mean node | 18.94 | 0.0031 |
| Moon osculating apogee | 18.41 | 0.0928 |

Two things in that table are easy to get wrong.

**For a distant orbit, light time is worth nothing and the observer's
velocity is worth everything.** A node is very nearly fixed in inertial space
— it moves with the orbit's slow precession, not with the body — so retarding
it by the 35 minutes light takes to arrive moves it by thousandths of an
arcsecond. What moves it 21″ is the observer's own motion. The two differ by
four orders of magnitude, so "corrections were applied" is not a useful
statement about an orbit point; which correction was applied is.

The perihelion row shows the same term wearing a disguise. 2.7″ is not a
smaller correction than 20.8″ — it is the same aberration constant times the
sine of the angle to the Earth's velocity apex, and Jupiter's perihelion at
J2000 happens to lie about 176° from that apex. The size of the number says
where the point is, not how much physics was applied to it.

**For the Moon it is the other way round, and the intermediate is a trap.**
The Moon's points are computed barycentrically, like everything else, so
retarding them drags in the Earth's orbital motion and light time alone moves
the node 19″. Aberration then very nearly takes it back, and the physically
meaningful answer is the 0.003″ that survives. Neither term means anything on
its own; only the sum does.

That matters for anyone comparing implementations. An engine that computed
lunar points geocentrically would reach the same small total from a small
light-time step and no aberration at all — a different route, the same
answer, and a different account of which corrections it applied. The test
pins the large intermediate as well as the total for exactly this reason: it
is what tells the two routes apart.

## How it is computed

`orbit_point_vector_at` in `src/engine.cpp` mirrors `vector_at`, the body
path, with one difference. A body's light-time equation is solved by Newton's
method because the body moves fast enough to matter; a node's contracts by
v/c per pass, so a fixed-point iteration reaches roundoff in about three
passes and no derivative is needed. Deflection and aberration are then the
same calls the body path makes, with the same guard: the deflection term is
skipped for an observer at the Sun or the barycentre, where it is not
defined.

## What this does not decide

Whether a *server* applies these is a separate question from whether it can.
See `docs/SERVER.md` for the protocol side: the answer reports what was
actually applied, per object, so an engine that cannot apply one of the three
says so rather than silently returning something else.
