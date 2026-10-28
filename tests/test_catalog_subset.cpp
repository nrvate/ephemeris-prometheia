// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-catalog-subset (tools/convert/subset_main.cpp), run as a
// subprocess on the in-tree sample catalog: a cut holds exactly the bodies
// its list names, each record as the source has it, the same list gives the
// same bytes, and a line naming no body stops the cut.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
using prometheia::CborValue;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& stem) {
    return fs::temp_directory_path() /
           ("prometheia-subset-" + std::to_string(::getpid()) + "-" + stem);
}

std::string slurp(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

int run_subset(const fs::path& list, const fs::path& out, std::string& err,
               const std::string& overlays = "") {
    const fs::path err_path = temp_path("stderr");
    const std::string cmd = std::string(SUBSET_BIN) + " " + PROMETHEIA_SOURCE_DIR +
                            "/tests/data/sample-100.epm " + overlays + list.string() + " -o " +
                            out.string() + " >/dev/null 2>" + err_path.string();
    const int st = std::system(cmd.c_str());
    err = slurp(err_path);
    fs::remove(err_path);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream(p) << text;
}

} // namespace

TEST_CASE("catalog_subset_copies_exactly_the_listed_bodies") {
    const std::string source = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    auto src = catalog::Reader::open(source);
    REQUIRE(src.ok());

    const fs::path list = temp_path("list.txt"), out = temp_path("out.epm"),
                   again = temp_path("again.epm");
    // A name, a number with a comment, a name in another case, a blank line.
    write(list, "Ceres\n2   # Pallas, by number\n\nVESTA\n");
    std::string err;
    REQUIRE(run_subset(list, out, err) == 0);
    auto cut = catalog::Reader::open(out.string());
    REQUIRE(cut.ok());
    CHECK(cut.value().record_count() == 3);

    int seen = 0;
    auto each = cut.value().for_each([&](const catalog::Record& r, const catalog::Names& n) {
        auto s = src.value().lookup(r.spkid);
        REQUIRE(s.ok());
        const catalog::Record& w = s.value();
        const catalog::Names sn = catalog::record_names(src.value().name_pool(), w);
        CHECK(n.pdes == sn.pdes);
        CHECK(n.name == sn.name);
        CHECK(r.body_class == w.body_class);
        CHECK(r.flags == w.flags);
        CHECK(r.epoch_jtdb == w.epoch_jtdb);
        CHECK(r.a_au == w.a_au);
        CHECK(r.e == w.e);
        CHECK(r.inc_rad == w.inc_rad);
        CHECK(r.node_rad == w.node_rad);
        CHECK(r.argp_rad == w.argp_rad);
        CHECK(r.mean_anom_rad == w.mean_anom_rad);
        for (int i = 0; i < 6; ++i)
            CHECK(r.sigmas[i] == w.sigmas[i]);
        CHECK(r.h_mag == w.h_mag);
        CHECK(r.g_slope == w.g_slope);
        CHECK(r.diameter_km == w.diameter_km);
        ++seen;
    });
    CHECK(each.ok());
    CHECK(seen == 3);

    // The same list, the same bytes: what lets a cut be pinned.
    REQUIRE(run_subset(list, again, err) == 0);
    CHECK(slurp(out) == slurp(again));

    // A line naming no body stops the cut, and says which line.
    write(list, "Ceres\nNo Such Body\n");
    CHECK(run_subset(list, again, err) == 1);
    CHECK(err.find(":2: 'No Such Body' matches no body") != std::string::npos);
    // --skip-missing names it and cuts the rest.
    {
        const fs::path err_path = temp_path("stderr-skip");
        const std::string cmd = std::string(SUBSET_BIN) + " " + PROMETHEIA_SOURCE_DIR +
                                "/tests/data/sample-100.epm " + list.string() + " -o " +
                                again.string() + " --skip-missing >/dev/null 2>" +
                                err_path.string();
        const int st = std::system(cmd.c_str());
        CHECK((WIFEXITED(st) && WEXITSTATUS(st) == 0));
        CHECK(slurp(err_path).find("'No Such Body' matches no body (skipped)") !=
              std::string::npos);
        fs::remove(err_path);
        auto skipped = catalog::Reader::open(again.string());
        REQUIRE(skipped.ok());
        CHECK(skipped.value().record_count() == 1);
    }

    fs::remove(list);
    fs::remove(out);
    fs::remove(again);
}

