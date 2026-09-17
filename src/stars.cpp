// SPDX-License-Identifier: GPL-2.0-or-later
//
// The compiled-in fixed-star catalog: records, names and lookup, and the
// constellation boundaries. docs/STARS.md.
#include "prometheia/stars.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_map>

#include "prometheia/frames.hpp"

namespace prometheia::stars {
namespace {

struct StarRecord {
    int hr, hd, hip, flamsteed, messier, bayer, bayer_index;
    const char* constellation;
    int kind, source;
    double vmag, ra, dec, epoch, pmra, pmdec, parallax, rv, size;
    const char* spectral;
    const char* names; // '|'-separated
};

struct ConstellationBoundary {
    double ra_from_h, ra_to_h, dec_from_deg;
    const char* abbreviation;
};

#include "star_catalog.inc"

constexpr size_t kRecords = sizeof(kStarRecords) / sizeof(kStarRecords[0]);

const std::vector<Constellation> kConstellations = {
    {"And", "Andromeda", "Andromedae"},
    {"Ant", "Antlia", "Antliae"},
    {"Aps", "Apus", "Apodis"},
    {"Aqr", "Aquarius", "Aquarii"},
    {"Aql", "Aquila", "Aquilae"},
    {"Ara", "Ara", "Arae"},
    {"Ari", "Aries", "Arietis"},
    {"Aur", "Auriga", "Aurigae"},
    {"Boo", "Bootes", "Bootis"},
    {"Cae", "Caelum", "Caeli"},
    {"Cam", "Camelopardalis", "Camelopardalis"},
    {"Cnc", "Cancer", "Cancri"},
    {"CVn", "Canes Venatici", "Canum Venaticorum"},
    {"CMa", "Canis Major", "Canis Majoris"},
    {"CMi", "Canis Minor", "Canis Minoris"},
    {"Cap", "Capricornus", "Capricorni"},
    {"Car", "Carina", "Carinae"},
    {"Cas", "Cassiopeia", "Cassiopeiae"},
    {"Cen", "Centaurus", "Centauri"},
    {"Cep", "Cepheus", "Cephei"},
    {"Cet", "Cetus", "Ceti"},
    {"Cha", "Chamaeleon", "Chamaeleontis"},
    {"Cir", "Circinus", "Circini"},
    {"Col", "Columba", "Columbae"},
    {"Com", "Coma Berenices", "Comae Berenices"},
    {"CrA", "Corona Australis", "Coronae Australis"},
    {"CrB", "Corona Borealis", "Coronae Borealis"},
    {"Crv", "Corvus", "Corvi"},
    {"Crt", "Crater", "Crateris"},
    {"Cru", "Crux", "Crucis"},
    {"Cyg", "Cygnus", "Cygni"},
    {"Del", "Delphinus", "Delphini"},
    {"Dor", "Dorado", "Doradus"},
    {"Dra", "Draco", "Draconis"},
    {"Equ", "Equuleus", "Equulei"},
    {"Eri", "Eridanus", "Eridani"},
    {"For", "Fornax", "Fornacis"},
    {"Gem", "Gemini", "Geminorum"},
    {"Gru", "Grus", "Gruis"},
    {"Her", "Hercules", "Herculis"},
    {"Hor", "Horologium", "Horologii"},
    {"Hya", "Hydra", "Hydrae"},
    {"Hyi", "Hydrus", "Hydri"},
    {"Ind", "Indus", "Indi"},
    {"Lac", "Lacerta", "Lacertae"},
    {"Leo", "Leo", "Leonis"},
    {"LMi", "Leo Minor", "Leonis Minoris"},
    {"Lep", "Lepus", "Leporis"},
    {"Lib", "Libra", "Librae"},
    {"Lup", "Lupus", "Lupi"},
    {"Lyn", "Lynx", "Lyncis"},
    {"Lyr", "Lyra", "Lyrae"},
    {"Men", "Mensa", "Mensae"},
    {"Mic", "Microscopium", "Microscopii"},
    {"Mon", "Monoceros", "Monocerotis"},
    {"Mus", "Musca", "Muscae"},
    {"Nor", "Norma", "Normae"},
    {"Oct", "Octans", "Octantis"},
    {"Oph", "Ophiuchus", "Ophiuchi"},
    {"Ori", "Orion", "Orionis"},
    {"Pav", "Pavo", "Pavonis"},
    {"Peg", "Pegasus", "Pegasi"},
    {"Per", "Perseus", "Persei"},
    {"Phe", "Phoenix", "Phoenicis"},
    {"Pic", "Pictor", "Pictoris"},
    {"Psc", "Pisces", "Piscium"},
    {"PsA", "Piscis Austrinus", "Piscis Austrini"},
    {"Pup", "Puppis", "Puppis"},
    {"Pyx", "Pyxis", "Pyxidis"},
    {"Ret", "Reticulum", "Reticuli"},
    {"Sge", "Sagitta", "Sagittae"},
    {"Sgr", "Sagittarius", "Sagittarii"},
    {"Sco", "Scorpius", "Scorpii"},
    {"Scl", "Sculptor", "Sculptoris"},
    {"Sct", "Scutum", "Scuti"},
    {"Ser", "Serpens", "Serpentis"},
    {"Sex", "Sextans", "Sextantis"},
    {"Tau", "Taurus", "Tauri"},
    {"Tel", "Telescopium", "Telescopii"},
    {"Tri", "Triangulum", "Trianguli"},
    {"TrA", "Triangulum Australe", "Trianguli Australis"},
    {"Tuc", "Tucana", "Tucanae"},
    {"UMa", "Ursa Major", "Ursae Majoris"},
    {"UMi", "Ursa Minor", "Ursae Minoris"},
    {"Vel", "Vela", "Velorum"},
    {"Vir", "Virgo", "Virginis"},
    {"Vol", "Volans", "Volantis"},
    {"Vul", "Vulpecula", "Vulpeculae"},
};

struct Greek {
    const char* name;
    const char* abbreviation;
    const char* symbol;
    const char* alternate; // SIMBAD's spelling where it differs
};
constexpr Greek kGreek[] = {
    {"alpha", "alp", "α", "alf"}, {"beta", "bet", "β", ""},     {"gamma", "gam", "γ", ""},
    {"delta", "del", "δ", ""},    {"epsilon", "eps", "ε", ""},  {"zeta", "zet", "ζ", ""},
    {"eta", "eta", "η", ""},      {"theta", "the", "θ", "tet"}, {"iota", "iot", "ι", ""},
    {"kappa", "kap", "κ", ""},    {"lambda", "lam", "λ", ""},   {"mu", "mu", "μ", ""},
    {"nu", "nu", "ν", ""},        {"xi", "xi", "ξ", "ksi"},     {"omicron", "omi", "ο", ""},
    {"pi", "pi", "π", ""},        {"rho", "rho", "ρ", ""},      {"sigma", "sig", "σ", ""},
    {"tau", "tau", "τ", ""},      {"upsilon", "ups", "υ", ""},  {"phi", "phi", "φ", ""},
    {"chi", "chi", "χ", ""},      {"psi", "psi", "ψ", ""},      {"omega", "ome", "ω", ""},
};

constexpr const char* kSuperscripts[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};

const Constellation* constellation_by_abbreviation(std::string_view abbr) {
    for (const Constellation& c : kConstellations) {
        if (c.abbreviation.size() == abbr.size() &&
            std::equal(abbr.begin(), abbr.end(), c.abbreviation.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return &c;
        }
    }
    return nullptr;
}

// Decodes one UTF-8 code point at s[i], advancing i; 0xFFFD on bad input.
char32_t next_code_point(std::string_view s, size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const int n = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + size_t(n) >= s.size() + (n == 0 ? 1 : 0)) {
        ++i;
        return 0xFFFD;
    }
    char32_t cp = n == 0 ? b : (b & (0x3F >> n));
    for (int k = 1; k <= n; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
    }
    i += size_t(n) + 1;
    return cp;
}

// The matching key of a name or query: lowercase ASCII letters and digits,
// accents folded, Greek letters spelled out, superscript digits as digits,
// everything else dropped.
std::string key_of(std::string_view s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = next_code_point(s, i);
        if (cp < 0x80) {
            const char c = static_cast<char>(cp);
            if (std::isalnum(static_cast<unsigned char>(c))) {
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            continue;
        }
        if (cp >= 0x3B1 && cp <= 0x3C9) { // Greek small letters (final sigma folds to sigma)
            const int k = cp == 0x3C2 ? 18 : int(cp - 0x3B1) + (cp > 0x3C2 ? 0 : 1);
            if (k >= 1 && k <= 24) {
                out += kGreek[k - 1].name;
            }
            continue;
        }
        if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
            const int k = int(cp - 0x391) + (cp > 0x3A2 ? 0 : 1);
            if (k >= 1 && k <= 24) {
                out += kGreek[k - 1].name;
            }
            continue;
        }
        switch (cp) {
        case 0xB9:
            out += '1';
            continue;
        case 0xB2:
            out += '2';
            continue;
        case 0xB3:
            out += '3';
            continue;
        default:
            break;
        }
        if (cp >= 0x2070 && cp <= 0x2079) {
            out += char('0' + (cp - 0x2070));
            continue;
        }
        // Latin-1 and Latin Extended-A letters with diacritics.
        static constexpr std::pair<char32_t, char> kFold[] = {
            {0xE0, 'a'},  {0xE1, 'a'},  {0xE2, 'a'},   {0xE3, 'a'},  {0xE4, 'a'},  {0xE5, 'a'},
            {0xE7, 'c'},  {0xE8, 'e'},  {0xE9, 'e'},   {0xEA, 'e'},  {0xEB, 'e'},  {0xEC, 'i'},
            {0xED, 'i'},  {0xEE, 'i'},  {0xEF, 'i'},   {0xF1, 'n'},  {0xF2, 'o'},  {0xF3, 'o'},
            {0xF4, 'o'},  {0xF5, 'o'},  {0xF6, 'o'},   {0xF8, 'o'},  {0xF9, 'u'},  {0xFA, 'u'},
            {0xFB, 'u'},  {0xFC, 'u'},  {0xFD, 'y'},   {0xFF, 'y'},  {0x101, 'a'}, {0x103, 'a'},
            {0x113, 'e'}, {0x11F, 'g'}, {0x12B, 'i'},  {0x14D, 'o'}, {0x15F, 's'}, {0x161, 's'},
            {0x16B, 'u'}, {0x17E, 'z'}, {0x2BB, '\0'}, {0x2BC, '\0'}};
        char32_t lower = cp;
        if (cp >= 0xC0 && cp <= 0xDE) {
            lower = cp + 0x20;
        } else if (cp >= 0x100 && cp <= 0x17F && (cp % 2) == 0) {
            lower = cp + 1;
        }
        for (const auto& [from, to] : kFold) {
            if (from == lower) {
                if (to) {
                    out += to;
                }
                break;
            }
        }
    }
    return out;
}

// The objects, built once from the records.
struct Catalog {
    std::vector<Object> objects;
    // key -> (object, quality of a match on this key, the form that matched)
    struct Entry {
        size_t index;
        MatchQuality quality;
        std::string form;
    };
    std::unordered_multimap<std::string, Entry> index;
    std::vector<std::pair<std::string, size_t>> name_keys; // for prefix lookups

