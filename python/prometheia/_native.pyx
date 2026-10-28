# SPDX-License-Identifier: GPL-2.0-or-later
# cython: language_level=3, boundscheck=False, wraparound=False
"""The engine in process, through the C interface (docs/PYTHON.md, "Native").

A Dataset names the files; each thread asks it for a Context and calls the
engine through that. A Context is one engine: it caches what it read, and is
used from the thread that made it, never from two at once. Calls release
the GIL while the engine works.
"""
import threading

from cpython.pythread cimport PyThread_get_thread_ident
from cpython.mem cimport PyMem_Free, PyMem_Malloc
from libc.string cimport memcpy, memset

from prometheia cimport cprometheia as c

from ._errors import Error

if c.prometheia_abi_version() != c.PROMETHEIA_ABI_VERSION:
    raise ImportError("libprometheia ABI %d, this binding was built for %d"
                      % (c.prometheia_abi_version(), c.PROMETHEIA_ABI_VERSION))

# ---- Words (docs/JSON_API.md, "Words": the protocol v4 registry names) -----

OBSERVERS = {"geocentric": c.PROMETHEIA_CENTER_GEOCENTRIC,
             "topocentric": c.PROMETHEIA_CENTER_TOPOCENTRIC,
             "heliocentric": c.PROMETHEIA_CENTER_HELIOCENTRIC,
             "barycentric": c.PROMETHEIA_CENTER_BARYCENTRIC,
             "body": c.PROMETHEIA_CENTER_BODY}
FRAMES = {"icrf": c.PROMETHEIA_FRAME_ICRF, "j2000": c.PROMETHEIA_FRAME_J2000,
          "mean-of-date": c.PROMETHEIA_FRAME_MEAN_OF_DATE,
          "true-of-date": c.PROMETHEIA_FRAME_TRUE_OF_DATE}
COORDINATES = {"ecliptic": c.PROMETHEIA_COORDS_ECLIPTIC,
               "equatorial": c.PROMETHEIA_COORDS_EQUATORIAL}
# The A.11 tokens served, and "tropical".
ZODIACS = {"tropical": c.PROMETHEIA_SIDEREAL_TROPICAL,
           "fagan-bradley": c.PROMETHEIA_SIDEREAL_FAGAN_BRADLEY,
           "lahiri": c.PROMETHEIA_SIDEREAL_LAHIRI,
           "raman": c.PROMETHEIA_SIDEREAL_RAMAN,
           "galcent-0sag": c.PROMETHEIA_SIDEREAL_GALCENT_0SAG,
           "true-citra": c.PROMETHEIA_SIDEREAL_TRUE_CITRA,
           "true-revati": c.PROMETHEIA_SIDEREAL_TRUE_REVATI,
           "true-pushya": c.PROMETHEIA_SIDEREAL_TRUE_PUSHYA,
           "galcent-rgilbrand": c.PROMETHEIA_SIDEREAL_GALCENT_RGILBRAND,
           "galequ-iau1958": c.PROMETHEIA_SIDEREAL_GALEQU_IAU1958,
           "galequ-true": c.PROMETHEIA_SIDEREAL_GALEQU_TRUE,
           "galequ-mula": c.PROMETHEIA_SIDEREAL_GALEQU_MULA,
           "true-mula": c.PROMETHEIA_SIDEREAL_TRUE_MULA,
           "galcent-mula-wilhelm": c.PROMETHEIA_SIDEREAL_GALCENT_MULA_WILHELM,
           "galcent-cochrane": c.PROMETHEIA_SIDEREAL_GALCENT_COCHRANE}
SIDEREAL_PLANES = {"date": c.PROMETHEIA_SIDEREAL_PLANE_DATE,
                   "anchor": c.PROMETHEIA_SIDEREAL_PLANE_ANCHOR,
                   "invariable": c.PROMETHEIA_SIDEREAL_PLANE_INVARIABLE}
PRECESSION = {"iau2006": c.PROMETHEIA_PRECESSION_IAU2006,
              "vondrak2011": c.PROMETHEIA_PRECESSION_VONDRAK2011}
CORRECTIONS = ("light-time", "gravitational-deflection", "aberration")
_CORRECTION_SETS = {"apparent": CORRECTIONS, "astrometric": ("light-time",),
                    "geometric": ()}
POINTS = {"ascending-node": c.PROMETHEIA_ORBIT_ASCENDING_NODE,
          "descending-node": c.PROMETHEIA_ORBIT_DESCENDING_NODE,
          "perihelion": c.PROMETHEIA_ORBIT_PERIHELION,
          "aphelion": c.PROMETHEIA_ORBIT_APHELION}
METHODS = {"mean": c.PROMETHEIA_ELEMENTS_MEAN, "osculating": c.PROMETHEIA_ELEMENTS_OSCULATING,
           "interpolated": c.PROMETHEIA_ELEMENTS_INTERPOLATED}
# The A.22 tokens.
HOUSE_SYSTEMS = {"placidus": c.PROMETHEIA_HOUSES_PLACIDUS, "koch": c.PROMETHEIA_HOUSES_KOCH,
                 "porphyry": c.PROMETHEIA_HOUSES_PORPHYRY,
                 "regiomontanus": c.PROMETHEIA_HOUSES_REGIOMONTANUS,
                 "campanus": c.PROMETHEIA_HOUSES_CAMPANUS, "equal": c.PROMETHEIA_HOUSES_EQUAL,
                 "whole-sign": c.PROMETHEIA_HOUSES_WHOLE_SIGN,
                 "alcabitius": c.PROMETHEIA_HOUSES_ALCABITIUS,
                 "morinus": c.PROMETHEIA_HOUSES_MORINUS,
                 "meridian": c.PROMETHEIA_HOUSES_MERIDIAN,
                 "topocentric": c.PROMETHEIA_HOUSES_TOPOCENTRIC}
EVENTS = {"rise": c.PROMETHEIA_EVENT_RISE, "set": c.PROMETHEIA_EVENT_SET,
          "upper-transit": c.PROMETHEIA_EVENT_UPPER_TRANSIT,
          "lower-transit": c.PROMETHEIA_EVENT_LOWER_TRANSIT}
LIMBS = {"upper": c.PROMETHEIA_LIMB_UPPER, "centre": c.PROMETHEIA_LIMB_CENTRE,
         "lower": c.PROMETHEIA_LIMB_LOWER}
OUTCOMES = {c.PROMETHEIA_OUTCOME_FOUND: "found", c.PROMETHEIA_OUTCOME_ALWAYS_ABOVE: "always-above",
            c.PROMETHEIA_OUTCOME_ALWAYS_BELOW: "always-below",
            c.PROMETHEIA_OUTCOME_NOT_IN_WINDOW: "not-in-window"}
