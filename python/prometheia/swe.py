# SPDX-License-Identifier: GPL-2.0-or-later
"""A pyswisseph-shaped interface to Prometheia (docs/PYTHON.md).

    import prometheia.swe as swe          # in place of: import swisseph as swe
    swe.set_ephe_path("ephe")             # a directory with DE440, or a file
    (lon, lat, dist, dlon, dlat, ddist), ret = swe.calc_ut(jd, swe.MARS)
    cusps, ascmc = swe.houses(jd, 47.37, 8.55, b"P")

The calls, their arguments, return shapes and constants follow pyswisseph's
documentation (constant values read from an installed pyswisseph, never its
source). The numbers are Prometheia's: they differ from the Swiss
Ephemeris' at the milliarcsecond level, for reasons measured in
docs/VALIDATION.md, and the answer always comes from the JPL ephemeris the
server loaded, whatever FLG_SWIEPH / FLG_MOSEPH / FLG_JPLEPH says.

A request this package cannot serve raises Error naming what is missing; it
never answers with zeros. Not in this release: eclipses, rise and set,
phenomena, heliacal events, houses from an explicit ARMC, house positions,
co-Ascendants.
"""
import math

from . import _calendar
from . import default_client, set_ephe_path as _set_ephe_path, close as _close
from ._errors import Error

# ---- constants (pyswisseph 2.10.03's values) --------------------------------

SUN, MOON, MERCURY, VENUS, MARS, JUPITER, SATURN, URANUS, NEPTUNE, PLUTO = range(10)
MEAN_NODE, TRUE_NODE, MEAN_APOG, OSCU_APOG, EARTH = 10, 11, 12, 13, 14
CHIRON, PHOLUS, CERES, PALLAS, JUNO, VESTA = 15, 16, 17, 18, 19, 20
INTP_APOG, INTP_PERG = 21, 22
NPLANETS = 23
ECL_NUT = -1
FIXSTAR = -10
AST_OFFSET = 10000
VARUNA = 30000
FICT_OFFSET = 40
FICT_OFFSET_1 = 39
FICT_MAX = 999
PLMOON_OFFSET = 9000
COMET_OFFSET = 1000
CUPIDO, HADES, ZEUS, KRONOS, APOLLON, ADMETOS, VULKANUS, POSEIDON = range(40, 48)
ISIS, NIBIRU, HARRINGTON, NEPTUNE_LEVERRIER, NEPTUNE_ADAMS = range(48, 53)
PLUTO_LOWELL, PLUTO_PICKERING, VULCAN, WHITE_MOON, PROSERPINA, WALDEMATH = range(53, 59)

FLG_JPLEPH = 1
FLG_SWIEPH = 2
FLG_MOSEPH = 4
FLG_DEFAULTEPH = FLG_SWIEPH
FLG_HELCTR = 8
FLG_TRUEPOS = 16
FLG_J2000 = 32
FLG_NONUT = 64
FLG_SPEED3 = 128
FLG_SPEED = 256
FLG_NOGDEFL = 512
FLG_NOABERR = 1024
FLG_ASTROMETRIC = FLG_NOABERR | FLG_NOGDEFL
FLG_EQUATORIAL = 2048
FLG_XYZ = 4096
FLG_RADIANS = 8192
FLG_BARYCTR = 16384
FLG_TOPOCTR = 32768
FLG_ORBEL_AA = FLG_TOPOCTR
FLG_TROPICAL = 0
FLG_SIDEREAL = 65536
FLG_ICRS = 131072
FLG_DPSIDEPS_1980 = 262144
FLG_JPLHOR = FLG_DPSIDEPS_1980
FLG_JPLHOR_APPROX = 524288
FLG_CENTER_BODY = 1048576

SIDBITS = 256
SIDBIT_ECL_T0 = 256
SIDBIT_SSY_PLANE = 512
SIDBIT_USER_UT = 1024
SIDBIT_ECL_DATE = 2048
SIDBIT_NO_PREC_OFFSET = 4096
SIDBIT_PREC_ORIG = 8192