    Catalog() {
        objects.reserve(kRecords);
        for (size_t i = 0; i < kRecords; ++i) {
            const StarRecord& r = kStarRecords[i];
            Object o;
            o.index = i;
            o.kind = static_cast<Kind>(r.kind);
            o.astrometry = static_cast<Astrometry>(r.source);
            o.hr = r.hr;
            o.hd = r.hd;
            o.hip = r.hip;
            o.flamsteed = r.flamsteed;
            o.messier = r.messier;
            o.bayer = r.bayer;
            o.bayer_index = r.bayer_index;
            o.vmag = r.vmag;
            o.ra_deg = r.ra;
            o.dec_deg = r.dec;
            o.epoch_jyear = r.epoch;
            o.pm_ra_mas_yr = r.pmra;
            o.pm_dec_mas_yr = r.pmdec;
            o.parallax_mas = r.parallax;
            o.rv_km_s = r.rv;
            o.size_arcmin = r.size;
            o.spectral_type = r.spectral;
            const Constellation* c = constellation_by_abbreviation(r.constellation);
            o.constellation = c ? c->abbreviation : constellation_at(r.ra, r.dec);
            std::string_view names = r.names;
            while (!names.empty()) {
                const size_t bar = names.find('|');
                o.names.push_back(names.substr(0, bar));
                names = bar == std::string_view::npos ? std::string_view{} : names.substr(bar + 1);
            }
            objects.push_back(std::move(o));
        }
        for (const Object& o : objects) {
            add_keys(o);
        }
    }