BODIES = {"sun": c.PROMETHEIA_SUN, "moon": c.PROMETHEIA_MOON, "mercury": c.PROMETHEIA_MERCURY,
          "venus": c.PROMETHEIA_VENUS, "earth": c.PROMETHEIA_EARTH, "mars": c.PROMETHEIA_MARS,
          "jupiter": c.PROMETHEIA_JUPITER, "saturn": c.PROMETHEIA_SATURN,
          "uranus": c.PROMETHEIA_URANUS, "neptune": c.PROMETHEIA_NEPTUNE,
          "pluto": c.PROMETHEIA_PLUTO, "emb": c.PROMETHEIA_EARTH_MOON_BARY,
          "ssb": c.PROMETHEIA_SOLAR_SYSTEM_BARY}

_STATUS = {c.PROMETHEIA_ERROR_IO: "io", c.PROMETHEIA_ERROR_FORMAT: "format",
           c.PROMETHEIA_ERROR_CORRUPTION: "corruption",
           c.PROMETHEIA_ERROR_ARGUMENT: "argument", c.PROMETHEIA_ERROR_NOT_FOUND: "not-found",
           c.PROMETHEIA_ERROR_INTERNAL: "internal", c.PROMETHEIA_ERROR_COVERAGE: "coverage"}


cdef int fail(c.prometheia_status st, c.prometheia_error* err) except -1:
    """Raises the engine's refusal: its sentence, and its status as Error.code
    ("argument", "coverage", "not-found", ...)."""
    raise Error((<bytes>err.message).decode("utf-8", "replace"), code=_STATUS.get(st, "internal"))


cdef int word(dict table, object value, str what) except? -1:
    try:
        return table[value]
    except (KeyError, TypeError):
        raise Error("%s %r is not one of: %s" % (what, value, ", ".join(sorted(table))),
                    code="argument") from None


cdef str text(const char* s):
    return "" if s == NULL else (<bytes>s).decode("utf-8", "replace")


# ---- Options -------------------------------------------------------------

cdef class Options:
    """What a position call means: the observer, frame, coordinates,
    corrections, zodiac and rates, in the JSON tools' words
    (docs/JSON_API.md). Built once and passed to any number of calls.

        Options(observer="topocentric", site=(8.55, 47.37, 500.0), zodiac="lahiri")

    site is (lon_deg, lat_deg[, height_m]) or {"lon_deg", "lat_deg",
    "height_m"}, geodetic, east positive; center is the observing body's
    NAIF id for observer="body"; zodiac is a token or
    {"user": {"epoch_jd_tt", "ayanamsa_deg"}}; corrections is "apparent",
    "astrometric", "geometric" or a collection of CORRECTIONS.
    """
    cdef c.prometheia_options o

    def __init__(self, *, observer="geocentric", site=None, center=None,
                 frame="true-of-date", coordinates="ecliptic", corrections="apparent",
                 zodiac="tropical", sidereal_plane="date", precession="vondrak2011",
                 rates=True, sigma=True):
        c.prometheia_options_init(&self.o)
        self.o.center = word(OBSERVERS, observer, "observer")
        self.o.frame = word(FRAMES, frame, "frame")
        self.o.coords = word(COORDINATES, coordinates, "coordinates")
        self.o.sidereal_plane = word(SIDEREAL_PLANES, sidereal_plane, "sidereal_plane")
        self.o.precession = word(PRECESSION, precession, "precession")
        if isinstance(zodiac, dict):
            user = zodiac.get("user") if len(zodiac) == 1 else None
            if not isinstance(user, dict) or set(user) != {"epoch_jd_tt", "ayanamsa_deg"}:
                raise Error('a user zodiac is {"user": {"epoch_jd_tt", "ayanamsa_deg"}}',
                            code="argument")
            self.o.sidereal = c.PROMETHEIA_SIDEREAL_USER
            self.o.sidereal_epoch_jd = user["epoch_jd_tt"]
            self.o.sidereal_ayanamsa_deg = user["ayanamsa_deg"]
        else:
            self.o.sidereal = word(ZODIACS, zodiac, "zodiac")
        wanted = _CORRECTION_SETS.get(corrections) if isinstance(corrections, str) else None
        if wanted is None:
            if isinstance(corrections, str):
                raise Error("corrections %r is not apparent, astrometric or geometric"
                            % corrections, code="argument")
            wanted = tuple(corrections)
            for w in wanted:
                if w not in CORRECTIONS:
                    raise Error("correction %r is not one of: %s" % (w, ", ".join(CORRECTIONS)),
                                code="argument")
        self.o.light_time = "light-time" in wanted
        self.o.deflection = "gravitational-deflection" in wanted
        self.o.aberration = "aberration" in wanted
        self.o.speed = bool(rates)
        self.o.sigma = bool(sigma)
        if site is not None:
            if isinstance(site, dict):
                lon, lat, h = site["lon_deg"], site["lat_deg"], site.get("height_m", 0.0)
            else:
                lon, lat, h = (tuple(site) + (0.0,))[:3]
            self.o.site_lon_deg, self.o.site_lat_deg, self.o.site_height_m = lon, lat, h
        elif observer == "topocentric":
            raise Error("a topocentric observer needs a site", code="argument")
        if center is not None:
            if observer != "body":
                raise Error("center is read only with observer=\"body\"", code="argument")
            self.o.center_body = center
        elif observer == "body":
            raise Error('observer="body" needs a center (a NAIF id)', code="argument")


cdef Options _defaults = Options()


cdef inline const c.prometheia_options* opts_of(Options options):
    return &(_defaults if options is None else options).o


# ---- Results -------------------------------------------------------------

cdef class Position:
    """One answer: longitude/right ascension, latitude/declination (degrees),
    distance (AU), their daily rates, the rectangular vector, and where the
    answer came from. sigma_arcsec and ayanamsa_deg are None where they do
    not apply."""
    cdef readonly double lon_deg, lat_deg, dist_au, lon_speed, lat_speed, dist_speed
    cdef readonly double light_time_days
    cdef readonly tuple xyz_au, vel_au_day
    cdef readonly object sigma_arcsec, ayanamsa_deg
    cdef readonly int denum
    cdef readonly str source

    def __repr__(self):
        return ("Position(lon_deg=%r, lat_deg=%r, dist_au=%r, source=%r)"
                % (self.lon_deg, self.lat_deg, self.dist_au, self.source))


