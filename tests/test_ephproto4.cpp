// SPDX-License-Identifier: GPL-2.0-or-later
//
// The protocol v4 conformance pin, in-tree: Astrolog's fixture set (3.10,
// ephsrv/conformance/ in their tree, vendored read-only from the drop) run
// through the vendored codec, third_party/ephproto/v4/ephproto.h. The
// manifest carries each fixture's expected verdict; a disagreement between
// the codec and the manifest means the pinned pair moved and must be
// re-reviewed and re-pinned (third_party/README.md). The set is also
// checksummed (the manifest's set-sha256 over the fixtures in row order) and
// pinned: a changed set fails here until it is re-pinned deliberately.
//
// A second, independent reader of the same fixtures lives in
// tools/check/ephproto4_fixtures.py; the registries-by-name check is
// tools/check/ephproto4_registries.py (run by tools/gate.sh).
//
// Where the fixtures come from: $PROMETHEIA_EPHPROTO4 (a conformance
// directory), else $PROMETHEIA_ASTROLOG/ephsrv/conformance. SKIPs when
// neither is set or the directory is not there.
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ephproto.h>
#include <prometheia/elements.hpp>
#include <prometheia/engine.hpp>
#include <prometheia/hypotheticals.hpp>

#include <doctest/doctest.h>

namespace {

// The locked set, recorded in third_party/README.md at the 91/91 verdict.
constexpr const char* kPinnedSetSha256 =
    "1c934c7da19965f21ded99a0a53e36eaa3c45434cfbfaeabddafaf154ea454ec";

// FIPS 180-4 SHA-256, only to check the fixture set's digest against the
// manifest's and the pin. Public-domain algorithm, written from the standard.
class Sha256 {
public:
    void update(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            buf_[len_++] = p[i];
            if (len_ == 64) {
                block(buf_);
                len_ = 0;
            }
        }
    }
    std::string hex() {
        const uint64_t bits = uint64_t(blocks_) * 512 + uint64_t(len_) * 8;
        update(reinterpret_cast<const uint8_t*>("\x80"), 1);
        while (len_ != 56) {
            update(reinterpret_cast<const uint8_t*>("\0"), 1);
        }
        for (int i = 7; i >= 0; --i) {
            const uint8_t b = uint8_t(bits >> (8 * i));
            update(&b, 1);
        }
        std::string out;
        char t[9];
        for (int i = 0; i < 8; ++i) {
            std::snprintf(t, sizeof t, "%08x", h_[i]);
            out += t;
        }
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static constexpr uint32_t k[] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
        ++blocks_;
    }
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64];
    size_t len_ = 0;
    uint64_t blocks_ = 0;
};

std::string slurp(const std::string& path, bool* ok) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *ok = false;
        return {};
    }
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    *ok = true;
    return out;
}

std::vector<uint8_t> unhex(const std::string& text) {
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char c : text) {
        if (std::isspace(unsigned(c))) {
            continue;
        }
        const int v = std::isdigit(unsigned(c)) ? c - '0'
                      : c >= 'a' && c <= 'f'    ? c - 'a' + 10
                      : c >= 'A' && c <= 'F'    ? c - 'A' + 10
                                                : -1;
        REQUIRE(v >= 0);
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(uint8_t(hi * 16 + v));
            hi = -1;
        }
    }
    REQUIRE(hi < 0); // an even number of hex digits
    return out;
}

struct Row {
    std::string file, direction, expect, note;
    int type = 0;
};

const char* outcome_name(eph::Outcome o) {
    return o == eph::kOk ? "ok" : o == eph::kMalformed ? "malformed" : "unsupported";
}

} // namespace