(SIDM_FAGAN_BRADLEY, SIDM_LAHIRI, SIDM_DELUCE, SIDM_RAMAN, SIDM_USHASHASHI, SIDM_KRISHNAMURTI,
 SIDM_DJWHAL_KHUL, SIDM_YUKTESHWAR, SIDM_JN_BHASIN, SIDM_BABYL_KUGLER1, SIDM_BABYL_KUGLER2,
 SIDM_BABYL_KUGLER3, SIDM_BABYL_HUBER, SIDM_BABYL_ETPSC, SIDM_ALDEBARAN_15TAU, SIDM_HIPPARCHOS,
 SIDM_SASSANIAN, SIDM_GALCENT_0SAG, SIDM_J2000, SIDM_J1900, SIDM_B1950, SIDM_SURYASIDDHANTA,
 SIDM_SURYASIDDHANTA_MSUN, SIDM_ARYABHATA, SIDM_ARYABHATA_MSUN, SIDM_SS_REVATI, SIDM_SS_CITRA,
 SIDM_TRUE_CITRA, SIDM_TRUE_REVATI, SIDM_TRUE_PUSHYA, SIDM_GALCENT_RGILBRAND, SIDM_GALEQU_IAU1958,
 SIDM_GALEQU_TRUE, SIDM_GALEQU_MULA, SIDM_GALALIGN_MARDYKS, SIDM_TRUE_MULA,
 SIDM_GALCENT_MULA_WILHELM, SIDM_ARYABHATA_522, SIDM_BABYL_BRITTON, SIDM_TRUE_SHEORAN,
 SIDM_GALCENT_COCHRANE, SIDM_GALEQU_FIORENZA, SIDM_VALENS_MOON, SIDM_LAHIRI_1940,
 SIDM_LAHIRI_VP285, SIDM_KRISHNAMURTI_VP291, SIDM_LAHIRI_ICRC) = range(47)
SIDM_USER = 255
NSIDM_PREDEF = 47

JUL_CAL = _calendar.JUL_CAL
GREG_CAL = _calendar.GREG_CAL

ASC, MC, ARMC, VERTEX, EQUASC, COASC1, COASC2, POLASC = range(8)
NASCMC = 8

NODBIT_MEAN, NODBIT_OSCU, NODBIT_OSCU_BAR, NODBIT_FOPOINT = 1, 2, 4, 256

SPLIT_DEG_ROUND_SEC, SPLIT_DEG_ROUND_MIN, SPLIT_DEG_ROUND_DEG = 1, 2, 4
SPLIT_DEG_ZODIACAL, SPLIT_DEG_KEEP_SIGN, SPLIT_DEG_KEEP_DEG = 8, 16, 32
SPLIT_DEG_NAKSHATRA = 1024

ECL2HOR, EQU2HOR, HOR2ECL, HOR2EQU = 0, 1, 0, 1
TRUE_TO_APP, APP_TO_TRUE = 0, 1

AUNIT_TO_KM = 149597870.7
AUNIT_TO_LIGHTYEAR = 1.5812507409819728e-05
AUNIT_TO_PARSEC = 4.848136811095274e-06

version = "prometheia"

# ---- what the numbers mean here -----------------------------------------------

