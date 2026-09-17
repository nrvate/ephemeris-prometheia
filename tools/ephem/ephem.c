/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ephem: positions from the command line.
 *
 * The first consumer of the C interface (include/prometheia/prometheia.h)
 * and the project's debugging tool. Written in strict C99 against the
 * public header only, so every feature it shows is reachable from C.
 * Usage: ephem --help; documentation: docs/EPHEM.md.
 */
#include <prometheia/prometheia.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CATALOGS 32
#define MAX_BODIES 256
#define MAX_ROWS 1000000L

enum { EXIT_PARTIAL = 1, EXIT_USAGE = 2 };
enum { FORMAT_TABLE, FORMAT_CSV, FORMAT_JSON };
enum { SCALE_DEFAULT, SCALE_UTC, SCALE_TT, SCALE_UT1 };

typedef struct named_body {
    const char* name;
    int id;
    const char* label;
} named_body;

static const named_body kNamedBodies[] = {
    {"sun", PROMETHEIA_SUN, "Sun"},
    {"moon", PROMETHEIA_MOON, "Moon"},
    {"mercury", PROMETHEIA_MERCURY, "Mercury"},
    {"venus", PROMETHEIA_VENUS, "Venus"},
    {"earth", PROMETHEIA_EARTH, "Earth"},
    {"mars", PROMETHEIA_MARS, "Mars"},
    {"jupiter", PROMETHEIA_JUPITER, "Jupiter"},
    {"saturn", PROMETHEIA_SATURN, "Saturn"},
    {"uranus", PROMETHEIA_URANUS, "Uranus"},
    {"neptune", PROMETHEIA_NEPTUNE, "Neptune"},
    {"pluto", PROMETHEIA_PLUTO, "Pluto"},
    {"emb", PROMETHEIA_EARTH_MOON_BARY, "EMB"},
    {"ssb", PROMETHEIA_SOLAR_SYSTEM_BARY, "SSB"},
};

static const char* const kDefaultBodies[] = {"sun",     "moon",   "mercury", "venus",   "mars",
                                             "jupiter", "saturn", "uranus",  "neptune", "pluto"};

typedef struct config {
    const char* ephemeris;
    const char* catalogs[MAX_CATALOGS];
    int n_catalogs;
    const char* bodies[MAX_BODIES];
    int n_bodies;

    int scale;
    int have_time; /* a calendar instant was given (or "now") */
    int year, month, day, hour, minute;
    double second;
    int have_jd; /* --jd */
    double jd;
    double step_days;
    long count;

    int format;
    int dms;
    int have_delta_t;
    double delta_t;
    prometheia_options opts;
} config;

typedef struct body_ref {
    int id;
    const char* label;
} body_ref;

/* ---- Small utilities ------------------------------------------------- */

static const char* g_program = "ephem";

static int usage_error(const char* fmt, const char* arg) {
    fprintf(stderr, "%s: ", g_program);
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n%s: try '%s --help'\n", g_program, g_program);
    return EXIT_USAGE;
}

static int equals_nocase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int parse_double(const char* s, double* out) {
    char* end;
    double v;
    if (!s || !*s)
        return 0;
    v = strtod(s, &end);
    if (*end != '\0' || !isfinite(v))
        return 0;
    *out = v;
    return 1;
}

static int parse_long(const char* s, long* out) {
    char* end;
    long v;
    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end != '\0')
        return 0;
    *out = v;
    return 1;
}

static int all_digits(const char* s) {
    if (*s == '-' || *s == '+')
        ++s;
    if (!*s)
        return 0;
    for (; *s; ++s)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

/* "[-]YYYY-MM-DD[(T| )HH:MM[:SS[.fff]]][Z]" */
static int parse_calendar(const char* s, config* c) {
    int n = 0, y, mo, d, h = 0, mi = 0, yy, mm;
    double sec = 0.0, day;
    const char* p;
    if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &n) != 3)
        return 0;
    p = s + n;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        ++p;
        if (sscanf(p, "%d:%d%n", &h, &mi, &n) != 2)
            return 0;
        p += n;
        if (*p == ':') {
            ++p;
            if (!isdigit((unsigned char)*p) || sscanf(p, "%lf%n", &sec, &n) != 1)
                return 0;
            p += n;
        }
    }
    if (*p == 'Z' || *p == 'z')
        ++p;
    if (*p != '\0')
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        !(sec >= 0.0 && sec < 61.0))
        return 0;
    /* Reject dates the month does not have (the UTC path checks too). */
    prometheia_civil_from_jd(prometheia_jd_from_ymdhms(y, mo, d, 0, 0, 0.0), &yy, &mm, &day);
    if (yy != y || mm != mo || (int)floor(day) != d)
        return 0;
    c->year = y;
    c->month = mo;
    c->day = d;
    c->hour = h;
    c->minute = mi;
    c->second = sec;
    c->have_time = 1;
    return 1;
}