TEST_CASE("ephproto4_conformance_fixtures") {
    const char* env = std::getenv("PROMETHEIA_EPHPROTO4");
    std::string dir;
    if (env && *env) {
        dir = env;
    } else if (const char* al = std::getenv("PROMETHEIA_ASTROLOG"); al && *al) {
        dir = std::string(al) + "/ephsrv/conformance";
    }
    if (dir.empty()) {
        std::printf("  SKIP: neither PROMETHEIA_EPHPROTO4 nor PROMETHEIA_ASTROLOG is set\n");
        return;
    }
    if (dir.back() == '/') {
        dir.pop_back();
    }
    bool ok = false;
    const std::string manifest = slurp(dir + "/MANIFEST.tsv", &ok);
    REQUIRE_MESSAGE(ok, ("no MANIFEST.tsv in " + dir));

    std::vector<Row> rows;
    std::string set_sha;
    size_t line_start = 0;
    while (line_start <= manifest.size()) {
        size_t line_end = manifest.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = manifest.size();
        }
        const std::string line = manifest.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (line.empty()) {
            if (line_start > manifest.size()) {
                break;
            }
            continue;
        }
        if (line[0] == '#') {
            if (const char* p = std::strstr(line.c_str(), "set-sha256")) {
                set_sha = std::string(p + 11, 64); // past "set-sha256 "
            }
            continue;
        }
        Row r;
        size_t at = 0;
        for (int cell = 0; cell < 4; ++cell) {
            const size_t tab = line.find('\t', at);
            REQUIRE(tab != std::string::npos);
            const std::string v = line.substr(at, tab - at);
            switch (cell) {
            case 0:
                r.file = v;
                break;
            case 1:
                r.direction = v;
                break;
            case 2:
                r.type = std::atoi(v.c_str());
                break;
            case 3:
                r.expect = v;
                break;
            }
            at = tab + 1;
        }
        r.note = line.substr(at);
        rows.push_back(std::move(r));
        if (line_start > manifest.size()) {
            break;
        }
    }
    REQUIRE(rows.size() >= 80); // the locked set has 91; far fewer means a bad read

    // The digest over the fixtures in row order: first against the manifest
    // (a half-written set reports here, not as bogus verdicts), then against
    // the pin (a changed set is a re-review and a deliberate re-pin).
    Sha256 sha;
    for (const Row& r : rows) {
        const std::string bytes = slurp(dir + "/" + r.file, &ok);
        REQUIRE_MESSAGE(ok, ("cannot read " + r.file));
        sha.update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }
    const std::string got = sha.hex();
    CHECK_MESSAGE(got == set_sha, ("fixture set is inconsistent (half-written?): " + got));
    CHECK_MESSAGE(got == kPinnedSetSha256,
                  ("the fixture set changed (" + got.substr(0, 16) +
                   "\u2026): review it against the spec and re-pin kPinnedSetSha256"));

    int agree = 0;
    int reencoded = 0;
    std::vector<std::string> bad;
    for (const Row& r : rows) {
        const std::string text = slurp(dir + "/" + r.file, &ok);
        REQUIRE(ok);
        const std::vector<uint8_t> bytes = unhex(text);
        eph::Envelope env{};
        std::string why;
        const eph::Outcome o = eph::ParseFrame(bytes.data(), bytes.size(), &env, &why);
        const bool good = outcome_name(o) == r.expect;
        if (good && o == eph::kOk) {
            // 3.10: a frame that parses must re-encode to its own bytes.
            std::vector<uint8_t> again;
            REQUIRE(eph::ReencodeFrame(bytes.data(), bytes.size(), &again));
            if (again == bytes) {
                ++reencoded;
            } else {
                bad.push_back(r.file + ": re-encode differs from the fixture");
            }
        } else if (good) {
            ++agree;
            continue;
        } else {
            bad.push_back(r.file + ": manifest says " + r.expect + ", codec says " +
                          outcome_name(o) + (why.empty() ? "" : " \u2014 " + why) + " (" + r.note +
                          ")");
        }
        if (good) {
            ++agree;
        }
    }
    for (const std::string& b : bad) {
        MESSAGE("DISAGREE ", b);
    }
    CHECK_MESSAGE(bad.empty(), (std::to_string(bad.size()) + " fixture disagreement(s)"));
    CHECK(agree == int(rows.size()));
    CHECK_MESSAGE(reencoded > 0, std::string("no ok fixture re-encoded to its own bytes"));
}

// ---- the kind-4 elements rule: the protocol owner's numeric fixture --------