# Bodies 0..22 as the JSON surface names them.
_BODIES = {
    SUN: "Sun", MOON: "Moon", MERCURY: "Mercury", VENUS: "Venus", MARS: "Mars",
    JUPITER: "Jupiter", SATURN: "Saturn", URANUS: "Uranus", NEPTUNE: "Neptune", PLUTO: "Pluto",
    MEAN_NODE: "mean node", TRUE_NODE: "true node", MEAN_APOG: "lilith",
    OSCU_APOG: "true lilith", EARTH: "Earth", INTP_APOG: "natural apogee",
    INTP_PERG: "natural perigee",
}
# The numbered minor bodies pyswisseph gives their own numbers.
_MINOR = {CHIRON: 2060, PHOLUS: 5145, CERES: 1, PALLAS: 2, JUNO: 3, VESTA: 4}
_NAMES = {
    SUN: "Sun", MOON: "Moon", MERCURY: "Mercury", VENUS: "Venus", MARS: "Mars",
    JUPITER: "Jupiter", SATURN: "Saturn", URANUS: "Uranus", NEPTUNE: "Neptune", PLUTO: "Pluto",
    MEAN_NODE: "mean Node", TRUE_NODE: "true Node", MEAN_APOG: "mean Apogee",
    OSCU_APOG: "osc. Apogee", EARTH: "Earth", CHIRON: "Chiron", PHOLUS: "Pholus", CERES: "Ceres",
    PALLAS: "Pallas", JUNO: "Juno", VESTA: "Vesta", INTP_APOG: "intp. Apogee",
    INTP_PERG: "intp. Perigee",
}
# Protocol v4 A.15's named hypotheticals, FICT_OFFSET + index.
_HYPOTHETICALS = (
    "cupido", "hades", "zeus", "kronos", "apollon", "admetos", "vulcanus", "poseidon",
    "isis-transpluto", "nibiru", "harrington", "neptune-leverrier", "neptune-adams",
    "pluto-lowell", "pluto-pickering", "vulcan", "white-moon", "proserpina", "waldemath",
)
# Protocol v4 A.11's zodiac tokens: the index is the sidereal mode number.
_ZODIACS = (
    "fagan-bradley", "lahiri", "deluce", "raman", "usha-shashi", "krishnamurti",
    "djwhal-khul", "yukteshwar", "jn-bhasin", "babyl-kugler1", "babyl-kugler2",
    "babyl-kugler3", "babyl-huber", "babyl-etpsc", "aldebaran-15tau", "hipparchos",
    "sassanian", "galcent-0sag", "j2000", "j1900", "b1950", "suryasiddhanta",
    "suryasiddhanta-msun", "aryabhata", "aryabhata-msun", "ss-revati", "ss-citra",
    "true-citra", "true-revati", "true-pushya", "galcent-rgilbrand", "galequ-iau1958",
    "galequ-true", "galequ-mula", "galalign-mardyks", "true-mula", "galcent-mula-wilhelm",
    "aryabhata-522", "babyl-britton", "true-sheoran", "galcent-cochrane", "galequ-fiorenza",
    "valens-moon", "lahiri-1940", "lahiri-vp285", "krishnamurti-vp291", "lahiri-icrc",
)
_HOUSE_NAMES = {
    "P": "Placidus", "K": "Koch", "O": "Porphyry", "R": "Regiomontanus", "C": "Campanus",
    "A": "Equal", "E": "Equal", "W": "Whole Sign", "B": "Alcabitius", "M": "Morinus",
    "X": "Meridian", "T": "Topocentric",
}

_state = {"sid_mode": SIDM_FAGAN_BRADLEY, "t0": 0.0, "ayan_t0": 0.0, "topo": None}

# ---- setup ---------------------------------------------------------------------------


def set_ephe_path(path=None):
    """A JPL DE file, or a directory holding DE440 (and DE441 behind it)."""
    _set_ephe_path(path)


def set_jpl_file(name):
    """Accepted for compatibility: the file is chosen by set_ephe_path()."""
    del name


def close():
    _close()


def set_topo(lon, lat, alt=0.0):
    _state["topo"] = (float(lon), float(lat), float(alt))


def set_sid_mode(sidmode, t0=0.0, ayan_t0=0.0):
    _state.update(sid_mode=int(sidmode), t0=float(t0), ayan_t0=float(ayan_t0))


# ---- the request a flag set asks for --------------------------------------------------

_UNSUPPORTED_FLAGS = (
    (FLG_CENTER_BODY, "FLG_CENTER_BODY (the planet's centre; JPL's files hold Mars to Pluto "
                      "as system barycentres)"),
    (FLG_JPLHOR, "FLG_JPLHOR / FLG_DPSIDEPS_1980"),
    (FLG_JPLHOR_APPROX, "FLG_JPLHOR_APPROX"),
)


