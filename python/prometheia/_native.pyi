# SPDX-License-Identifier: GPL-2.0-or-later
# Types for prometheia._native (_native.pyx), the engine in process. The
# package re-exports every name here at prometheia.*; docs/PYTHON.md,
# "Native". python/tests/test_native.py holds this file to the module.
from typing import Any, Literal, Mapping, Optional, Sequence, Tuple, Union

Scale = Literal["tt", "ut1"]
Site = Union[Tuple[float, float], Tuple[float, float, float], Mapping[str, float]]
Atmosphere = Union[None, Literal["standard"], Tuple[float, float], Mapping[str, float]]

OBSERVERS: dict[str, int]
FRAMES: dict[str, int]
COORDINATES: dict[str, int]
ZODIACS: dict[str, int]
SIDEREAL_PLANES: dict[str, int]
PRECESSION: dict[str, int]
CORRECTIONS: Tuple[str, ...]
POINTS: dict[str, int]
METHODS: dict[str, int]
HOUSE_SYSTEMS: dict[str, int]
BODIES: dict[str, int]
EVENTS: dict[str, int]
LIMBS: dict[str, int]
OUTCOMES: dict[int, str]
KINDS: dict[str, int]
SOLAR_KINDS: dict[str, int]
Object = Union[int, str, Tuple[Literal["star"], Union[int, str]],
               Tuple[Literal["hypothetical"], str], Tuple[Literal["orbit-point"], int, str, str]]

class Match:
    """One object a lookup found (Context.object_lookup)."""
    id: str
    kind: str
    quality: str
    name: str
    matched: str
    ambiguous: bool

class Phenomena:
    """What an object looks like from the observer (Context.phenomena;
    docs/PHENOMENA.md). None where a quantity does not apply;
    magnitude_model names the published formula, or why there is none."""
    phase_angle_deg: Optional[float]
    illuminated_fraction: Optional[float]
    elongation_deg: Optional[float]
    diameter_arcsec: Optional[float]
    magnitude: Optional[float]
    magnitude_model: str

class ObjectInfo:
    """What an object is (Context.object_info; docs/REGISTRY.md). Absent
    values are None. object is the form calc() and rise_set() take."""
    id: str
    kind: str
    orbit_class: Optional[str]
    name: Optional[str]
    standing: str
    standing_source: Optional[str]
    number: Optional[int]
    designation: Optional[str]
    provisional_designations: list[str]
    h_mag: Optional[float]
    g_slope: Optional[float]
    diameter_km: Optional[float]
    vmag: Optional[float]
    elements_epoch_jd: Optional[float]
    mean_motion_deg_day: Optional[float]
    sidereal_mean_motion_deg_day: Optional[float]
    period_days: Optional[float]
    source: Optional[str]
    object: Object

class Options:
    """What a position call means, in the JSON tools' words; built once."""
    def __init__(self, *, observer: str = "geocentric", site: Optional[Site] = None,
                 center: Optional[int] = None, frame: str = "true-of-date",
                 coordinates: str = "ecliptic",
                 corrections: Union[str, Sequence[str]] = "apparent",
                 zodiac: Union[str, Mapping[str, Mapping[str, float]]] = "tropical",
                 sidereal_plane: str = "date", precession: str = "vondrak2011",
                 rates: bool = True, sigma: bool = True) -> None: ...

class Position:
    lon_deg: float
    lat_deg: float
    dist_au: float
    lon_speed: float
    lat_speed: float
    dist_speed: float
    light_time_days: float
    xyz_au: Tuple[float, float, float]
    vel_au_day: Tuple[float, float, float]
    sigma_arcsec: Optional[float]
    ayanamsa_deg: Optional[float]
    denum: int
    source: str

class Houses:
    cusps: Tuple[float, ...]
    asc_deg: float
    mc_deg: float
    armc_deg: float
    vertex_deg: float
    equatorial_asc_deg: float
    obliquity_deg: float
    ayanamsa_deg: Optional[float]

class Horizontal:
    azimuth_deg: float
    altitude_deg: float
    apparent_altitude_deg: float
    hour_angle_deg: float

class Besselian:
    """The Besselian elements at an instant (docs/SOLAR_ECLIPSES.md)."""
    x: float
    y: float
    d_deg: float
    mu_deg: float
    l1: float
    l2: float
    tan_f1: float
    tan_f2: float