/* "1.5", "1.5d", "6h", "30m", "10s" -> days */
static int parse_step(const char* s, double* days) {
    char buf[64];
    size_t n = strlen(s);
    double unit = 1.0, v;
    if (n == 0 || n >= sizeof buf)
        return 0;
    memcpy(buf, s, n + 1);
    switch (buf[n - 1]) {
    case 'd':
        unit = 1.0;
        buf[n - 1] = '\0';
        break;
    case 'h':
        unit = 1.0 / 24.0;
        buf[n - 1] = '\0';
        break;
    case 'm':
        unit = 1.0 / 1440.0;
        buf[n - 1] = '\0';
        break;
    case 's':
        unit = 1.0 / 86400.0;
        buf[n - 1] = '\0';
        break;
    default:
        break;
    }
    if (!parse_double(buf, &v))
        return 0;
    *days = v * unit;
    return 1;
}

/* "LON,LAT[,HEIGHT]" in degrees and metres */
static int parse_site(const char* s, prometheia_options* o) {
    double lon, lat, h = 0.0;
    int n = 0, k = sscanf(s, "%lf,%lf%n", &lon, &lat, &n);
    if (k != 2)
        return 0;
    if (s[n] == ',') {
        if (!parse_double(s + n + 1, &h))
            return 0;
    } else if (s[n] != '\0') {
        return 0;
    }
    if (!isfinite(lon) || !isfinite(lat) || lat < -90.0 || lat > 90.0)
        return 0;
    o->site_lon_deg = lon;
    o->site_lat_deg = lat;
    o->site_height_m = h;
    return 1;
}

/* "fagan-bradley" | "fb" | "lahiri" | "user:JD:DEG" | "tropical" */
static int parse_sidereal(const char* s, prometheia_options* o) {
    if (equals_nocase(s, "tropical")) {
        o->sidereal = PROMETHEIA_SIDEREAL_TROPICAL;
        return 1;
    }
    if (equals_nocase(s, "fagan-bradley") || equals_nocase(s, "fb")) {
        o->sidereal = PROMETHEIA_SIDEREAL_FAGAN_BRADLEY;
        return 1;
    }
    if (equals_nocase(s, "lahiri")) {
        o->sidereal = PROMETHEIA_SIDEREAL_LAHIRI;
        return 1;
    }
    if (strncmp(s, "user:", 5) == 0) {
        char buf[128];
        char* colon;
        size_t n = strlen(s + 5);
        if (n >= sizeof buf)
            return 0;
        memcpy(buf, s + 5, n + 1);
        colon = strchr(buf, ':');
        if (!colon)
            return 0;
        *colon = '\0';
        if (!parse_double(buf, &o->sidereal_epoch_jd) ||
            !parse_double(colon + 1, &o->sidereal_ayanamsa_deg))
            return 0;
        o->sidereal = PROMETHEIA_SIDEREAL_USER;
        return 1;
    }
    return 0;
}

static int parse_keyword(const char* s, const char* const names[], const int values[], int n,
                         int* out) {
    int i;
    for (i = 0; i < n; ++i) {
        if (equals_nocase(s, names[i])) {
            *out = values[i];
            return 1;
        }
    }
    return 0;
}

/* ---- Help ------------------------------------------------------------- */

static void print_help(void) {
    printf("Usage: %s [options] [BODY...]\n"
           "\n"
           "Positions of solar-system bodies from a JPL DE binary or SPK kernel, plus\n"
           "small bodies from EPM1 catalogs. Default bodies: Sun, Moon, Mercury..Pluto.\n"
           "\n"
           "Bodies:\n"
           "  sun moon mercury venus earth mars jupiter saturn uranus neptune pluto\n"
           "  emb ssb        Earth-Moon and solar-system barycentres\n"
           "  INTEGER        a NAIF ID / SPK-ID (Mars..Pluto IDs 4-9 are system barycentres)\n"
           "  NAME           a catalog name or designation (Ceres, \"2004 MN4\")\n"
           "  @TEXT          force a catalog lookup (@1 is Ceres; plain 1 is NAIF 1)\n"
           "\n"
           "Data:\n"
           "  -e, --ephemeris FILE   planetary ephemeris (default: $PROMETHEIA_EPHEMERIS)\n"
           "  -c, --catalog FILE     add an EPM1 catalog; repeatable, later ones win\n"
           "                         ($PROMETHEIA_CATALOGS, ':'-separated, is added first)\n"
           "\n"
           "Time (default: now):\n"
           "  -t, --time WHEN        YYYY-MM-DD[THH:MM[:SS.s]] or 'now'; UTC unless --scale\n"
           "  -j, --jd JD            a Julian date, TT unless --scale ut1\n"
           "      --scale utc|tt|ut1 the time scale of --time/--jd\n"
           "  -n, --count N          N rows, stepping by --step (default 1)\n"
           "  -s, --step STEP        days, or with a unit: 1d 6h 30m 10s (default 1d);\n"
           "                         uniform in TT (in UT1 with --scale ut1)\n"
           "      --delta-t SECONDS  fixed TT-UT1 instead of the observed USNO model\n"
           "\n"
           "Observer and frame:\n"
           "      --center geo|topo|helio|bary   (default geo)\n"
           "      --site LON,LAT[,H] geodetic degrees east/north, metres; implies topo\n"
           "      --frame true|mean|j2000|icrf   equinox of date (default true)\n"
           "      --equatorial       right ascension/declination instead of ecliptic\n"
           "      --sidereal MODE    fagan-bradley (fb), lahiri, user:JD:DEG, tropical\n"
           "\n"
           "Corrections (default: apparent place, with rates):\n"
           "      --astrometric      light time only\n"
           "      --geometric        no light time, deflection or aberration\n"
           "      --no-light-time, --no-deflection, --no-aberration, --no-speed\n"
           "      --no-sigma         skip catalog uncertainties (much faster for small bodies)\n"
           "\n"
           "Output:\n"
           "  -f, --format table|csv|json  (default table)\n"
           "      --dms              table angles as degrees/hours, minutes, seconds\n"
           "  -h, --help             this text\n"
           "  -V, --version          library version\n"
           "\n"
           "Exit status: 0 success, 1 some body failed (others still printed), 2 usage\n"
           "or setup error.\n",
           g_program);
}