    void add(const std::string& key, size_t i, MatchQuality q, std::string form) {
        if (key.empty()) {
            return;
        }
        auto [lo, hi] = index.equal_range(key);
        for (auto it = lo; it != hi; ++it) {
            if (it->second.index == i) {
                if (q < it->second.quality) {
                    it->second.quality = q;
                    it->second.form = std::move(form);
                }
                return;
            }
        }
        index.emplace(key, Entry{i, q, std::move(form)});
    }

    void add_keys(const Object& o) {
        const size_t i = o.index;
        for (std::string_view n : o.names) {
            add(key_of(n), i, MatchQuality::Exact, std::string(n));
            name_keys.emplace_back(key_of(n), i);
        }
        if (o.hr)
            add("hr" + std::to_string(o.hr), i, MatchQuality::Exact, "HR " + std::to_string(o.hr));
        if (o.hd)
            add("hd" + std::to_string(o.hd), i, MatchQuality::Exact, "HD " + std::to_string(o.hd));
        if (o.hip)
            add("hip" + std::to_string(o.hip), i, MatchQuality::Exact,
                "HIP " + std::to_string(o.hip));
        if (o.messier) {
            add("m" + std::to_string(o.messier), i, MatchQuality::Exact,
                "M " + std::to_string(o.messier));
            add("messier" + std::to_string(o.messier), i, MatchQuality::Exact,
                "M " + std::to_string(o.messier));
        }
        const Constellation* c = constellation_by_abbreviation(o.constellation);
        if (!c) {
            return;
        }
        const std::string con_keys[] = {key_of(c->abbreviation), key_of(c->genitive),
                                        key_of(c->name)};
        if (o.bayer >= 1 && o.bayer <= 24) {
            const Greek& g = kGreek[o.bayer - 1];
            const std::string form = o.bayer_designation();
            const std::string idx = o.bayer_index ? std::to_string(o.bayer_index) : "";
            for (const char* letter : {g.name, g.abbreviation, g.alternate}) {
                if (!*letter) {
                    continue;
                }
                for (const std::string& ck : con_keys) {
                    add(std::string(letter) + idx + ck, i, MatchQuality::Exact, form);
                    if (!idx.empty()) {
                        add(std::string(letter) + ck, i, MatchQuality::Alias, form);
                    }
                }
            }
        }
        if (o.flamsteed) {
            const std::string form = o.flamsteed_designation();
            for (const std::string& ck : con_keys) {
                add(std::to_string(o.flamsteed) + ck, i, MatchQuality::Exact, form);
            }
        }
    }
};

const Catalog& catalog() {
    static const Catalog c;
    return c;
}

bool brighter(const Object& a, const Object& b) {
    const double va = std::isnan(a.vmag) ? 99.0 : a.vmag;
    const double vb = std::isnan(b.vmag) ? 99.0 : b.vmag;
    return va < vb;
}

// B1875.0 (Besselian) as a TT Julian date.
constexpr double kB1875 = 2405889.258550475;
constexpr double kPi = 3.14159265358979323846;

} // namespace