def _zodiac():
    mode = _state["sid_mode"]
    base, bits = mode & 0xFF, mode & ~0xFF
    for bit, why in ((SIDBIT_ECL_DATE, "SIDBIT_ECL_DATE"),
                     (SIDBIT_NO_PREC_OFFSET, "SIDBIT_NO_PREC_OFFSET"),
                     (SIDBIT_PREC_ORIG, "SIDBIT_PREC_ORIG")):
        if bits & bit:
            raise Error("%s is not served" % why)
    plane = "date"
    if bits & SIDBIT_ECL_T0:
        plane = "anchor"
    if bits & SIDBIT_SSY_PLANE:
        plane = "invariable"
    if base == SIDM_USER:
        t0 = _state["t0"]
        if bits & SIDBIT_USER_UT:
            t0 = default_client().convert_time(time={"jd_ut1": t0})["jd_tt"]
        zodiac = {"user": {"epoch_jd_tt": t0, "ayanamsa_deg": _state["ayan_t0"]}}
    elif 0 <= base < len(_ZODIACS):
        zodiac = _ZODIACS[base]
    else:
        raise Error("sidereal mode %d is not defined" % base)
    return zodiac, plane


def _request(flags):
    for bit, what in _UNSUPPORTED_FLAGS:
        if flags & bit:
            raise Error(what + " is not served")
    args = {}
    if flags & FLG_TOPOCTR:
        if _state["topo"] is None:
            raise Error("FLG_TOPOCTR needs set_topo() first")
        lon, lat, alt = _state["topo"]
        args["observer"] = "topocentric"
        args["site"] = {"lon_deg": lon, "lat_deg": lat, "height_m": alt}
    elif flags & FLG_HELCTR:
        args["observer"] = "heliocentric"
    elif flags & FLG_BARYCTR:
        args["observer"] = "barycentric"
    corrections = []
    if not flags & FLG_TRUEPOS:
        corrections.append("light-time")
    if not flags & FLG_NOGDEFL:
        corrections.append("gravitational-deflection")
    if not flags & FLG_NOABERR:
        corrections.append("aberration")
    args["corrections"] = corrections
    if flags & FLG_ICRS:
        args["frame"] = "icrf"
    elif flags & FLG_J2000:
        args["frame"] = "j2000"
    elif flags & FLG_NONUT:
        args["frame"] = "mean-of-date"
    if flags & FLG_EQUATORIAL:
        args["coordinates"] = "equatorial"
    if flags & FLG_SIDEREAL:
        args["zodiac"], args["sidereal_plane"] = _zodiac()
    args["rates"] = bool(flags & (FLG_SPEED | FLG_SPEED3))
    return args


def _retflags(flags):
    """What was done: the JPL ephemeris always; FLG_SPEED for either speed flag."""
    out = flags & ~(FLG_SWIEPH | FLG_MOSEPH | FLG_SPEED3)
    if flags & FLG_SPEED3:
        out |= FLG_SPEED
    return out | FLG_JPLEPH


def _object(planet):
    planet = int(planet)
    if planet in _BODIES:
        return _BODIES[planet]
    if planet in _MINOR:
        return {"asteroid": str(_MINOR[planet])}
    if planet > AST_OFFSET:
        return {"asteroid": str(planet - AST_OFFSET)}
    if FICT_OFFSET <= planet < FICT_OFFSET + len(_HYPOTHETICALS):
        return {"hypothetical": _HYPOTHETICALS[planet - FICT_OFFSET]}
    if planet == ECL_NUT:
        raise Error("ECL_NUT (obliquity and nutation) is not served by calc(); houses() "
                    "reports the true obliquity")
    raise Error("body %d is not served" % planet)


def _one(result):
    err = result.get("error")
    if err:
        raise Error(err["message"], err.get("code"))
    return result


def _time(jd, ut):
    return {"jd_ut1" if ut else "jd_tt": float(jd)}