TEST_CASE("ephproto4_kind4_elements_fixture") {
    // "The kind-4 elements rule" (Astrolog ephv4 176e333; fixture generated by
    // tools/kind4-fixture.sh at 20dd09c), vendored with its digest pinned.
    // The inputs are four invented element sets, in our own format in
    // tests/data/kind4-cases.jsonl; the fixture holds the Astrolog
    // implementation's answers: geometric, rectangular, the mean ecliptic and
    // equinox of J2000, from the orbit's origin -- a pure function of the
    // elements, so no anchor is needed to judge a disagreement.
    constexpr const char* kPinnedFixtureSha256 =
        "121ab3c6f9bed858b3f8aa949843f8e045850680054ca11aa184e289b58b3e63";
    const std::string root = PROMETHEIA_SOURCE_DIR;
    bool ok = false;
    const std::string fixture = slurp(root + "/third_party/ephproto/v4/elements/FIXTURE.tsv", &ok);
    REQUIRE(ok);
    Sha256 sha;
    sha.update(reinterpret_cast<const uint8_t*>(fixture.data()), fixture.size());
    CHECK(sha.hex() == kPinnedFixtureSha256);

    const std::string de = std::getenv("PROMETHEIA_DE440") && *std::getenv("PROMETHEIA_DE440")
                               ? std::getenv("PROMETHEIA_DE440")
                               : root + "/ephe/linux_p1550p2650.440";
    auto opened = prometheia::Engine::open(de);
    if (!opened) {
        std::printf("  SKIP: %s not present\n", de.c_str());
        return;
    }
    prometheia::Engine& e = opened.value();
    auto added = e.add_hypotheticals(root + "/tests/data/kind4-cases.jsonl");
    REQUIRE_MESSAGE(added.ok(), added.error().message);

    int rows = 0;
    size_t at = 0;
    while (at < fixture.size()) {
        const size_t nl = fixture.find('\n', at);
        const std::string line = fixture.substr(at, nl == std::string::npos ? nl : nl - at);
        at = nl == std::string::npos ? fixture.size() : nl + 1;
        if (line.empty() || line[0] == '#')
            continue;
        char tag = 0;
        double jd = 0, want[3] = {};
        REQUIRE(std::sscanf(line.c_str(), " %c %lf %lf %lf %lf", &tag, &jd, &want[0], &want[1],
                            &want[2]) == 5);
        const std::string token = std::string("case-") + char(std::tolower(tag));
        const prometheia::hypotheticals::Body* b = e.hypothetical(token);
        REQUIRE_MESSAGE(b, token);
        const prometheia::PolynomialElements& el = b->elements;

        prometheia::CalcOptions o = prometheia::CalcOptions::geometric();
        o.frame = prometheia::Frame::J2000;
        o.center = el.origin == prometheia::ElementOrigin::Earth ? prometheia::Center::Geocentric
                                                                 : prometheia::Center::Heliocentric;
        auto got = e.calc_hypothetical(token, jd, o);
        REQUIRE_MESSAGE(got.ok(), got.error().message);
        const double* x = got.value().pos.xyz_au;
        const double dr =
            std::sqrt((x[0] - want[0]) * (x[0] - want[0]) + (x[1] - want[1]) * (x[1] - want[1]) +
                      (x[2] - want[2]) * (x[2] - want[2]));
        const double r = std::sqrt(want[0] * want[0] + want[1] * want[1] + want[2] * want[2]);

        // The one known difference (DROP.md): Swiss carries k as a decimal in
        // degrees per day, 1.446e-12 relative off the protocol's value, so a
        // row disagrees in proportion to the mean anomaly the mean motion
        // accumulates -- none on the full-polynomial branch or at the epoch.
        bool at_epoch_branch = true;
        for (int j = 1; j < el.n_terms; ++j)
            at_epoch_branch = at_epoch_branch && el.mean_anomaly[j] == 0.0;
        double accumulated = 0.0; // radians of mean anomaly from the mean motion
        if (at_epoch_branch) {
            double n = prometheia::elements::kGaussK / std::pow(el.semi_major_axis[0], 1.5);
            if (el.origin == prometheia::ElementOrigin::Earth)
                n /= std::sqrt(prometheia::elements::kSunEarthMassRatio);
            accumulated = n * std::fabs(jd - el.epoch_jd_tt);
        }
        const double bound = 1.2 * 1.446e-12 * accumulated + 1e-14;
        CHECK_MESSAGE(dr / r <= bound, token << " at " << jd << ": " << dr / r << " > " << bound);
        ++rows;
    }
    CHECK(rows == 8);
}