/* ---- Argument parsing -------------------------------------------------- */

/* One command-line option: "-x value", "--long value" or "--long=value". */
typedef struct arg_cursor {
    int argc;
    char** argv;
    int i;
    char name[64]; /* the option name without any "=value" */
    const char* inline_value;
} arg_cursor;

static int is_opt(const arg_cursor* a, const char* shortopt, const char* longopt) {
    return (shortopt && strcmp(a->name, shortopt) == 0) || strcmp(a->name, longopt) == 0;
}

/* The option's value, or NULL after reporting that it is missing. */
static const char* value_of(arg_cursor* a) {
    if (a->inline_value)
        return a->inline_value;
    if (a->i + 1 < a->argc)
        return a->argv[++a->i];
    usage_error("option '%s' needs a value", a->name);
    return NULL;
}

/* Returns -1 to continue, or an exit status. */
static int parse_args(int argc, char** argv, config* c) {
    static const char* const scale_names[] = {"utc", "tt", "ut1"};
    static const int scale_values[] = {SCALE_UTC, SCALE_TT, SCALE_UT1};
    static const char* const center_names[] = {"geo",   "geocentric",   "topo", "topocentric",
                                               "helio", "heliocentric", "bary", "barycentric"};
    static const int center_values[] = {
        PROMETHEIA_CENTER_GEOCENTRIC,   PROMETHEIA_CENTER_GEOCENTRIC,
        PROMETHEIA_CENTER_TOPOCENTRIC,  PROMETHEIA_CENTER_TOPOCENTRIC,
        PROMETHEIA_CENTER_HELIOCENTRIC, PROMETHEIA_CENTER_HELIOCENTRIC,
        PROMETHEIA_CENTER_BARYCENTRIC,  PROMETHEIA_CENTER_BARYCENTRIC};
    static const char* const frame_names[] = {"true", "mean", "j2000", "icrf"};
    static const int frame_values[] = {PROMETHEIA_FRAME_TRUE_OF_DATE, PROMETHEIA_FRAME_MEAN_OF_DATE,
                                       PROMETHEIA_FRAME_J2000, PROMETHEIA_FRAME_ICRF};
    static const char* const format_names[] = {"table", "csv", "json"};
    static const int format_values[] = {FORMAT_TABLE, FORMAT_CSV, FORMAT_JSON};
    static const char* const flags_without_value[] = {"--help",
                                                      "--version",
                                                      "--equatorial",
                                                      "--ecliptic",
                                                      "--apparent",
                                                      "--astrometric",
                                                      "--geometric",
                                                      "--no-light-time",
                                                      "--no-deflection",
                                                      "--no-aberration",
                                                      "--no-speed",
                                                      "--no-sigma",
                                                      "--dms"};
    int site_given = 0, center_given = 0, only_bodies = 0;
    size_t k;
    arg_cursor a;
    a.argc = argc;
    a.argv = argv;

    for (a.i = 1; a.i < argc; ++a.i) {
        const char* arg = argv[a.i];
        const char* v;
        const char* eq;

        if (only_bodies || arg[0] != '-' || arg[1] == '\0' || all_digits(arg)) {
            if (c->n_bodies == MAX_BODIES)
                return usage_error("too many bodies (limit %s)", "256");
            c->bodies[c->n_bodies++] = arg;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            only_bodies = 1;
            continue;
        }
        eq = arg[1] == '-' ? strchr(arg, '=') : NULL;
        a.inline_value = eq ? eq + 1 : NULL;
        k = eq ? (size_t)(eq - arg) : strlen(arg);
        if (k >= sizeof a.name)
            return usage_error("unknown option '%s'", arg);
        memcpy(a.name, arg, k);
        a.name[k] = '\0';
        if (a.inline_value) {
            for (k = 0; k < sizeof flags_without_value / sizeof flags_without_value[0]; ++k)
                if (strcmp(a.name, flags_without_value[k]) == 0)
                    return usage_error("option '%s' takes no value", a.name);
        }

        if (is_opt(&a, "-h", "--help")) {
            print_help();
            return 0;
        } else if (is_opt(&a, "-V", "--version")) {
            printf("ephem (Ephemeris Prometheia) %s, C ABI %d\n", prometheia_version(),
                   prometheia_abi_version());
            return 0;
        } else if (is_opt(&a, "-e", "--ephemeris")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            c->ephemeris = v;
        } else if (is_opt(&a, "-c", "--catalog")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (c->n_catalogs == MAX_CATALOGS)
                return usage_error("too many catalogs (limit %s)", "32");
            c->catalogs[c->n_catalogs++] = v;
        } else if (is_opt(&a, "-t", "--time")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            c->have_jd = 0;
            if (equals_nocase(v, "now"))
                c->have_time = 0;
            else if (!parse_calendar(v, c))
                return usage_error("cannot parse time '%s' (YYYY-MM-DD[THH:MM[:SS]])", v);
        } else if (is_opt(&a, "-j", "--jd")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_double(v, &c->jd))
                return usage_error("cannot parse Julian date '%s'", v);
            c->have_jd = 1;
            c->have_time = 0;
        } else if (is_opt(&a, NULL, "--scale")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_keyword(v, scale_names, scale_values, 3, &c->scale))
                return usage_error("unknown time scale '%s' (utc, tt, ut1)", v);
        } else if (is_opt(&a, "-n", "--count")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_long(v, &c->count) || c->count < 1 || c->count > MAX_ROWS)
                return usage_error("--count must be 1..1000000, got '%s'", v);
        } else if (is_opt(&a, "-s", "--step")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_step(v, &c->step_days))
                return usage_error("cannot parse step '%s'", v);
        } else if (is_opt(&a, NULL, "--delta-t")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_double(v, &c->delta_t))
                return usage_error("cannot parse Delta T '%s'", v);
            c->have_delta_t = 1;
        } else if (is_opt(&a, NULL, "--center")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_keyword(v, center_names, center_values, 8, &c->opts.center))
                return usage_error("unknown center '%s' (geo, topo, helio, bary)", v);
            center_given = 1;
        } else if (is_opt(&a, NULL, "--site")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_site(v, &c->opts))
                return usage_error("cannot parse site '%s' (LON,LAT[,HEIGHT])", v);
            site_given = 1;
        } else if (is_opt(&a, NULL, "--frame")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_keyword(v, frame_names, frame_values, 4, &c->opts.frame))
                return usage_error("unknown frame '%s' (true, mean, j2000, icrf)", v);
        } else if (is_opt(&a, NULL, "--sidereal")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_sidereal(v, &c->opts))
                return usage_error("unknown sidereal mode '%s'", v);
        } else if (is_opt(&a, "-f", "--format")) {
            if (!(v = value_of(&a)))
                return EXIT_USAGE;
            if (!parse_keyword(v, format_names, format_values, 3, &c->format))
                return usage_error("unknown format '%s' (table, csv, json)", v);
        } else if (is_opt(&a, NULL, "--equatorial")) {
            c->opts.coords = PROMETHEIA_COORDS_EQUATORIAL;
        } else if (is_opt(&a, NULL, "--ecliptic")) {
            c->opts.coords = PROMETHEIA_COORDS_ECLIPTIC;
        } else if (is_opt(&a, NULL, "--apparent")) {
            c->opts.light_time = c->opts.deflection = c->opts.aberration = 1;
        } else if (is_opt(&a, NULL, "--astrometric")) {
            c->opts.light_time = 1;
            c->opts.deflection = c->opts.aberration = 0;
        } else if (is_opt(&a, NULL, "--geometric")) {
            c->opts.light_time = c->opts.deflection = c->opts.aberration = 0;
        } else if (is_opt(&a, NULL, "--no-light-time")) {
            c->opts.light_time = 0;
        } else if (is_opt(&a, NULL, "--no-deflection")) {
            c->opts.deflection = 0;
        } else if (is_opt(&a, NULL, "--no-aberration")) {
            c->opts.aberration = 0;
        } else if (is_opt(&a, NULL, "--no-speed")) {
            c->opts.speed = 0;
        } else if (is_opt(&a, NULL, "--no-sigma")) {
            c->opts.sigma = 0;
        } else if (is_opt(&a, NULL, "--dms")) {
            c->dms = 1;
        } else {
            return usage_error("unknown option '%s'", a.name);
        }
    }

    if (site_given && !center_given)
        c->opts.center = PROMETHEIA_CENTER_TOPOCENTRIC;
    if (c->opts.center == PROMETHEIA_CENTER_TOPOCENTRIC && !site_given)
        return usage_error("%s", "--center topo needs --site LON,LAT[,H]");
    if (c->have_jd && c->scale == SCALE_UTC)
        return usage_error("%s", "--jd is TT or UT1; give UTC instants with --time");
    if (c->scale == SCALE_DEFAULT)
        c->scale = c->have_jd ? SCALE_TT : SCALE_UTC;
    return -1;
}