class SolarEclipse:
    """A whole solar eclipse (Context.solar_eclipse_next). None marks what does
    not exist; times are Julian dates."""
    type: int
    kind: Literal["partial", "annular", "total", "hybrid"]
    central: Optional[bool]
    saros: int
    lunation: int
    max_tt: float
    max_ut1: float
    gamma: float
    magnitude: float
    magnitude_covered: float
    obscuration: float
    lat_deg: float
    lon_deg: float
    sun_alt_deg: float
    sun_az_deg: float
    path_width_km: Optional[float]
    central_duration_s: Optional[float]
    p1_tt: Optional[float]
    u1_tt: Optional[float]
    c1_tt: Optional[float]
    c2_tt: Optional[float]
    u4_tt: Optional[float]
    p4_tt: Optional[float]
    elements: Besselian

class SolarWhere:
    """The shadow axis on the ground (Context.solar_eclipse_where)."""
    jd_tt: float
    jd_ut1: float
    axis_hits: bool
    kind: Optional[Literal["annular", "total"]]
    lat_deg: Optional[float]
    lon_deg: Optional[float]
    north_lat_deg: Optional[float]
    north_lon_deg: Optional[float]
    south_lat_deg: Optional[float]
    south_lon_deg: Optional[float]
    sun_alt_deg: Optional[float]
    path_width_km: Optional[float]
    central_duration_s: Optional[float]
    magnitude: Optional[float]
    obscuration: Optional[float]
    elements: Besselian

class SolarLocal:
    """What a site sees (Context.solar_eclipse_local_next); contacts are C1..C4."""
    type: int
    kind: Literal["partial", "annular", "total"]
    saros: int
    lunation: int
    contact_tt: list[Optional[float]]
    contact_ut1: list[Optional[float]]
    contact_alt_deg: list[Optional[float]]
    contact_az_deg: list[Optional[float]]
    max_tt: float
    max_ut1: float
    max_alt_deg: float
    max_az_deg: float
    visible_max: bool
    magnitude: float
    obscuration: float
    diameter_ratio: float
    duration_s: Optional[float]
    gamma: float
    global_max_tt: float

class Event:
    jd_ut1: float
    outcome: Literal["found", "always-above", "always-below", "not-in-window"]

class LunarEclipse:
    kind: Literal["total", "partial", "penumbral"]
    saros_series: int
    saros_member: int
    jd_max_tt: float
    jd_max_ut1: float
    gamma: float
    umbral_magnitude: float
    penumbral_magnitude: float
    umbra_radius_deg: float
    penumbra_radius_deg: float
    moon_semidiameter_deg: float
    contacts_tt: Tuple[Optional[float], ...]
    contacts_ut1: Tuple[Optional[float], ...]

class LunarEclipseLocal:
    eclipse: LunarEclipse
    altitude_deg: Tuple[Optional[float], ...]
    apparent_altitude_deg: Tuple[Optional[float], ...]