// ---- Object -----------------------------------------------------------------

std::string Object::bayer_designation() const {
    if (bayer < 1 || bayer > 24) {
        return {};
    }
    std::string s = kGreek[bayer - 1].symbol;
    if (bayer_index > 0 && bayer_index < 10) {
        s += kSuperscripts[bayer_index];
    }
    return s + " " + std::string(constellation);
}

std::string Object::bayer_designation_long() const {
    const Constellation* c = constellation_by_abbreviation(constellation);
    if (bayer < 1 || bayer > 24 || !c) {
        return {};
    }
    std::string s = kGreek[bayer - 1].name;
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (bayer_index > 0) {
        s += std::to_string(bayer_index);
    }
    return s + " " + std::string(c->genitive);
}

std::string Object::flamsteed_designation() const {
    return flamsteed ? std::to_string(flamsteed) + " " + std::string(constellation) : std::string();
}

std::string Object::name() const {
    if (!names.empty()) {
        return std::string(names.front());
    }
    if (bayer) {
        return bayer_designation();
    }
    if (flamsteed) {
        return flamsteed_designation();
    }
    if (messier) {
        return "M " + std::to_string(messier);
    }
    if (hr) {
        return "HR " + std::to_string(hr);
    }
    return "HIP " + std::to_string(hip);
}

std::vector<std::string> Object::designations() const {
    std::vector<std::string> out;
    if (messier)
        out.push_back("M " + std::to_string(messier));
    if (bayer) {
        out.push_back(bayer_designation());
        out.push_back(bayer_designation_long());
    }
    if (flamsteed)
        out.push_back(flamsteed_designation());
    if (hr)
        out.push_back("HR " + std::to_string(hr));
    if (hd)
        out.push_back("HD " + std::to_string(hd));
    if (hip)
        out.push_back("HIP " + std::to_string(hip));
    return out;
}

// ---- Catalog access ---------------------------------------------------------------

size_t count() {
    return kRecords;
}

const Object& at(size_t index) {
    return catalog().objects[index];
}