cdef Position position(const c.prometheia_result* r):
    cdef Position p = Position.__new__(Position)
    p.lon_deg, p.lat_deg, p.dist_au = r.lon_deg, r.lat_deg, r.dist_au
    p.lon_speed, p.lat_speed, p.dist_speed = r.lon_speed, r.lat_speed, r.dist_speed
    p.light_time_days = r.light_time_days
    p.xyz_au = (r.xyz_au[0], r.xyz_au[1], r.xyz_au[2])
    p.vel_au_day = (r.vel_au_day[0], r.vel_au_day[1], r.vel_au_day[2])
    p.sigma_arcsec = r.sigma_arcsec if r.flags & c.PROMETHEIA_HAS_SIGMA else None
    p.ayanamsa_deg = r.ayanamsa_deg if r.flags & c.PROMETHEIA_HAS_AYANAMSA else None
    p.denum = r.denum
    p.source = text(r.source)
    return p


cdef class Houses:
    """Twelve cusps (cusps[0] is cusp 1) and the angles, degrees on the
    ecliptic of date (docs/HOUSES.md). ayanamsa_deg is None when tropical."""
    cdef readonly tuple cusps
    cdef readonly double asc_deg, mc_deg, armc_deg, vertex_deg, equatorial_asc_deg
    cdef readonly double obliquity_deg
    cdef readonly object ayanamsa_deg

    def __repr__(self):
        return "Houses(asc_deg=%r, mc_deg=%r, cusps=%r)" % (self.asc_deg, self.mc_deg, self.cusps)


cdef Houses houses_of(const c.prometheia_houses* h):
    cdef Houses out = Houses.__new__(Houses)
    out.cusps = tuple([h.cusp_deg[k] for k in range(12)])
    out.asc_deg, out.mc_deg, out.armc_deg = h.asc_deg, h.mc_deg, h.armc_deg
    out.vertex_deg, out.equatorial_asc_deg = h.vertex_deg, h.equatorial_asc_deg
    out.obliquity_deg = h.obliquity_deg
    out.ayanamsa_deg = h.ayanamsa_deg if h.flags & c.PROMETHEIA_HAS_AYANAMSA else None
    return out


cdef class Horizontal:
    """A direction in a site's horizon: azimuth from north through east and
    the true and apparent (refracted) altitudes, degrees, with the local
    apparent hour angle (docs/FRAMES.md, "The horizon")."""
    cdef readonly double azimuth_deg, altitude_deg, apparent_altitude_deg, hour_angle_deg

    def __repr__(self):
        return ("Horizontal(azimuth_deg=%r, altitude_deg=%r, apparent_altitude_deg=%r)"
                % (self.azimuth_deg, self.altitude_deg, self.apparent_altitude_deg))


cdef int atmosphere_of(object atmosphere, c.prometheia_atmosphere* out) except -1:
    """None: no atmosphere (returns 0). "standard", (pressure_hpa,
    temperature_c) or {"pressure_hpa", "temperature_c"}: fills out (1)."""
    if atmosphere is None:
        return 0
    c.prometheia_atmosphere_init(out)
    if atmosphere == "standard":
        return 1
    if isinstance(atmosphere, dict):
        out.pressure_hpa = atmosphere.get("pressure_hpa", out.pressure_hpa)
        out.temperature_c = atmosphere.get("temperature_c", out.temperature_c)
    elif isinstance(atmosphere, (tuple, list)) and len(atmosphere) == 2:
        out.pressure_hpa, out.temperature_c = atmosphere
    else:
        raise Error('an atmosphere is None, "standard", (pressure_hpa, temperature_c) or '
                    '{"pressure_hpa", "temperature_c"}', code="argument")
    return 1


cdef class Event:
    """An event search's answer: jd_ut1 (NaN unless found), and outcome:
    "found", "always-above", "always-below" or "not-in-window"."""
    cdef readonly double jd_ut1
    cdef readonly str outcome

    def __repr__(self):
        return "Event(jd_ut1=%r, outcome=%r)" % (self.jd_ut1, self.outcome)


KINDS = {"planet": c.PROMETHEIA_KIND_PLANET, "moon": c.PROMETHEIA_KIND_MOON,
         "lunar-point": c.PROMETHEIA_KIND_LUNAR_POINT,
         "orbit-point": c.PROMETHEIA_KIND_ORBIT_POINT,
         "hypothetical": c.PROMETHEIA_KIND_HYPOTHETICAL, "star": c.PROMETHEIA_KIND_STAR,
         "deep-sky": c.PROMETHEIA_KIND_DEEP_SKY, "asteroid": c.PROMETHEIA_KIND_ASTEROID,
         "comet": c.PROMETHEIA_KIND_COMET}
_KIND_NAMES = {v: k for k, v in KINDS.items()}
_STANDINGS = {c.PROMETHEIA_STANDING_NONE: "none", c.PROMETHEIA_STANDING_IAU: "iau",
              c.PROMETHEIA_STANDING_TRADITIONAL: "traditional",
              c.PROMETHEIA_STANDING_DEFINED: "defined"}
_QUALITIES = {c.PROMETHEIA_MATCH_EXACT: "exact", c.PROMETHEIA_MATCH_ALIAS: "alias",
              c.PROMETHEIA_MATCH_OLD_DESIGNATION: "old-designation",
              c.PROMETHEIA_MATCH_PREFIX: "prefix"}


cdef int object_of(object obj, c.prometheia_object* out, list keep) except -1:
    """A body by NAIF id or SPK-ID (int); ("star", index or name);
    ("hypothetical", token); ("orbit-point", body, point, method); or a
    registry id (str: "sb:136199"). keep holds the bytes while the call runs."""
    memset(out, 0, sizeof(out[0]))
    if isinstance(obj, int):
        out.kind, out.id = c.PROMETHEIA_OBJECT_BODY, obj
        return 0
    if isinstance(obj, str):
        token = obj.encode("utf-8")
        keep.append(token)
        out.kind, out.token = c.PROMETHEIA_OBJECT_ID, token
        return 0
    if isinstance(obj, tuple) and len(obj) == 4 and obj[0] == "orbit-point":
        word(POINTS, obj[2], "point")
        word(METHODS, obj[3], "method")
        token = ("%s:%s" % (obj[2], obj[3])).encode("utf-8")
        keep.append(token)
        out.kind, out.id, out.token = c.PROMETHEIA_OBJECT_ORBIT_POINT, obj[1], token
        return 0
    if isinstance(obj, tuple) and len(obj) == 2 and obj[0] == "star":
        out.kind = c.PROMETHEIA_OBJECT_STAR
        out.id = obj[1] if isinstance(obj[1], int) else star_find(obj[1])
        return 0
    if isinstance(obj, tuple) and len(obj) == 2 and obj[0] == "hypothetical":
        token = str(obj[1]).encode("utf-8")
        keep.append(token)
        out.kind, out.token = c.PROMETHEIA_OBJECT_HYPOTHETICAL, token
        return 0
    raise Error('an object is a NAIF id, ("star", index or name), ("hypothetical", token), '
                '("orbit-point", body, point, method) or a registry id', code="argument")