/* ---- Time --------------------------------------------------------------- */

typedef struct time_model {
    int fixed;
    double seconds;
} time_model;

static double fixed_delta_t(void* user, double jd_tt) {
    (void)jd_tt;
    return ((const time_model*)user)->seconds;
}

static double delta_t_of(const time_model* m, double jd_tt) {
    return m->fixed ? m->seconds : prometheia_delta_t(jd_tt);
}

/* "YYYY-MM-DD HH:MM:SS.sss" (UTC, to the millisecond), or "" before 1972. */
static void format_utc(double jd_tt, char* out, size_t n) {
    prometheia_utc u;
    /* Truncating after adding half a millisecond rounds and still rolls the
     * minute, the day and a leap second over correctly. */
    if (prometheia_tt_to_utc(jd_tt + 0.0005 / 86400.0, &u, NULL) != PROMETHEIA_OK) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%06.3f", u.year, u.month, u.day, u.hour, u.minute,
             floor(u.second * 1000.0) / 1000.0);
}

/* ---- Output ------------------------------------------------------------- */

static void format_sexagesimal(double value, int hours, int is_signed, char* out, size_t n) {
    const double scaled = hours ? value / 15.0 : value;
    const char sign = scaled < 0.0 ? '-' : '+';
    /* Work in units of 0.0001 s (hours) or 0.001" (degrees). */
    const double per_second = hours ? 10000.0 : 1000.0;
    double units = floor(fabs(scaled) * 3600.0 * per_second + 0.5);
    long whole = (long)floor(units / (3600.0 * per_second));
    double rest = units - whole * 3600.0 * per_second;
    int minutes = (int)floor(rest / (60.0 * per_second));
    double seconds = (rest - minutes * 60.0 * per_second) / per_second;
    if (!hours && !is_signed && whole >= 360)
        whole -= 360;
    if (hours && whole >= 24)
        whole -= 24;
    if (hours)
        snprintf(out, n, "%02ldh%02dm%07.4fs", whole, minutes, seconds);
    else if (is_signed)
        snprintf(out, n, "%c%02ld\xC2\xB0%02d'%06.3f\"", sign, whole, minutes, seconds);
    else
        snprintf(out, n, "%03ld\xC2\xB0%02d'%06.3f\"", whole, minutes, seconds);
}