class Context:
    """One engine over a Dataset's files, for the thread that made it."""
    dataset: "Dataset"
    @property
    def source(self) -> str: ...
    def close(self) -> None: ...
    def __enter__(self) -> "Context": ...
    def __exit__(self, *exc: Any) -> None: ...
    def calc(self, body: Object, jd: float, options: Optional[Options] = None,
             scale: Scale = "tt") -> Position: ...
    def calc_star(self, star: Union[int, str], jd: float, options: Optional[Options] = None,
                  scale: Scale = "tt") -> Position: ...
    def calc_hypothetical(self, token: str, jd: float, options: Optional[Options] = None,
                          scale: Scale = "tt") -> Position: ...
    def calc_orbit_point(self, body: int, point: str, method: str, jd: float,
                         options: Optional[Options] = None, scale: Scale = "tt") -> Position: ...
    def houses(self, system: str, jd: float, lon_deg: float, lat_deg: float,
               options: Optional[Options] = None, scale: Scale = "tt") -> Houses: ...
    def delta_t(self, jd_tt: float) -> float: ...
    def sidereal_time(self, jd_ut1: float, kind: Literal["mean", "apparent"] = "apparent",
                      options: Optional[Options] = None) -> float: ...
    def obliquity(self, jd_tt: float, kind: Literal["mean", "true"] = "true",
                  options: Optional[Options] = None) -> float: ...
    def horizontal(self, jd_ut1: float, lon_deg: float, lat_deg: float, ra_deg: float,
                   dec_deg: float, atmosphere: Atmosphere = None, height_m: float = 0.0,
                   options: Optional[Options] = None) -> Horizontal: ...
    def equatorial_from_horizontal(self, jd_ut1: float, lon_deg: float, lat_deg: float,
                                   azimuth_deg: float, altitude_deg: float,
                                   apparent: bool = True, atmosphere: Atmosphere = None,
                                   height_m: float = 0.0,
                                   options: Optional[Options] = None) -> Tuple[float, float]: ...
    def rise_set(self, object: Object, event: str, jd_ut1_start: float, lon_deg: float,
                 lat_deg: float, height_m: float = 0.0, *, limb: str = "upper",
                 refraction: bool = True, atmosphere: Atmosphere = "standard",
                 dip_height_m: float = 0.0, window_days: float = 2.0, backward: bool = False,
                 options: Optional[Options] = None) -> Event: ...
    def lunar_eclipse_next(self, jd: float, *, kinds: Union[None, str, Sequence[str]] = None,
                           backward: bool = False) -> LunarEclipse: ...
    def lunar_eclipse_at(self, jd_max: float,
                         site: Optional[Sequence[float]] = None) -> LunarEclipseLocal: ...
    def solar_eclipse_next(self, jd_start: float, kinds: Union[None, str, Sequence[str]] = None,
                           *, backward: bool = False, k_umbra: Optional[float] = None,
                           k_penumbra: Optional[float] = None,
                           sun_radius_km: Optional[float] = None) -> SolarEclipse: ...
    def solar_eclipse_where(self, jd_tt: float, *, k_umbra: Optional[float] = None,
                            k_penumbra: Optional[float] = None,
                            sun_radius_km: Optional[float] = None) -> SolarWhere: ...
    def solar_eclipse_local_next(self, jd_start: float, lon_deg: float, lat_deg: float,
                                 height_m: float = 0.0, *, backward: bool = False,
                                 k_umbra: Optional[float] = None,
                                 k_penumbra: Optional[float] = None,
                                 sun_radius_km: Optional[float] = None) -> SolarLocal: ...
    def lookup(self, name: str) -> int: ...
    def phenomena(self, body: Object, jd: float, options: Optional[Options] = None,
                  scale: Scale = "tt") -> Phenomena: ...
    def object_lookup(self, query: str, *, prefix: bool = False, limit: int = 20) -> list[Match]: ...
    def object_info(self, id: str) -> ObjectInfo: ...
    def object_list(self, *, kind: Union[None, str, Sequence[str]] = None,
                    orbit_class: Optional[str] = None, named_only: bool = False,
                    numbered_only: bool = False, h_max: Optional[float] = None,
                    vmag_max: Optional[float] = None, cursor: Optional[int] = None,
                    limit: int = 500) -> Tuple[list[str], Optional[int]]: ...
    def hypotheticals(self) -> list[str]: ...
    def ephemeris_spans(self) -> list[Tuple[str, float, float]]: ...
    def catalogs(self) -> list[Tuple[str, int]]: ...

class Dataset:
    """The files a context answers from, in the order they are added."""
    ephemeris: str
    ephemerides: Tuple[str, ...]
    catalogs: Tuple[str, ...]
    perturbers: Tuple[str, ...]
    hypotheticals: Tuple[str, ...]
    def __init__(self, ephemeris: str, *, ephemerides: Sequence[str] = (),
                 catalogs: Sequence[str] = (), perturbers: Sequence[str] = (),
                 hypotheticals: Sequence[str] = ()) -> None: ...
    def context(self) -> Context: ...
    def local(self) -> Context: ...

def version() -> str: ...
def abi_version() -> int: ...
def houses_from_armc(system: str, armc_deg: float, obliquity_deg: float,
                     lat_deg: float) -> Houses: ...
def ecliptic_to_equatorial(lon_deg: float, lat_deg: float,
                           obliquity_deg: float) -> Tuple[float, float]: ...
def equatorial_to_ecliptic(ra_deg: float, dec_deg: float,
                           obliquity_deg: float) -> Tuple[float, float]: ...
def refraction(altitude_deg: float, from_: Literal["true", "apparent"] = "true",
               atmosphere: Atmosphere = "standard", height_m: float = 0.0) -> float: ...
def utc_to_tt(year: int, month: int, day: int, hour: int = 0, minute: int = 0,
              second: float = 0.0) -> float: ...
def tt_to_utc(jd_tt: float) -> Tuple[int, int, int, int, int, float, float]: ...
def delta_t(jd_tt: float) -> float: ...
def tdb_minus_tt(jd_tt: float) -> float: ...
def jd_from_ymdhms(year: int, month: int, day: int, hour: int = 0, minute: int = 0,
                   second: float = 0.0) -> float: ...
def civil_from_jd(jd: float) -> Tuple[int, int, float]: ...
def star_find(query: str) -> int: ...
def star_lookup(query: str, prefix: bool = False,
                limit: int = 20) -> list[Tuple[int, str, str]]: ...
def star_count() -> int: ...
def star_info(index: int) -> dict[str, Any]: ...
def constellation_at(ra_deg: float, dec_deg: float) -> str: ...
