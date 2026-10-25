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

#include <doctest/doctest.h>

using namespace prometheia;
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

int run_subset(const fs::path& list, const fs::path& out, std::string& err) {
    const fs::path err_path = temp_path("stderr");
    const std::string cmd = std::string(SUBSET_BIN) + " " + PROMETHEIA_SOURCE_DIR +
                            "/tests/data/sample-100.epm " + list.string() + " -o " + out.string() +
                            " >/dev/null 2>" + err_path.string();
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

    fs::remove(list);
    fs::remove(out);
    fs::remove(again);
}