def _six(row, flags):
    ecl = "longitude_deg" in row
    lon = row["longitude_deg" if ecl else "right_ascension_deg"]
    lat = row["latitude_deg" if ecl else "declination_deg"]
    dist = row.get("distance_au")
    dist = float("nan") if dist is None else dist
    rates = row.get("rates") or {}
    if flags & (FLG_SPEED | FLG_SPEED3):
        dlon = rates.get("longitude_deg_per_day" if ecl else "right_ascension_deg_per_day", 0.0)
        dlat = rates.get("latitude_deg_per_day" if ecl else "declination_deg_per_day", 0.0)
        ddist = rates.get("distance_au_per_day")
        ddist = float("nan") if ddist is None else ddist
    else:
        dlon = dlat = ddist = 0.0
    if flags & FLG_XYZ:
        return _to_xyz(lon, lat, dist, dlon, dlat, ddist)
    if flags & FLG_RADIANS:
        r = math.radians
        return (r(lon), r(lat), dist, r(dlon), r(dlat), ddist)
    return (lon, lat, dist, dlon, dlat, ddist)


def _to_xyz(lon, lat, dist, dlon, dlat, ddist):
    lo, la = math.radians(lon), math.radians(lat)
    dlo, dla = math.radians(dlon), math.radians(dlat)
    cl, sl, cb, sb = math.cos(lo), math.sin(lo), math.cos(la), math.sin(la)
    x, y, z = dist * cb * cl, dist * cb * sl, dist * sb
    vx = ddist * cb * cl - dist * sb * dla * cl - dist * cb * sl * dlo
    vy = ddist * cb * sl - dist * sb * dla * sl + dist * cb * cl * dlo
    vz = ddist * sb + dist * cb * dla
    return (x, y, z, vx, vy, vz)


def _calc(jd, planet, flags, ut):
    args = _request(flags)
    args["time"] = _time(jd, ut)
    args["objects"] = [_object(planet)]
    out = default_client().positions(**args)
    row = _one(out["results"][0])["rows"][0]
    return _six(row, flags), _retflags(flags)


# ---- positions ------------------------------------------------------------------------


def calc_ut(tjdut, planet, flags=FLG_SWIEPH | FLG_SPEED):
    """((lon, lat, dist, lon speed, lat speed, dist speed), retflags) at UT1."""
    return _calc(tjdut, planet, flags, True)


def calc(tjdet, planet, flags=FLG_SWIEPH | FLG_SPEED):
    """The same at TT (ephemeris time)."""
    return _calc(tjdet, planet, flags, False)


def get_planet_name(planet):
    planet = int(planet)
    if planet in _NAMES:
        return _NAMES[planet]
    if FICT_OFFSET <= planet < FICT_OFFSET + len(_HYPOTHETICALS):
        return _HYPOTHETICALS[planet - FICT_OFFSET].replace("-", " ").title()
    if planet > AST_OFFSET:
        found = default_client().lookup(query=str(planet - AST_OFFSET))
        for m in found.get("matches", []):
            if "name" in m:
                return m["name"]
    return ""


def _star(star, jd, flags, ut):
    args = _request(flags)
    args["time"] = _time(jd, ut)
    args["objects"] = [{"star": star}]
    result = _one(default_client().positions(**args)["results"][0])
    return _six(result["rows"][0], flags), result["object"]["resolved"], _retflags(flags)


def fixstar2_ut(star, tjdut, flags=FLG_SWIEPH):
    """((six values), resolved name, retflags) of a star by name or designation."""
    return _star(star, tjdut, flags, True)


def fixstar2(star, tjdet, flags=FLG_SWIEPH):
    return _star(star, tjdet, flags, False)


fixstar_ut = fixstar2_ut
fixstar = fixstar2


def nod_aps_ut(tjdut, planet, method=NODBIT_MEAN, flags=FLG_SWIEPH | FLG_SPEED):
    """Ascending node, descending node, perihelion, aphelion (four six-tuples)."""
    return _nod_aps(tjdut, planet, method, flags, True)


def nod_aps(tjdet, planet, method=NODBIT_MEAN, flags=FLG_SWIEPH | FLG_SPEED):
    return _nod_aps(tjdet, planet, method, flags, False)