static void json_string(const char* s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 0x20)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}

static void csv_field(const char* s) {
    if (strpbrk(s, ",\"\n")) {
        putchar('"');
        for (; *s; ++s) {
            if (*s == '"')
                putchar('"');
            putchar(*s);
        }
        putchar('"');
    } else {
        fputs(s, stdout);
    }
}

static const char* center_text(int center) {
    switch (center) {
    case PROMETHEIA_CENTER_TOPOCENTRIC:
        return "topocentric";
    case PROMETHEIA_CENTER_HELIOCENTRIC:
        return "heliocentric";
    case PROMETHEIA_CENTER_BARYCENTRIC:
        return "barycentric";
    default:
        return "geocentric";
    }
}

static const char* frame_text(const prometheia_options* o) {
    const int eq = o->coords == PROMETHEIA_COORDS_EQUATORIAL;
    switch (o->frame) {
    case PROMETHEIA_FRAME_ICRF:
        return eq ? "ICRF equator" : "ICRF axes, J2000 mean ecliptic";
    case PROMETHEIA_FRAME_J2000:
        return eq ? "mean equator and equinox of J2000" : "mean ecliptic and equinox of J2000";
    case PROMETHEIA_FRAME_MEAN_OF_DATE:
        return eq ? "mean equator and equinox of date" : "mean ecliptic and equinox of date";
    default:
        return eq ? "true equator and equinox of date" : "ecliptic and true equinox of date";
    }
}

static const char* corrections_text(const prometheia_options* o) {
    if (o->light_time && o->deflection && o->aberration)
        return "apparent";
    if (o->light_time && !o->deflection && !o->aberration)
        return "astrometric";
    if (!o->light_time && !o->deflection && !o->aberration)
        return "geometric";
    if (o->light_time)
        return o->deflection ? "light time + deflection" : "light time + aberration";
    return o->deflection ? (o->aberration ? "deflection + aberration" : "deflection only")
                         : "aberration only";
}