// A base and an overlay, as the engine stacks them: the overlay's record of
// a body replaces the base's whole, names included, so a body renamed there
// answers to its new name only and carries the overlay's orbit.
TEST_CASE("catalog_subset_stacks_overlays_newest_wins") {
    const std::string source = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/sample-100.epm";
    auto src = catalog::Reader::open(source);
    REQUIRE(src.ok());
    auto ceres = src.value().lookup(20000001);
    REQUIRE(ceres.ok());
    catalog::Record renamed = ceres.value();
    renamed.a_au += 1e-6;

    const fs::path overlay = temp_path("overlay.epm"), list = temp_path("stack.txt"),
                   out = temp_path("stack.epm");
    {
        auto w = catalog::Writer::create(overlay.string(), catalog::WriterOptions{});
        REQUIRE(w.ok());
        REQUIRE(w.value().add(renamed, "1", "Ceres Renamed").ok());
        REQUIRE(w.value().finish(CborValue::make_map()).ok());
    }
    std::string err;
    write(list, "Ceres Renamed\nPallas\n");
    REQUIRE(run_subset(list, out, err, overlay.string() + " ") == 0);
    auto cut = catalog::Reader::open(out.string());
    REQUIRE(cut.ok());
    CHECK(cut.value().record_count() == 2);
    auto got = cut.value().lookup(20000001);
    REQUIRE(got.ok());
    CHECK(got.value().a_au == renamed.a_au);
    CHECK(catalog::record_names(cut.value().name_pool(), got.value()).name == "Ceres Renamed");

    // --all folds the overlay into the base: every body, the newest record
    // of each, in one catalog that needs nothing stacked.
    REQUIRE(run_subset("--all", out, err, overlay.string() + " ") == 0);
    auto folded = catalog::Reader::open(out.string());
    REQUIRE(folded.ok());
    CHECK(folded.value().record_count() == src.value().record_count());
    auto f = folded.value().lookup(20000001);
    REQUIRE(f.ok());
    CHECK(f.value().a_au == renamed.a_au);
    auto pallas = folded.value().lookup(20000002);
    REQUIRE(pallas.ok());
    CHECK(pallas.value().a_au == src.value().lookup(20000002).value().a_au);

    // The base's old name no longer names anything.
    write(list, "Ceres\n");
    CHECK(run_subset(list, out, err, overlay.string() + " ") == 1);
    CHECK(err.find("'Ceres' matches no body") != std::string::npos);

    fs::remove(overlay);
    fs::remove(list);
    fs::remove(out);
}

TEST_CASE("catalog_subset_names_a_body_reached_by_its_old_designation") {
    // A 1.2 overlay: a numbered body with its old provisional designation and
    // orbit class. A list line by the old designation is served, the cutter
    // says so on stderr (Astrolog's bundle records which body it got), and
    // the cut carries both fields.
    const fs::path overlay = temp_path("overlay12.epm"), list = temp_path("old.txt"),
                   out = temp_path("old.epm");
    {
        auto src = catalog::Reader::open(std::string(PROMETHEIA_SOURCE_DIR) +
                                         "/tests/data/sample-100.epm");
        REQUIRE(src.ok());
        auto ceres = src.value().lookup(20000001);
        REQUIRE(ceres.ok());
        catalog::Record r = ceres.value();
        r.spkid = 20523794;
        r.orbit_class = catalog::OrbitClass::TNO;
        auto w = catalog::Writer::create(overlay.string(), catalog::WriterOptions{});
        REQUIRE(w.ok());
        REQUIRE(w.value().add(r, "523794", "", "2015 RR245").ok());
        REQUIRE(w.value().finish(CborValue::make_map()).ok());
    }
    write(list, "2015 RR245\nCeres\n");
    std::string err;
    REQUIRE(run_subset(list, out, err, overlay.string() + " ") == 0);
    CHECK(err.find("'2015 RR245' -> 523794 (old designation)") != std::string::npos);
    CHECK(err.find("Ceres") == std::string::npos); // reached by its name: nothing said
    auto cut = catalog::Reader::open(out.string());
    REQUIRE(cut.ok());
    auto got = cut.value().lookup(20523794);
    REQUIRE(got.ok());
    CHECK(got.value().orbit_class == catalog::OrbitClass::TNO);
    CHECK(catalog::record_names(cut.value().name_pool(), got.value()).prov_des == "2015 RR245");
    for (const fs::path& p : {overlay, list, out})
        fs::remove(p);
}