def _nod_aps(jd, planet, method, flags, ut):
    if method & (NODBIT_OSCU_BAR | NODBIT_FOPOINT):
        raise Error("NODBIT_OSCU_BAR and NODBIT_FOPOINT are not served")
    body = _object(planet)
    if not isinstance(body, str) or planet in (MEAN_NODE, TRUE_NODE, MEAN_APOG, OSCU_APOG,
                                                 INTP_APOG, INTP_PERG):
        raise Error("nodes and apsides are served for the Moon and planets")
    how = "osculating" if method & NODBIT_OSCU else "mean"
    args = _request(flags)
    args["time"] = _time(jd, ut)
    args["objects"] = [{"point": p, "of": body, "method": how}
                       for p in ("ascending-node", "descending-node", "perihelion", "aphelion")]
    results = default_client().positions(**args)["results"]
    return tuple(_six(_one(r)["rows"][0], flags) for r in results)


# ---- sidereal -------------------------------------------------------------------------


def _ayanamsa(jd, ut, true):
    zodiac, plane = _zodiac()
    out = default_client().positions(time=_time(jd, ut), objects=["Sun"], zodiac=zodiac,
                                     sidereal_plane=plane, rates=False,
                                     frame="true-of-date" if true else "mean-of-date")
    return _one(out["results"][0])["rows"][0]["ayanamsa_deg"]


def get_ayanamsa_ut(tjdut):
    """The mean ayanamsha (no nutation) of the set_sid_mode() zodiac, at UT1."""
    return _ayanamsa(tjdut, True, False)


def get_ayanamsa(tjdet):
    return _ayanamsa(tjdet, False, False)


def get_ayanamsa_ex_ut(tjdut, flags):
    """(retflags, ayanamsha): the true one, or the mean one with FLG_NONUT."""
    return _retflags(flags), _ayanamsa(tjdut, True, not flags & FLG_NONUT)


def get_ayanamsa_ex(tjdet, flags):
    return _retflags(flags), _ayanamsa(tjdet, False, not flags & FLG_NONUT)


def get_ayanamsa_name(sidmode):
    base = int(sidmode) & 0xFF
    if 0 <= base < len(_ZODIACS):
        return _ZODIACS[base]
    return ""


# ---- houses ---------------------------------------------------------------------------


def _letter(hsys):
    if isinstance(hsys, (bytes, bytearray)):
        hsys = hsys.decode("ascii", "replace")
    if isinstance(hsys, int):
        hsys = chr(hsys)
    return str(hsys)[:1]


def _houses(tjdut, lat, lon, hsys, flags):
    args = {"time": {"jd_ut1": float(tjdut)},
            "site": {"lon_deg": float(lon), "lat_deg": float(lat)},
            "systems": [_letter(hsys)]}
    if flags & FLG_SIDEREAL:
        args["zodiac"], plane = _zodiac()
        if plane != "date":
            raise Error("sidereal houses are counted on the ecliptic of date only")
    row = _one(default_client().houses(**args)["results"][0])["rows"][0]
    nan = float("nan")
    # COASC1, COASC2 and POLASC are not served (docs/HOUSES.md): NaN, not 0.
    ascmc = (row["ascendant_deg"], row["mc_deg"], row["armc_deg"], row["vertex_deg"],
             row["equatorial_ascendant_deg"], nan, nan, nan)
    return tuple(row["cusps_deg"]), ascmc


def houses(tjdut, lat, lon, hsys=b"P"):
    """(12 cusps, 8 angles); ascmc[5:8] (co-Ascendants, polar Ascendant) are NaN."""
    return _houses(tjdut, lat, lon, hsys, 0)


def houses_ex(tjdut, lat, lon, hsys=b"P", flags=0):
    """houses() with FLG_SIDEREAL honoured (the set_sid_mode() zodiac)."""
    return _houses(tjdut, lat, lon, hsys, flags)


def house_name(hsys):
    return _HOUSE_NAMES.get(_letter(hsys), "")


def sidtime(tjdut):
    """Greenwich apparent sidereal time, hours (IAU 2006/2000A)."""
    row = _one(default_client().houses(time={"jd_ut1": float(tjdut)},
                                       site={"lon_deg": 0.0, "lat_deg": 0.0},
                                       systems=["O"])["results"][0])["rows"][0]
    return row["armc_deg"] / 15.0


# ---- time -----------------------------------------------------------------------------

julday = _calendar.julday
revjul = _calendar.revjul
day_of_week = _calendar.day_of_week