static const char* sidereal_text(int mode) {
    switch (mode) {
    case PROMETHEIA_SIDEREAL_FAGAN_BRADLEY:
        return "sidereal (Fagan/Bradley)";
    case PROMETHEIA_SIDEREAL_LAHIRI:
        return "sidereal (Lahiri)";
    case PROMETHEIA_SIDEREAL_USER:
        return "sidereal (user anchor)";
    default:
        return "tropical";
    }
}

typedef struct printer {
    const config* c;
    long rows;   /* rows printed so far */
    int errors;  /* failures so far */
    int columns; /* table: time columns present */
} printer;

static void table_header(printer* p, const char* source, double jd_tt, const time_model* dt) {
    const config* c = p->c;
    const int eq = c->opts.coords == PROMETHEIA_COORDS_EQUATORIAL;
    char utc[40];
    printf("# %s\n", source);
    printf("# %s, %s, %s, %s", center_text(c->opts.center), corrections_text(&c->opts),
           frame_text(&c->opts), sidereal_text(c->opts.sidereal));
    if (c->opts.center == PROMETHEIA_CENTER_TOPOCENTRIC)
        printf(", site %.6f,%.6f,%gm", c->opts.site_lon_deg, c->opts.site_lat_deg,
               c->opts.site_height_m);
    printf("\n");
    format_utc(jd_tt, utc, sizeof utc);
    if (c->count == 1) {
        printf("# %s%sJD %.6f TT, Delta T %.3f s%s\n", utc, utc[0] ? " UTC, " : "", jd_tt,
               delta_t_of(dt, jd_tt), dt->fixed ? " (fixed)" : "");
    } else {
        printf("# %ld rows from JD %.6f TT, step %.10g d, Delta T %s\n", c->count, jd_tt,
               c->step_days, dt->fixed ? "fixed" : "observed USNO model");
    }
    p->columns = c->count > 1;
    if (p->columns)
        printf("%-23s  %-16s  ", "UTC", "JD (TT)");
    printf("%-12s  %*s  %*s  %14s", "body", c->dms ? 16 : 13, eq ? "RA" : "longitude",
           c->dms ? 15 : 12, eq ? "dec" : "latitude", "distance AU");
    if (c->opts.speed)
        printf("  %12s  %12s  %13s", eq ? "RA/day" : "lon/day", eq ? "dec/day" : "lat/day",
               "dist/day");
    printf("  sigma\n");
}

static void csv_header(const config* c) {
    const int eq = c->opts.coords == PROMETHEIA_COORDS_EQUATORIAL;
    printf("utc,jd_tt,body,id,%s,%s,dist_au,%s,%s,dist_speed,x_au,y_au,z_au,"
           "vx_au_day,vy_au_day,vz_au_day,light_time_days,sigma_arcsec,ayanamsa_deg,source\n",
           eq ? "ra_deg" : "lon_deg", eq ? "dec_deg" : "lat_deg", eq ? "ra_speed" : "lon_speed",
           eq ? "dec_speed" : "lat_speed");
}

static void print_row(printer* p, double jd_tt, const body_ref* b, const prometheia_result* r) {
    const config* c = p->c;
    const int eq = c->opts.coords == PROMETHEIA_COORDS_EQUATORIAL;
    char utc[40];
    format_utc(jd_tt, utc, sizeof utc);

    if (c->format == FORMAT_TABLE) {
        char lon[32], lat[32];
        if (p->columns)
            printf("%-23s  %16.6f  ", utc[0] ? utc : "-", jd_tt);
        if (c->dms) {
            format_sexagesimal(r->lon_deg, eq, 0, lon, sizeof lon);
            format_sexagesimal(r->lat_deg, 0, 1, lat, sizeof lat);
            printf("%-12s  %16s  %16s  %14.10f", b->label, lon, lat, r->dist_au);
        } else {
            printf("%-12s  %13.8f  %12.8f  %14.10f", b->label, r->lon_deg, r->lat_deg, r->dist_au);
        }
        if (c->opts.speed)
            printf("  %12.8f  %12.8f  %13.10f", r->lon_speed, r->lat_speed, r->dist_speed);
        if (r->flags & PROMETHEIA_HAS_SIGMA)
            printf("  %.4f\"", r->sigma_arcsec);
        printf("\n");
    } else if (c->format == FORMAT_CSV) {
        printf("%s,%.17g,", utc, jd_tt);
        csv_field(b->label);
        printf(",%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,",
               b->id, r->lon_deg, r->lat_deg, r->dist_au, r->lon_speed, r->lat_speed, r->dist_speed,
               r->xyz_au[0], r->xyz_au[1], r->xyz_au[2], r->vel_au_day[0], r->vel_au_day[1],
               r->vel_au_day[2], r->light_time_days);
        if (r->flags & PROMETHEIA_HAS_SIGMA)
            printf("%.17g", r->sigma_arcsec);
        putchar(',');
        if (r->flags & PROMETHEIA_HAS_AYANAMSA)
            printf("%.17g", r->ayanamsa_deg);
        putchar(',');
        csv_field(r->source);
        putchar('\n');
    } else {
        printf("%s\n    {\"utc\": ", p->rows ? "," : "");
        if (utc[0])
            json_string(utc);
        else
            printf("null");
        printf(", \"jd_tt\": %.17g, \"body\": ", jd_tt);
        json_string(b->label);
        printf(", \"id\": %d, \"%s\": %.17g, \"%s\": %.17g, \"dist_au\": %.17g", b->id,
               eq ? "ra_deg" : "lon_deg", r->lon_deg, eq ? "dec_deg" : "lat_deg", r->lat_deg,
               r->dist_au);
        printf(", \"%s\": %.17g, \"%s\": %.17g, \"dist_speed\": %.17g",
               eq ? "ra_speed" : "lon_speed", r->lon_speed, eq ? "dec_speed" : "lat_speed",
               r->lat_speed, r->dist_speed);
        printf(", \"xyz_au\": [%.17g, %.17g, %.17g], \"vel_au_day\": [%.17g, %.17g, %.17g]",
               r->xyz_au[0], r->xyz_au[1], r->xyz_au[2], r->vel_au_day[0], r->vel_au_day[1],
               r->vel_au_day[2]);
        printf(", \"light_time_days\": %.17g, \"sigma_arcsec\": ", r->light_time_days);
        if (r->flags & PROMETHEIA_HAS_SIGMA)
            printf("%.17g", r->sigma_arcsec);
        else
            printf("null");
        printf(", \"ayanamsa_deg\": ");
        if (r->flags & PROMETHEIA_HAS_AYANAMSA)
            printf("%.17g", r->ayanamsa_deg);
        else
            printf("null");
        printf(", \"source\": ");
        json_string(r->source);
        putchar('}');
    }
    ++p->rows;
}