cdef class Phenomena:
    """What an object looks like from the observer (Context.phenomena;
    docs/PHENOMENA.md). None where a quantity does not apply;
    magnitude_model names the published formula, or why there is none."""
    cdef readonly object phase_angle_deg, illuminated_fraction, elongation_deg
    cdef readonly object diameter_arcsec, magnitude
    cdef readonly str magnitude_model

    def __repr__(self):
        return "Phenomena(phase %r, elongation %r, V %r)" % (
            self.phase_angle_deg, self.elongation_deg, self.magnitude)


_RANK = {"exact": 0, "old-designation": 0, "alias": 1, "prefix": 2}


cdef class Match:
    """One object a lookup found (Context.object_lookup)."""
    cdef readonly str id, kind, quality, name, matched
    cdef public bint ambiguous

    def __repr__(self):
        return "Match(%r, %s, %s%s)" % (self.id, self.kind, self.quality,
                                        ", ambiguous" if self.ambiguous else "")


cdef Match match_of(const c.prometheia_object_match* m):
    cdef Match out = Match.__new__(Match)
    out.id, out.kind = text(m.id), _KIND_NAMES[m.kind]
    out.quality, out.name, out.matched = _QUALITIES[m.quality], text(m.name), text(m.matched)
    out.ambiguous = False
    return out


cdef object nan_none(double x):
    return None if x != x else x


cdef class ObjectInfo:
    """What an object is (Context.object_info; docs/REGISTRY.md). Absent
    values are None. object is the form calc() and rise_set() take."""
    cdef readonly str id, kind, orbit_class, name, standing, standing_source
    cdef readonly object number
    cdef readonly str designation
    cdef readonly list provisional_designations
    cdef readonly object h_mag, g_slope, diameter_km, vmag, elements_epoch_jd
    cdef readonly object mean_motion_deg_day, sidereal_mean_motion_deg_day, period_days
    cdef readonly str source
    cdef readonly object object

    def __repr__(self):
        return "ObjectInfo(%r, %s, %r)" % (self.id, self.kind, self.name)


cdef ObjectInfo info_of(const c.prometheia_object_entry* i):
    cdef ObjectInfo out = ObjectInfo.__new__(ObjectInfo)
    out.id, out.kind = text(i.id), _KIND_NAMES[i.kind]
    out.orbit_class = text(i.orbit_class) or None
    out.name = text(i.name) or None
    out.standing, out.standing_source = _STANDINGS[i.standing], text(i.standing_source) or None
    out.number = i.number if i.number != 0 else None
    out.designation = text(i.designation) or None
    prov = text(i.provisional_designation)
    out.provisional_designations = [prov] if prov else []
    out.h_mag, out.g_slope = nan_none(i.h_mag), nan_none(i.g_slope)
    out.diameter_km, out.vmag = nan_none(i.diameter_km), nan_none(i.vmag)
    out.elements_epoch_jd = nan_none(i.elements_epoch_jd)
    out.mean_motion_deg_day = nan_none(i.mean_motion_deg_day)
    out.sidereal_mean_motion_deg_day = nan_none(i.sidereal_mean_motion_deg_day)
    out.period_days = nan_none(i.period_days)
    out.source = text(i.source) or None
    token = text(i.object_token)
    if i.object_kind == c.PROMETHEIA_OBJECT_STAR:
        out.object = ("star", i.object_id)
    elif i.object_kind == c.PROMETHEIA_OBJECT_HYPOTHETICAL:
        out.object = ("hypothetical", token)
    elif i.object_kind == c.PROMETHEIA_OBJECT_ORBIT_POINT:
        point, method = token.split(":")
        out.object = ("orbit-point", i.object_id, point, method)
    else:
        out.object = i.object_id
    return out


cdef bint is_ut(str scale) except -1:
    if scale == "tt":
        return False
    if scale == "ut1":
        return True
    raise Error("scale %r is not tt or ut1" % (scale,), code="argument")


# ---- Context -------------------------------------------------------------

_LUNAR_KINDS = {"total": c.PROMETHEIA_LUNAR_ECLIPSE_TOTAL,
                "partial": c.PROMETHEIA_LUNAR_ECLIPSE_PARTIAL,
                "penumbral": c.PROMETHEIA_LUNAR_ECLIPSE_PENUMBRAL}
_LUNAR_KIND_NAMES = {v: k for k, v in _LUNAR_KINDS.items()}


cdef class LunarEclipse:
    """A lunar eclipse (docs/ECLIPSES.md): kind ("total", "partial" or
    "penumbral"), saros_series and saros_member (0 when the series' start is
    not tabulated), the greatest eclipse jd_max_tt / jd_max_ut1, gamma (Earth
    radii, north positive), the umbral and penumbral magnitudes (the umbral
    one is negative when the Moon misses the umbra), the shadow's radii and
    the Moon's semi-diameter in degrees, and the contacts P1 U1 U2 U3 U4 P4 as
    tuples contacts_tt / contacts_ut1 (None where a contact does not occur)."""
    cdef readonly str kind
    cdef readonly int saros_series, saros_member
    cdef readonly double jd_max_tt, jd_max_ut1, gamma, umbral_magnitude, penumbral_magnitude
    cdef readonly double umbra_radius_deg, penumbra_radius_deg, moon_semidiameter_deg
    cdef readonly tuple contacts_tt, contacts_ut1

    def __repr__(self):
        return ("LunarEclipse(kind=%r, jd_max_tt=%r, saros=%d/%d, umbral_magnitude=%r)"
                % (self.kind, self.jd_max_tt, self.saros_series, self.saros_member,
                   self.umbral_magnitude))


cdef class LunarEclipseLocal:
    """An eclipse seen from a site: eclipse (LunarEclipse) and the Moon's
    topocentric altitude in degrees at P1 U1 U2 U3 U4 P4 and the greatest
    eclipse (seven values, None where the contact does not occur), true
    (altitude_deg) and with standard refraction (apparent_altitude_deg)."""
    cdef readonly LunarEclipse eclipse
    cdef readonly tuple altitude_deg, apparent_altitude_deg

    def __repr__(self):
        return "LunarEclipseLocal(eclipse=%r, altitude_deg=%r)" % (self.eclipse, self.altitude_deg)