def _iso(year, month, day, hour, minutes, seconds, cal):
    if cal == JUL_CAL:
        jd = _calendar.julday(year, month, day, 0.0, JUL_CAL)
        year, month, day, _ = _calendar.revjul(jd, GREG_CAL)
    whole = int(math.floor(seconds))
    frac = ("%.9f" % (seconds - whole))[1:]
    sign = "-" if year < 0 else ""
    return "%s%04d-%02d-%02dT%02d:%02d:%02d%sZ" % (sign, abs(year), month, day, hour, minutes,
                                                 whole, frac.rstrip("0").rstrip("."))


def utc_to_jd(year, month, day, hour, minutes, seconds, cal=GREG_CAL):
    """(jd TT, jd UT1) of a UTC clock time; before 1972 the time is read as UT1."""
    _calendar._check(cal)
    out = default_client().convert_time(time=_iso(year, month, day, hour, minutes, seconds, cal))
    return out["jd_tt"], out["jd_ut1"]


def _civil(out, cal):
    stamp = out.get("utc") or out.get("ut1")
    date, clock = stamp.rstrip("Z").split("T")
    neg = date.startswith("-")
    y, m, d = (int(x) for x in date.lstrip("-").split("-"))
    y = -y if neg else y
    hh, mm, ss = clock.split(":")
    if cal == JUL_CAL:
        frac = (int(hh) + int(mm) / 60.0) / 24.0
        jd = _calendar.julday(y, m, d, 0.0, GREG_CAL)
        y, m, d, _ = _calendar.revjul(jd + frac, JUL_CAL)
    return y, m, d, int(hh), int(mm), float(ss)


def jdet_to_utc(tjdet, cal=GREG_CAL):
    _calendar._check(cal)
    return _civil(default_client().convert_time(time={"jd_tt": float(tjdet)}), cal)


def jdut1_to_utc(tjdut, cal=GREG_CAL):
    _calendar._check(cal)
    return _civil(default_client().convert_time(time={"jd_ut1": float(tjdut)}), cal)


def deltat(tjdut):
    """TT - UT1 in days (the server's delta T model, docs/TIME.md)."""
    return default_client().convert_time(time={"jd_ut1": float(tjdut)})["delta_t_s"] / 86400.0


def deltat_ex(tjdut, flag=FLG_SWIEPH):
    del flag
    return deltat(tjdut)