/* ---- Main --------------------------------------------------------------- */

static int add_env_catalogs(prometheia_engine* eng, prometheia_error* err) {
    const char* env = getenv("PROMETHEIA_CATALOGS");
    char *copy, *tok, *save;
    size_t n;
    if (!env || !*env)
        return 1;
    n = strlen(env);
    copy = (char*)malloc(n + 1);
    if (!copy)
        return 0;
    memcpy(copy, env, n + 1);
    for (tok = copy; tok; tok = save) {
        save = strchr(tok, ':');
        if (save)
            *save++ = '\0';
        if (!*tok)
            continue;
        if (prometheia_engine_add_catalog(eng, tok, err) != PROMETHEIA_OK) {
            fprintf(stderr, "%s: catalog %s: %s\n", g_program, tok, err->message);
            free(copy);
            return 0;
        }
    }
    free(copy);
    return 1;
}

int main(int argc, char** argv) {
    config c;
    time_model dt;
    prometheia_error err;
    prometheia_engine* eng = NULL;
    body_ref bodies[MAX_BODIES];
    int n_bodies = 0, i, status;
    long row;
    double base, first_tt;
    printer p;

    memset(&c, 0, sizeof c);
    prometheia_options_init(&c.opts);
    c.count = 1;
    c.step_days = 1.0;
    c.format = FORMAT_TABLE;

    status = parse_args(argc, argv, &c);
    if (status >= 0)
        return status;

    if (!c.ephemeris)
        c.ephemeris = getenv("PROMETHEIA_EPHEMERIS");
    if (!c.ephemeris || !*c.ephemeris) {
        fprintf(stderr, "%s: no ephemeris: pass -e FILE or set PROMETHEIA_EPHEMERIS\n", g_program);
        return EXIT_USAGE;
    }
    if (prometheia_engine_open(c.ephemeris, &eng, &err) != PROMETHEIA_OK) {
        fprintf(stderr, "%s: %s\n", g_program, err.message);
        return EXIT_USAGE;
    }
    if (!add_env_catalogs(eng, &err)) {
        prometheia_engine_close(eng);
        return EXIT_USAGE;
    }
    for (i = 0; i < c.n_catalogs; ++i) {
        if (prometheia_engine_add_catalog(eng, c.catalogs[i], &err) != PROMETHEIA_OK) {
            fprintf(stderr, "%s: catalog %s: %s\n", g_program, c.catalogs[i], err.message);
            prometheia_engine_close(eng);
            return EXIT_USAGE;
        }
    }
    dt.fixed = c.have_delta_t;
    dt.seconds = c.delta_t;
    if (dt.fixed)
        prometheia_engine_set_delta_t(eng, fixed_delta_t, &dt);

    /* Bodies: NAIF integers, built-in names, then catalog names. */
    memset(&p, 0, sizeof p);
    p.c = &c;
    if (c.n_bodies == 0) {
        for (i = 0; i < (int)(sizeof kDefaultBodies / sizeof kDefaultBodies[0]); ++i)
            c.bodies[c.n_bodies++] = kDefaultBodies[i];
    }
    for (i = 0; i < c.n_bodies; ++i) {
        const char* s = c.bodies[i];
        body_ref b;
        size_t k;
        b.label = s;
        if (all_digits(s)) {
            long id;
            if (!parse_long(s, &id) || id < -2147483647L || id > 2147483647L) {
                fprintf(stderr, "%s: body ID out of range: %s\n", g_program, s);
                p.errors++;
                continue;
            }
            b.id = (int)id;
            for (k = 0; k < sizeof kNamedBodies / sizeof kNamedBodies[0]; ++k)
                if (kNamedBodies[k].id == b.id)
                    b.label = kNamedBodies[k].label;
            bodies[n_bodies++] = b;
            continue;
        }
        if (s[0] != '@') {
            int found = 0;
            for (k = 0; k < sizeof kNamedBodies / sizeof kNamedBodies[0]; ++k) {
                if (equals_nocase(s, kNamedBodies[k].name)) {
                    b.id = kNamedBodies[k].id;
                    b.label = kNamedBodies[k].label;
                    found = 1;
                }
            }
            if (found) {
                bodies[n_bodies++] = b;
                continue;
            }
        } else {
            b.label = ++s;
        }
        if (prometheia_engine_lookup(eng, s, &b.id, &err) != PROMETHEIA_OK) {
            fprintf(stderr, "%s: unknown body '%s': %s\n", g_program, s, err.message);
            p.errors++;
            continue;
        }
        bodies[n_bodies++] = b;
    }

    /* The base epoch, in TT (or UT1 with --scale ut1). */
    if (c.have_jd) {
        base = c.jd;
    } else {
        if (!c.have_time) {
            const time_t now = time(NULL);
            const struct tm* g = gmtime(&now);
            c.year = g->tm_year + 1900;
            c.month = g->tm_mon + 1;
            c.day = g->tm_mday;
            c.hour = g->tm_hour;
            c.minute = g->tm_min;
            c.second = g->tm_sec > 59 ? 59.0 : (double)g->tm_sec;
            c.scale = SCALE_UTC;
        }
        if (c.scale == SCALE_UTC) {
            if (prometheia_utc_to_tt(c.year, c.month, c.day, c.hour, c.minute, c.second, &base,
                                     &err) != PROMETHEIA_OK) {
                fprintf(stderr, "%s: %s (use --scale tt or ut1 before 1972)\n", g_program,
                        err.message);
                prometheia_engine_close(eng);
                return EXIT_USAGE;
            }
        } else {
            base = prometheia_jd_from_ymdhms(c.year, c.month, c.day, c.hour, c.minute, c.second);
        }
    }

    first_tt = base;
    if (c.scale == SCALE_UT1) {
        first_tt = base + delta_t_of(&dt, base) / 86400.0;
        first_tt = base + delta_t_of(&dt, first_tt) / 86400.0;
    }

    if (c.format == FORMAT_TABLE)
        table_header(&p, prometheia_engine_source(eng), first_tt, &dt);
    else if (c.format == FORMAT_CSV)
        csv_header(&c);
    else {
        printf("{\"ephemeris\": ");
        json_string(prometheia_engine_source(eng));
        printf(", \"center\": \"%s\", \"frame\": ", center_text(c.opts.center));
        json_string(frame_text(&c.opts));
        printf(", \"corrections\": \"%s\", \"zodiac\": ", corrections_text(&c.opts));
        json_string(sidereal_text(c.opts.sidereal));
        printf(", \"rows\": [");
    }

    for (row = 0; row < c.count; ++row) {
        const double t = base + (double)row * c.step_days;
        double jd_tt = t;
        if (c.scale == SCALE_UT1) {
            jd_tt = t + delta_t_of(&dt, t) / 86400.0;
            jd_tt = t + delta_t_of(&dt, jd_tt) / 86400.0;
        }
        for (i = 0; i < n_bodies; ++i) {
            prometheia_result r;
            const prometheia_status s =
                c.scale == SCALE_UT1 ? prometheia_calc_ut(eng, bodies[i].id, t, &c.opts, &r, &err)
                                     : prometheia_calc(eng, bodies[i].id, t, &c.opts, &r, &err);
            if (s != PROMETHEIA_OK) {
                fprintf(stderr, "%s: %s at JD %.6f TT: %s\n", g_program, bodies[i].label, jd_tt,
                        err.message);
                p.errors++;
                continue;
            }
            print_row(&p, jd_tt, &bodies[i], &r);
        }
    }

    if (c.format == FORMAT_JSON)
        printf("%s]}\n", p.rows ? "\n  " : "");
    prometheia_engine_close(eng);
    if (fflush(stdout) != 0) {
        fprintf(stderr, "%s: write error\n", g_program);
        return EXIT_USAGE;
    }
    return p.errors ? EXIT_PARTIAL : 0;
}