std::vector<Match> lookup(std::string_view query, size_t max_matches, bool prefix) {
    const Catalog& cat = catalog();
    std::string key = key_of(query);
    // "Messier 45" and "M 45" read the same; "alpha1" keeps its superscript.
    std::vector<Match> out;
    if (key.empty() || max_matches == 0) {
        return out;
    }
    auto [lo, hi] = cat.index.equal_range(key);
    for (auto it = lo; it != hi; ++it) {
        out.push_back({it->second.index, it->second.quality, it->second.form});
    }
    if (prefix && key.size() >= 3) {
        for (const auto& [name_key, i] : cat.name_keys) {
            if (name_key.size() > key.size() && name_key.compare(0, key.size(), key) == 0 &&
                std::none_of(out.begin(), out.end(),
                             [i = i](const Match& m) { return m.index == i; })) {
                const Object& o = cat.objects[i];
                std::string_view shown;
                for (std::string_view n : o.names) {
                    if (key_of(n) == name_key) {
                        shown = n;
                    }
                }
                out.push_back({i, MatchQuality::Prefix, std::string(shown)});
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [&](const Match& a, const Match& b) {
        if (a.quality != b.quality) {
            return a.quality < b.quality;
        }
        const Object& oa = cat.objects[a.index];
        const Object& ob = cat.objects[b.index];
        if (brighter(oa, ob) != brighter(ob, oa)) {
            return brighter(oa, ob);
        }
        return a.index < b.index;
    });
    if (out.size() > max_matches) {
        out.resize(max_matches);
    }
    return out;
}

Result<size_t> find(std::string_view query) {
    const std::vector<Match> matches = lookup(query, 64, false);
    if (matches.empty()) {
        return make_error(ErrorCode::NotFound,
                          "no star or deep-sky object named '" + std::string(query) + "'");
    }
    const MatchQuality best = matches.front().quality;
    std::vector<const Object*> tied;
    for (const Match& m : matches) {
        if (m.quality == best) {
            tied.push_back(&at(m.index));
        }
    }
    if (tied.size() == 1) {
        return matches.front().index;
    }
    // Components of one designation (beta1 and beta2 Sco for "Beta Sco"):
    // the brightest, which lookup() ordered first.
    const auto same_designation = [&](const Object* o) {
        return (o->bayer && o->bayer == tied[0]->bayer &&
                o->constellation == tied[0]->constellation) ||
               (o->flamsteed && o->flamsteed == tied[0]->flamsteed &&
                o->constellation == tied[0]->constellation) ||
               (o->hip && o->hip == tied[0]->hip);
    };
    if (std::all_of(tied.begin(), tied.end(), same_designation)) {
        return matches.front().index;
    }
    std::string list;
    for (size_t k = 0; k < tied.size() && k < 5; ++k) {
        list += (k ? ", " : "") + tied[k]->name();
    }
    return make_error(ErrorCode::ArgumentError,
                      "'" + std::string(query) + "' is ambiguous: " + list);
}

const std::vector<Constellation>& constellations() {
    return kConstellations;
}

std::string_view constellation_at(double ra_deg, double dec_deg) {
    static const auto to_b1875 = [] {
        std::array<double, 9> m{};
        double p[9], b[9];
        frames::mean_equator_of_date_matrix(kB1875, p);
        frames::frame_bias_matrix(b);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r * 3 + c] = p[r * 3] * b[c] + p[r * 3 + 1] * b[3 + c] + p[r * 3 + 2] * b[6 + c];
        return m;
    }();
    const double a = ra_deg * kPi / 180.0, d = dec_deg * kPi / 180.0;
    const double v[3] = {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
    double w[3];
    for (int r = 0; r < 3; ++r)
        w[r] = to_b1875[r * 3] * v[0] + to_b1875[r * 3 + 1] * v[1] + to_b1875[r * 3 + 2] * v[2];
    double ra_h = std::atan2(w[1], w[0]) * 12.0 / kPi;
    if (ra_h < 0.0)
        ra_h += 24.0;
    const double dec = std::asin(std::clamp(w[2], -1.0, 1.0)) * 180.0 / kPi;
    for (const ConstellationBoundary& b : kConstellationBoundaries) {
        if (dec >= b.dec_from_deg && ra_h >= b.ra_from_h && ra_h < b.ra_to_h) {
            const Constellation* c = constellation_by_abbreviation(b.abbreviation);
            return c ? c->abbreviation : std::string_view{};
        }
    }
    return {};
}

std::string_view greek_letter_name(int letter) {
    return letter >= 1 && letter <= 24 ? kGreek[letter - 1].name : "";
}
std::string_view greek_letter_abbreviation(int letter) {
    return letter >= 1 && letter <= 24 ? kGreek[letter - 1].abbreviation : "";
}
std::string_view greek_letter_symbol(int letter) {
    return letter >= 1 && letter <= 24 ? kGreek[letter - 1].symbol : "";
}

} // namespace prometheia::stars