def utc_time_zone(year, month, day, hour, minutes, seconds, offset):
    """Local time to UTC with +offset hours (east positive), or back with -offset.

    Counted in seconds from the day's start, so whole minutes stay whole."""
    secs = hour * 3600 + minutes * 60 + seconds - offset * 3600.0
    days = math.floor(secs / 86400.0)
    secs -= days * 86400.0
    y, m, d, _ = _calendar.revjul(_calendar.julday(year, month, day, 0.0) + days)
    hh = int(secs // 3600)
    mi = int((secs - hh * 3600) // 60)
    return y, m, d, hh, mi, secs - hh * 3600 - mi * 60


# ---- arithmetic ------------------------------------------------------------------------


def degnorm(x):
    y = math.fmod(x, 360.0)
    if y < 0.0:
        y += 360.0
    return 0.0 if y >= 360.0 else y


def radnorm(x):
    y = math.fmod(x, 2.0 * math.pi)
    if y < 0.0:
        y += 2.0 * math.pi
    return 0.0 if y >= 2.0 * math.pi else y


def difdegn(p1, p2):
    return degnorm(p1 - p2)


def difdeg2n(p1, p2):
    d = degnorm(p1 - p2)
    return d - 360.0 if d >= 180.0 else d


def deg_midp(x1, x2):
    return degnorm(x2 + difdeg2n(x1, x2) / 2.0)


def cotrans(coord, eps):
    """Ecliptic to equatorial with eps < 0, equatorial to ecliptic with eps > 0 (degrees)."""
    lon, lat = math.radians(coord[0]), math.radians(coord[1])
    dist = coord[2] if len(coord) > 2 else 1.0
    e = math.radians(eps)
    x, y, z = math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)
    y2 = y * math.cos(e) + z * math.sin(e)
    z2 = -y * math.sin(e) + z * math.cos(e)
    return degnorm(math.degrees(math.atan2(y2, x))), math.degrees(math.asin(max(-1.0, min(1.0, z2)))), dist


def split_deg(degree, roundflag):
    """(deg, min, sec, secfr, sign), as pyswisseph gives them.

    Without SPLIT_DEG_ZODIACAL or _NAKSHATRA, ``sign`` is the number's sign
    (1 or -1) and the parts are of its absolute value; with one, it is the
    sign or nakshatra index. Rounding adds half a unit of the flag's place;
    then ``secfr`` is the whole seconds (as a float), otherwise the fraction
    of a second. KEEP_SIGN and KEEP_DEG keep the value from rounding into the
    next sign or degree."""
    if roundflag & SPLIT_DEG_ROUND_DEG:
        half = 0.5
    elif roundflag & SPLIT_DEG_ROUND_MIN:
        half = 0.5 / 60.0
    elif roundflag & SPLIT_DEG_ROUND_SEC:
        half = 0.5 / 3600.0
    else:
        half = 0.0
    sign = -1 if degree < 0 else 1
    x = abs(degree)
    y = x + half
    unit = count = None
    # NAKSHATRA wins over ZODIACAL for a positive number; a negative one is
    # never split into nakshatras, and takes signs if ZODIACAL is also set
    # (observed).
    if roundflag & SPLIT_DEG_NAKSHATRA and degree >= 0:
        unit, count = 13.33333333333333, 27  # the width pyswisseph rounds with (observed)
    elif roundflag & SPLIT_DEG_ZODIACAL:
        unit, count = 30.0, 12
    keep = 30.0 if unit is None else unit
    if roundflag & (SPLIT_DEG_KEEP_SIGN | SPLIT_DEG_KEEP_DEG) and int(y / keep) != int(x / keep):
        y = x
    # KEEP_DEG compares the degrees within the sign or nakshatra.
    if roundflag & SPLIT_DEG_KEEP_DEG and (
            int(y - int(y / keep) * keep if unit else y) != int(x - int(x / keep) * keep if unit
                                                              else x)):
        y = x
    if unit:
        # The index is not reduced past a full circle (400 deg is sign 13),
        # save that 12 (27) itself reads as 0 (observed).
        sign = int(y / unit)
        y -= sign * unit
        if sign == count:
            sign = 0
    d = int(y)
    rem = (y - d) * 60.0
    m = int(rem)
    rem = (rem - m) * 60.0
    sec = int(rem)
    return d, m, sec, float(sec) if half else rem - sec, sign


# ---- not in this release ------------------------------------------------------------


def _not_served(name, why="not in this release (docs/PYTHON.md)"):
    def f(*args, **kwargs):
        raise Error("%s: %s" % (name, why))
    f.__name__ = name
    return f


for _n in ("sol_eclipse_when_glob", "sol_eclipse_when_loc", "sol_eclipse_where",
           "sol_eclipse_how", "lun_eclipse_when", "lun_eclipse_when_loc", "lun_eclipse_how",
           "lun_occult_when_glob", "lun_occult_when_loc", "lun_occult_where", "rise_trans",
           "rise_trans_true_hor", "pheno", "pheno_ut", "heliacal_ut", "heliacal_pheno_ut",
           "vis_limit_mag", "houses_armc", "houses_armc_ex2", "houses_ex2", "house_pos",
           "gauquelin_sector", "fixstar2_mag", "fixstar_mag", "azalt", "azalt_rev", "refrac",
           "refrac_extended", "solcross", "solcross_ut", "mooncross", "mooncross_ut",
           "mooncross_node", "mooncross_node_ut", "helio_cross", "helio_cross_ut",
           "get_orbital_elements", "orbit_max_min_true_distance", "calc_pctr", "time_equ",
           "lmt_to_lat", "lat_to_lmt", "sidtime0", "set_delta_t_userdef", "set_tid_acc",
           "get_tid_acc", "set_lapse_rate"):
    globals()[_n] = _not_served(_n)
del _n