cdef LunarEclipse lunar_eclipse_of(const c.prometheia_lunar_eclipse* x):
    cdef LunarEclipse r = LunarEclipse.__new__(LunarEclipse)
    cdef int k
    r.kind = _LUNAR_KIND_NAMES[x.kind]
    r.saros_series, r.saros_member = x.saros_series, x.saros_member
    r.jd_max_tt, r.jd_max_ut1, r.gamma = x.jd_max_tt, x.jd_max_ut1, x.gamma
    r.umbral_magnitude, r.penumbral_magnitude = x.umbral_magnitude, x.penumbral_magnitude
    r.umbra_radius_deg, r.penumbra_radius_deg = x.umbra_radius_deg, x.penumbra_radius_deg
    r.moon_semidiameter_deg = x.moon_semidiameter_deg
    r.contacts_tt = tuple([nan_none(x.contact_tt[k]) for k in range(6)])
    r.contacts_ut1 = tuple([nan_none(x.contact_ut1[k]) for k in range(6)])
    return r


cdef class Context:
    """One engine over a Dataset's files, for the thread that made it."""
    cdef c.prometheia_engine* e
    cdef unsigned long owner
    cdef readonly object dataset

    def __cinit__(self):
        self.e = NULL

    def __dealloc__(self):
        if self.e != NULL:
            c.prometheia_engine_close(self.e)
            self.e = NULL

    def close(self):
        """Frees the engine now; the context then refuses every call."""
        if self.e != NULL:
            c.prometheia_engine_close(self.e)
            self.e = NULL

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    cdef c.prometheia_engine* engine(self) except NULL:
        if self.e == NULL:
            raise Error("this context is closed", code="argument")
        if PyThread_get_thread_ident() != self.owner:
            raise Error("a context is used from the thread that made it; ask the dataset for "
                        "one per thread (Dataset.local())", code="argument")
        return self.e

    def calc(self, object body, double jd, Options options=None, str scale="tt"):
        """Any object at a TT or UT1 Julian date: a body by NAIF id
        (catalog bodies by SPK-ID; BODIES names the planets), ("star", index
        or name), ("hypothetical", token), ("orbit-point", body, point,
        method), or a registry id from object_lookup() ("sb:136199")."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef c.prometheia_result r
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef bint ut = is_ut(scale)
        cdef int naif
        cdef c.prometheia_object obj
        cdef list keep = []
        if type(body) is int:
            naif = body
            with nogil:
                st = (c.prometheia_calc_ut(e, naif, jd, o, &r, &err) if ut
                      else c.prometheia_calc(e, naif, jd, o, &r, &err))
        else:
            object_of(body, &obj, keep)
            with nogil:
                st = (c.prometheia_calc_object_ut(e, &obj, jd, o, &r, &err) if ut
                      else c.prometheia_calc_object(e, &obj, jd, o, &r, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return position(&r)

    def calc_star(self, star, double jd, Options options=None, str scale="tt"):
        """A fixed star or Messier object by catalog index or name."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef int index = star if isinstance(star, int) else star_find(star)
        cdef c.prometheia_result r
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef bint ut = is_ut(scale)
        with nogil:
            st = (c.prometheia_calc_star_ut(e, index, jd, o, &r, &err) if ut
                  else c.prometheia_calc_star(e, index, jd, o, &r, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return position(&r)

    def calc_hypothetical(self, str token, double jd, Options options=None, str scale="tt"):
        """A named hypothetical body by its token ("cupido"; hypotheticals())."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef bytes t = token.encode("utf-8")
        cdef const char* tp = t
        cdef c.prometheia_result r
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef bint ut = is_ut(scale)
        with nogil:
            st = (c.prometheia_calc_hypothetical_ut(e, tp, jd, o, &r, &err) if ut
                  else c.prometheia_calc_hypothetical(e, tp, jd, o, &r, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return position(&r)

    def calc_orbit_point(self, int body, str point, str method, double jd,
                         Options options=None, str scale="tt"):
        """A node or apsis of a body's orbit (POINTS, METHODS;
        docs/ORBIT-POINTS.md)."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef int pt = word(POINTS, point, "point"), m = word(METHODS, method, "method")
        cdef c.prometheia_result r
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef bint ut = is_ut(scale)
        with nogil:
            st = (c.prometheia_calc_orbit_point_ut(e, body, pt, m, jd, o, &r, &err) if ut
                  else c.prometheia_calc_orbit_point(e, body, pt, m, jd, o, &r, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return position(&r)

    def houses(self, str system, double jd, double lon_deg, double lat_deg,
               Options options=None, str scale="tt"):
        """Cusps and angles at a site (HOUSE_SYSTEMS; docs/HOUSES.md). Of
        the options only the zodiac and the precession model enter."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef int s = word(HOUSE_SYSTEMS, system, "house system")
        cdef c.prometheia_houses h
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef bint ut = is_ut(scale)
        with nogil:
            st = (c.prometheia_calc_houses_ut(e, s, jd, lon_deg, lat_deg, o, &h, &err) if ut
                  else c.prometheia_calc_houses(e, s, jd, lon_deg, lat_deg, o, &h, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return houses_of(&h)

    def sidereal_time(self, double jd_ut1, str kind="apparent", Options options=None):
        """Greenwich sidereal time as an angle, degrees: "mean" or
        "apparent"; add the east longitude for local sidereal time."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef int k = word({"mean": c.PROMETHEIA_SIDEREAL_TIME_MEAN,
                           "apparent": c.PROMETHEIA_SIDEREAL_TIME_APPARENT}, kind, "kind")
        cdef double out = 0.0
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_sidereal_time_ut(e, jd_ut1, k, o, &out, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return out

    def obliquity(self, double jd_tt, str kind="true", Options options=None):
        """The obliquity of the ecliptic, degrees: "mean" or "true"."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef int k = word({"mean": c.PROMETHEIA_OBLIQUITY_MEAN,
                           "true": c.PROMETHEIA_OBLIQUITY_TRUE}, kind, "kind")
        cdef double out = 0.0
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_obliquity(e, jd_tt, k, o, &out, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return out

    def horizontal(self, double jd_ut1, double lon_deg, double lat_deg, double ra_deg,
                   double dec_deg, atmosphere=None, double height_m=0.0, Options options=None):
        """A direction on the true equator and equinox of date (for a body,
        its topocentric apparent position from this site) in the site's
        horizon. atmosphere None: no refraction; "standard" or (hPa, C):
        Bennett's, down to the dip for height_m. Of the options only the
        precession model enters."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef c.prometheia_atmosphere air
        cdef const c.prometheia_atmosphere* a = &air if atmosphere_of(atmosphere, &air) else NULL
        cdef c.prometheia_horizontal h
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_horizontal_ut(e, jd_ut1, lon_deg, lat_deg, height_m, ra_deg,
                                            dec_deg, a, o, &h, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        cdef Horizontal out = Horizontal.__new__(Horizontal)
        out.azimuth_deg, out.altitude_deg = h.azimuth_deg, h.altitude_deg
        out.apparent_altitude_deg, out.hour_angle_deg = h.apparent_altitude_deg, h.hour_angle_deg
        return out

    def equatorial_from_horizontal(self, double jd_ut1, double lon_deg, double lat_deg,
                                   double azimuth_deg, double altitude_deg, bint apparent=True,
                                   atmosphere=None, double height_m=0.0, Options options=None):
        """(ra_deg, dec_deg) of date for an azimuth and altitude; with
        apparent and an atmosphere, the refraction is removed first."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef c.prometheia_atmosphere air
        cdef const c.prometheia_atmosphere* a = &air if atmosphere_of(atmosphere, &air) else NULL
        cdef double ra = 0.0, dec = 0.0
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_equatorial_from_horizontal_ut(e, jd_ut1, lon_deg, lat_deg, height_m,
                                                            azimuth_deg, altitude_deg, apparent,
                                                            a, o, &ra, &dec, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return ra, dec

    def rise_set(self, object, str event, double jd_ut1_start, double lon_deg, double lat_deg,
                 double height_m=0.0, *, str limb="upper", bint refraction=True,
                 atmosphere="standard", double dip_height_m=0.0, double window_days=2.0,
                 bint backward=False, Options options=None):
        """The first rise, set, upper- or lower-transit (EVENTS) strictly
        after jd_ut1_start (before, with backward) within window_days, at a
        site; height_m gives the parallax, dip_height_m the dip (default 0:
        the mathematical horizon). object is a NAIF id, ("star", index or
        name) or ("hypothetical", token). A window with no such event is
        not an error: see Event.outcome."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef c.prometheia_object obj
        cdef list keep = []
        object_of(object, &obj, keep)
        cdef int ev = word(EVENTS, event, "event")
        cdef c.prometheia_rise_set_options ro
        c.prometheia_rise_set_options_init(&ro)
        ro.limb = word(LIMBS, limb, "limb")
        ro.refraction = refraction
        if atmosphere is None:
            ro.refraction = 0
        else:
            atmosphere_of(atmosphere, &ro.atmosphere)
        ro.dip_height_m, ro.window_days, ro.backward = dip_height_m, window_days, backward
        cdef c.prometheia_event out
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_rise_set_ut(e, &obj, ev, jd_ut1_start, lon_deg, lat_deg, height_m,
                                          &ro, o, &out, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        cdef Event result = Event.__new__(Event)
        result.jd_ut1, result.outcome = out.jd_ut1, OUTCOMES[out.outcome]
        return result

    def lunar_eclipse_next(self, double jd, *, kinds=None, bint backward=False):
        """The first lunar eclipse after jd (a TT Julian date; before, with
        backward) of the kinds ("total", "partial", "penumbral": a name or a
        sequence of them; None: all), as a LunarEclipse. Error code
        "not-found" when none occurs within about 160 years."""
        cdef c.prometheia_engine* e = self.engine()
        cdef unsigned mask = 0
        if kinds is None:
            mask = 7
        elif isinstance(kinds, str):
            mask = word(_LUNAR_KINDS, kinds, "kind")
        else:
            for k in kinds:
                mask |= word(_LUNAR_KINDS, k, "kind")
        cdef c.prometheia_lunar_eclipse out
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = c.prometheia_lunar_eclipse_next(e, jd, mask, backward, &out, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return lunar_eclipse_of(&out)

    def lunar_eclipse_at(self, double jd_max, site=None):
        """The eclipse whose greatest is within two days of jd_max (TT) as
        a LunarEclipseLocal: its altitudes are seen from site, a (lon_deg,
        lat_deg) or (lon_deg, lat_deg, height_m) tuple, and None without
        one."""
        cdef c.prometheia_engine* e = self.engine()
        cdef c.prometheia_site s
        cdef const c.prometheia_site* sp = NULL
        if site is not None:
            if len(site) not in (2, 3):
                raise Error("site is (lon_deg, lat_deg[, height_m])", code="argument")
            s.lon_deg, s.lat_deg = site[0], site[1]
            s.height_m = site[2] if len(site) == 3 else 0.0
            sp = &s
        cdef c.prometheia_lunar_eclipse_local out
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        cdef int k
        with nogil:
            st = c.prometheia_lunar_eclipse_at(e, jd_max, sp, &out, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        cdef LunarEclipseLocal r = LunarEclipseLocal.__new__(LunarEclipseLocal)
        r.eclipse = lunar_eclipse_of(&out.eclipse)
        r.altitude_deg = tuple([nan_none(out.altitude_deg[k]) for k in range(7)])
        r.apparent_altitude_deg = tuple([nan_none(out.apparent_altitude_deg[k])
                                         for k in range(7)])
        return r

    def lookup(self, str name):
        """A catalog body's SPK-ID from its designation or name ("1",
        "Ceres"); Error code "not-found" when no loaded catalog has it."""
        cdef c.prometheia_engine* e = self.engine()
        cdef bytes n = name.encode("utf-8")
        cdef int body = 0
        cdef c.prometheia_error err
        cdef c.prometheia_status st = c.prometheia_engine_lookup(e, n, &body, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return body

    def phenomena(self, object body, double jd, Options options=None, str scale="tt"):
        """Phase angle, illuminated fraction, elongation, apparent diameter
        and V magnitude of any object calc() takes (docs/PHENOMENA.md)."""
        cdef c.prometheia_engine* e = self.engine()
        cdef const c.prometheia_options* o = opts_of(options)
        cdef c.prometheia_object obj
        cdef list keep = []
        object_of(body, &obj, keep)
        cdef bint ut = is_ut(scale)
        cdef c.prometheia_phenomena r
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        with nogil:
            st = (c.prometheia_calc_phenomena_ut(e, &obj, jd, o, &r, &err) if ut
                  else c.prometheia_calc_phenomena(e, &obj, jd, o, &r, &err))
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        cdef Phenomena out = Phenomena.__new__(Phenomena)
        out.phase_angle_deg, out.illuminated_fraction = (nan_none(r.phase_angle_deg),
                                                         nan_none(r.illuminated_fraction))
        out.elongation_deg, out.diameter_arcsec = (nan_none(r.elongation_deg),
                                                   nan_none(r.diameter_arcsec))
        out.magnitude, out.magnitude_model = nan_none(r.magnitude), text(r.magnitude_model)
        return out

    def object_lookup(self, str query, *, bint prefix=False, int limit=20):
        """Every object answering to query in every namespace, best first
        (docs/REGISTRY.md). Several matches of the best quality are an
        ambiguity, and each says so (Match.ambiguous); nothing is chosen."""
        cdef c.prometheia_engine* e = self.engine()
        if limit < 1:
            return []
        cdef bytes q = query.encode("utf-8")
        cdef c.prometheia_object_match* m = <c.prometheia_object_match*>PyMem_Malloc(
            limit * sizeof(c.prometheia_object_match))
        if m == NULL:
            raise MemoryError()
        cdef int n = 0
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        try:
            st = c.prometheia_object_lookup(e, q, c.PROMETHEIA_LOOKUP_PREFIX if prefix else 0,
                                            m, limit, &n, &err)
            if st != c.PROMETHEIA_OK:
                fail(st, &err)
            out = [match_of(&m[k]) for k in range(n)]
        finally:
            PyMem_Free(m)
        best = [x for x in out if _RANK[x.quality] == _RANK[out[0].quality]] if out else []
        if len(best) > 1 and out[0].quality != "prefix":
            for x in best:
                x.ambiguous = True
        return out

    def object_info(self, str id):
        """What an object is (ObjectInfo), by its registry id."""
        cdef c.prometheia_engine* e = self.engine()
        cdef bytes b = id.encode("utf-8")
        cdef c.prometheia_object_entry i
        cdef c.prometheia_error err
        cdef c.prometheia_status st = c.prometheia_object_info(e, b, &i, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
        return info_of(&i)

    def object_list(self, *, kind=None, orbit_class=None, bint named_only=False,
                    bint numbered_only=False, h_max=None, vmag_max=None, cursor=None,
                    int limit=500):
        """One page of registry ids passing every condition given, and the
        cursor of the next page (None after the last). kind is a KINDS name
        or a list of them."""
        cdef c.prometheia_engine* e = self.engine()
        cdef c.prometheia_object_filter f
        c.prometheia_object_filter_init(&f)
        if kind is not None:
            for k in ([kind] if isinstance(kind, str) else kind):
                f.kinds |= 1u << word(KINDS, k, "kind")
        cdef bytes cls
        if orbit_class is not None:
            cls = str(orbit_class).encode("ascii")
            if len(cls) > 7:
                raise Error("orbit class %r is not an SBDB code" % (orbit_class,),
                            code="argument")
            memcpy(f.orbit_class, <const char*>cls, len(cls))
        f.named_only, f.numbered_only = named_only, numbered_only
        if h_max is not None:
            f.h_max = h_max
        if vmag_max is not None:
            f.vmag_max = vmag_max
        cdef unsigned long long cur = 0 if cursor is None else cursor
        if limit < 1:
            raise Error("limit is at least 1", code="argument")
        cdef c.prometheia_object_match* m = <c.prometheia_object_match*>PyMem_Malloc(
            limit * sizeof(c.prometheia_object_match))
        if m == NULL:
            raise MemoryError()
        cdef int n = 0
        cdef c.prometheia_error err
        cdef c.prometheia_status st
        try:
            st = c.prometheia_object_list(e, &f, &cur, m, limit, &n, &err)
            if st != c.PROMETHEIA_OK:
                fail(st, &err)
            ids = [text(m[k].id) for k in range(n)]
        finally:
            PyMem_Free(m)
        return ids, (cur if cur != 0 else None)

    def hypotheticals(self):
        """The hypothetical tokens defined, in the order first defined."""
        cdef c.prometheia_engine* e = self.engine()
        return [text(c.prometheia_hypothetical_token(e, i))
                for i in range(c.prometheia_hypothetical_count(e))]

    @property
    def source(self):
        """The loaded ephemeris, every file in order."""
        return text(c.prometheia_engine_source(self.engine()))

    def ephemeris_spans(self):
        """[(description, first_jd_tdb, last_jd_tdb)], in the order the
        files answer."""
        cdef c.prometheia_engine* e = self.engine()
        cdef double a, b
        cdef const char* s
        cdef c.prometheia_error err
        out = []
        for i in range(c.prometheia_engine_ephemeris_count(e)):
            st = c.prometheia_engine_ephemeris_span(e, i, &a, &b, &s, &err)
            if st != c.PROMETHEIA_OK:
                fail(st, &err)
            out.append((text(s), a, b))
        return out

    def catalogs(self):
        """[(path, records)], in the order added; a later one wins."""
        cdef c.prometheia_engine* e = self.engine()
        cdef int n
        cdef const char* p
        cdef c.prometheia_error err
        out = []
        for i in range(c.prometheia_engine_catalog_count(e)):
            st = c.prometheia_engine_catalog_info(e, i, &n, &p, &err)
            if st != c.PROMETHEIA_OK:
                fail(st, &err)
            out.append((text(p), n))
        return out


cdef Context open_context(object dataset):
    cdef Context ctx = Context.__new__(Context)
    cdef c.prometheia_error err
    cdef c.prometheia_status st
    cdef bytes path = dataset.ephemeris.encode()
    st = c.prometheia_engine_open(path, &ctx.e, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    for p in dataset.ephemerides:
        path = p.encode()
        st = c.prometheia_engine_add_ephemeris(ctx.e, path, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
    for p in dataset.catalogs:
        path = p.encode()
        st = c.prometheia_engine_add_catalog(ctx.e, path, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
    for p in dataset.perturbers:
        path = p.encode()
        st = c.prometheia_engine_add_perturbers(ctx.e, path, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
    for p in dataset.hypotheticals:
        path = p.encode()
        st = c.prometheia_engine_add_hypotheticals(ctx.e, path, &err)
        if st != c.PROMETHEIA_OK:
            fail(st, &err)
    ctx.owner = PyThread_get_thread_ident()
    ctx.dataset = dataset
    return ctx


class Dataset:
    """The files a context answers from, in the order they are added:

        Dataset("ephe/linux_p1550p2650.440",
                ephemerides=["ephe/linux_m13000p17000.441"],  # behind DE440
                catalogs=["sbdb.epm"], perturbers=["ephe/sb441-n16.bsp"])

    context() opens a new engine over them; local() keeps one per thread.
    Until the engine's shared dataset lands (docs/PYTHON.md, "Native"), each
    context is an engine of its own and reads the files itself.
    """

    def __init__(self, ephemeris, *, ephemerides=(), catalogs=(), perturbers=(),
                 hypotheticals=()):
        self.ephemeris = str(ephemeris)
        self.ephemerides = tuple(str(p) for p in ephemerides)
        self.catalogs = tuple(str(p) for p in catalogs)
        self.perturbers = tuple(str(p) for p in perturbers)
        self.hypotheticals = tuple(str(p) for p in hypotheticals)
        self._local = threading.local()

    def context(self):
        """A new context, for the calling thread."""
        return open_context(self)

    def local(self):
        """This thread's context, opened on first use and kept."""
        ctx = getattr(self._local, "context", None)
        if ctx is None:
            ctx = self._local.context = open_context(self)
        return ctx


# ---- No engine needed ------------------------------------------------------

def version():
    return text(c.prometheia_version())


def abi_version():
    return c.prometheia_abi_version()


def houses_from_armc(str system, double armc_deg, double obliquity_deg, double lat_deg):
    """Tropical cusps and angles from an ARMC, the TRUE obliquity and a
    latitude, with no instant (docs/C_API.md, "Houses")."""
    cdef int s = word(HOUSE_SYSTEMS, system, "house system")
    cdef c.prometheia_houses h
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_houses_from_armc(s, armc_deg, obliquity_deg,
                                                                lat_deg, &h, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return houses_of(&h)


def ecliptic_to_equatorial(double lon_deg, double lat_deg, double obliquity_deg):
    """(ra_deg, dec_deg) of an ecliptic direction, rotated by the obliquity."""
    cdef double a = 0.0, b = 0.0
    c.prometheia_ecliptic_to_equatorial(lon_deg, lat_deg, obliquity_deg, &a, &b)
    return a, b


def equatorial_to_ecliptic(double ra_deg, double dec_deg, double obliquity_deg):
    """(lon_deg, lat_deg) of an equatorial direction, rotated by the obliquity."""
    cdef double a = 0.0, b = 0.0
    c.prometheia_equatorial_to_ecliptic(ra_deg, dec_deg, obliquity_deg, &a, &b)
    return a, b


def refraction(double altitude_deg, str from_="true", atmosphere="standard",
               double height_m=0.0):
    """Refraction R, degrees (Bennett 1982): from a "true" altitude,
    apparent = true + R; from an "apparent" one, true = apparent - R. Zero
    below the dip for height_m, or below -1 deg."""
    cdef int d = word({"true": c.PROMETHEIA_REFRACTION_FROM_TRUE,
                       "apparent": c.PROMETHEIA_REFRACTION_FROM_APPARENT}, from_, "from_")
    cdef c.prometheia_atmosphere air
    cdef const c.prometheia_atmosphere* a = &air if atmosphere_of(atmosphere, &air) else NULL
    cdef double out = 0.0
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_refraction(altitude_deg, d, a, height_m, &out,
                                                          &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return out


def utc_to_tt(int year, int month, int day, int hour=0, int minute=0, double second=0.0):
    """JD(TT) of a UTC instant, 1972 onward, through the leap-second table."""
    cdef double jd = 0.0
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_utc_to_tt(year, month, day, hour, minute, second,
                                                         &jd, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return jd


def tt_to_utc(double jd_tt):
    """(year, month, day, hour, minute, second, tai_minus_utc) of a JD(TT)."""
    cdef c.prometheia_utc u
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_tt_to_utc(jd_tt, &u, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return u.year, u.month, u.day, u.hour, u.minute, u.second, u.tai_minus_utc


def delta_t(double jd_tt):
    """TT - UT1, seconds: the observed USNO model (docs/TIME.md)."""
    return c.prometheia_delta_t(jd_tt)


def tdb_minus_tt(double jd_tt):
    """TDB - TT at the geocentre, seconds."""
    return c.prometheia_tdb_minus_tt(jd_tt)


def jd_from_ymdhms(int year, int month, int day, int hour=0, int minute=0, double second=0.0):
    """The JD of a proleptic Gregorian date and clock time, in the caller's scale."""
    return c.prometheia_jd_from_ymdhms(year, month, day, hour, minute, second)


def civil_from_jd(double jd):
    """(year, month, fractional day) of a JD."""
    cdef int y = 0, m = 0
    cdef double d = 0.0
    c.prometheia_civil_from_jd(jd, &y, &m, &d)
    return y, m, d


def star_find(str query):
    """The one catalog object a name or designation means, by index."""
    cdef bytes q = query.encode("utf-8")
    cdef int index = 0
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_star_find(q, &index, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return index


def star_lookup(str query, bint prefix=False, int limit=20):
    """[(index, quality, matched)], best first; quality "exact", "alias" or
    "prefix"."""
    cdef bytes q = query.encode("utf-8")
    cdef c.prometheia_star_match m[64]
    cdef int n = c.prometheia_star_lookup(q, prefix, m, min(max(limit, 0), 64))
    names = ("exact", "alias", "prefix")
    return [(m[i].index, names[m[i].quality], (<bytes>m[i].matched).decode("utf-8"))
            for i in range(n)]


def star_count():
    return c.prometheia_star_count()


def star_info(int index):
    """A catalog object's identifiers and astrometry, as a dict."""
    cdef c.prometheia_star s
    cdef c.prometheia_error err
    cdef c.prometheia_status st = c.prometheia_star_info(index, &s, &err)
    if st != c.PROMETHEIA_OK:
        fail(st, &err)
    return {"index": s.index, "kind": s.kind, "astrometry": s.astrometry, "hr": s.hr,
            "hd": s.hd, "hip": s.hip, "flamsteed": s.flamsteed, "messier": s.messier,
            "bayer": s.bayer, "bayer_index": s.bayer_index,
            "constellation": (<bytes>s.constellation).decode("utf-8"),
            "name": (<bytes>s.name).decode("utf-8"),
            "names": (<bytes>s.names).decode("utf-8").split("|"),
            "bayer_designation": (<bytes>s.bayer_designation).decode("utf-8"),
            "spectral_type": (<bytes>s.spectral_type).decode("utf-8"), "vmag": s.vmag,
            "ra_deg": s.ra_deg, "dec_deg": s.dec_deg, "epoch_jyear": s.epoch_jyear,
            "pm_ra_mas_yr": s.pm_ra_mas_yr, "pm_dec_mas_yr": s.pm_dec_mas_yr,
            "parallax_mas": s.parallax_mas, "rv_km_s": s.rv_km_s,
            "size_arcmin": s.size_arcmin}


def constellation_at(double ra_deg, double dec_deg):
    """The IAU constellation holding an ICRS direction."""
    return text(c.prometheia_constellation_at(ra_deg, dec_deg))
