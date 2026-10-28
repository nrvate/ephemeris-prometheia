// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-catalog-subset — cut EPM1 catalogs down to a list of bodies,
// for a client that ships a few named bodies rather than 1.5 million
// (docs/INGESTION.md, "A catalog of named bodies"). Several catalogs stack as
// the engine stacks them: a base, then overlays, the newest record of a body
// winning, names included. Each record is copied as it is, and the output
// names no time, so the same catalogs and list always give the same bytes.
//
// The list has one body per line: its number or primary designation as SBDB
// gives it ("1", "2060", "2002 AW197"), its name ("Ceres") or, from a 1.2
// catalog, a numbered body's old provisional designation, matched ASCII
// case-insensitively; '#' starts a comment. A line that reaches a body only by
// its old provisional designation is served and named on stderr, as
// "LIST:N: '2015 RR245' -> 523794 (old designation)". A line that matches no body, or
// more than one, stops the cut and is named: nothing is guessed. With
// --skip-missing such a line is named and left out, and the cut goes on.
//
// With --all and no list it keeps every body: a refresh overlay folded into
// its base, written as one up-to-date catalog that needs nothing stacked.
//
// Usage: prometheia-catalog-subset <in.epm>... <list.txt> -o <out.epm> [--skip-missing]
//        prometheia-catalog-subset <in.epm>... --all -o <out.epm>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>
#include <prometheia/prometheia.hpp>

namespace {

using prometheia::CborValue;
namespace catalog = prometheia::catalog;

std::string fold(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip(const std::string& s) {
    const size_t hash = s.find('#');
    std::string t = s.substr(0, hash);
    const size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = t.find_last_not_of(" \t\r\n");
    return t.substr(a, b - a + 1);
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <in.epm>... <list.txt> -o <out.epm> [--skip-missing]\n"
                 "       %s <in.epm>... --all -o <out.epm>\n",
                 argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::string out_path;
    bool all = false, skip_missing = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--all") {
            all = true;
        } else if (a == "--skip-missing") {
            skip_missing = true;
        } else if (a == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < (all ? 1u : 2u) || out_path.empty()) {
        usage(argv[0]);
        return 2;
    }
    const std::string list_path = all ? std::string() : positional.back();
    const std::vector<std::string> in_paths(positional.begin(),
                                            all ? positional.end() : positional.end() - 1);

    std::vector<catalog::Reader> readers;
    for (const std::string& path : in_paths) {
        auto opened = catalog::Reader::open(path);
        if (!opened.ok()) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), opened.error().message.c_str());
            return 1;
        }
        readers.push_back(std::move(opened).value());
    }

    // Each body's newest record (the catalog it comes from) and its names
    // there: a later catalog replaces an earlier one's record whole, as in
    // the engine, so a body named in an overlay answers to its new name.
    struct Newest {
        size_t reader;
        std::string pdes, name, prov_des;
    };
    std::map<uint64_t, Newest> newest;
    for (size_t k = 0; k < readers.size(); ++k) {
        auto indexed = readers[k].for_each_name([&](uint64_t spkid, const catalog::Names& n) {
            newest[spkid] =
                Newest{k, std::string(n.pdes), std::string(n.name), std::string(n.prov_des)};
        });
        if (!indexed.ok()) {
            std::fprintf(stderr, "%s: %s\n", in_paths[k].c_str(), indexed.error().message.c_str());
            return 1;
        }
    }
    std::set<uint64_t> wanted;
    if (all) {
        for (const auto& [spkid, n] : newest)
            wanted.insert(wanted.end(), spkid);
    }
    // Every key a line may use, to the SPK-IDs it names; and the keys that
    // are only some numbered body's old provisional designation (1.2), so a
    // line that reaches a body that way says so.
    std::map<std::string, std::set<uint64_t>> keys;
    std::set<std::string> current;
    if (!all) {
        for (const auto& [spkid, n] : newest) {
            if (!n.pdes.empty()) {
                keys[fold(n.pdes)].insert(spkid);
                current.insert(fold(n.pdes));
            }
            if (!n.name.empty()) {
                keys[fold(n.name)].insert(spkid);
                current.insert(fold(n.name));
            }
            if (!n.prov_des.empty())
                keys[fold(n.prov_des)].insert(spkid);
        }
    }

    std::ifstream list;
    if (!all) {
        list.open(list_path);
        if (!list) {
            std::fprintf(stderr, "cannot read %s\n", list_path.c_str());
            return 1;
        }
    }
    std::vector<std::string> problems;
    std::string line;
    for (int lineno = 1; !all && std::getline(list, line); ++lineno) {
        const std::string body = strip(line);
        if (body.empty())
            continue;
        auto it = keys.find(fold(body));
        if (it == keys.end()) {
            problems.push_back(list_path + ":" + std::to_string(lineno) + ": '" + body +
                               "' matches no body");
        } else if (it->second.size() > 1) {
            problems.push_back(list_path + ":" + std::to_string(lineno) + ": '" + body +
                               "' matches " + std::to_string(it->second.size()) + " bodies");
        } else {
            const uint64_t spkid = *it->second.begin();
            wanted.insert(spkid);
            if (!current.count(it->first))
                std::fprintf(stderr, "%s:%d: '%s' -> %s (old designation)\n", list_path.c_str(),
                             lineno, body.c_str(), newest[spkid].pdes.c_str());
        }
    }
    for (const std::string& p : problems)
        std::fprintf(stderr, "%s%s\n", p.c_str(), skip_missing ? " (skipped)" : "");
    if (!problems.empty() && !skip_missing)
        return 1;
    if (wanted.empty()) {
        std::fprintf(stderr, "%s names no body\n", list_path.c_str());
        return 1;
    }

    auto w = catalog::Writer::create(out_path, catalog::WriterOptions{});
    if (!w.ok()) {
        std::fprintf(stderr, "%s: %s\n", out_path.c_str(), w.error().message.c_str());
        return 1;
    }
    catalog::Writer& writer = w.value();
    for (uint64_t spkid : wanted) { // a std::set: ascending, as the writer requires
        const catalog::Reader& reader = readers[newest.at(spkid).reader];
        auto r = reader.lookup(spkid);
        if (!r.ok()) {
            std::fprintf(stderr, "spkid %llu: %s\n", (unsigned long long)spkid,
                         r.error().message.c_str());
            return 1;
        }
        const catalog::Names n = catalog::record_names(reader.name_pool(), r.value());
        if (auto e = writer.add(r.value(), n.pdes, n.name, n.prov_des); !e.ok()) {
            std::fprintf(stderr, "spkid %llu: %s\n", (unsigned long long)spkid,
                         e.error().message.c_str());
            return 1;
        }
    }

    // The first catalog's metadata, then what was cut and from what. No time
    // and no tool version, so the output depends only on the inputs.
    CborValue meta = readers.front().metadata();
    if (meta.type != CborValue::Type::Map)
        meta = CborValue::make_map();
    std::string from;
    for (const std::string& path : in_paths)
        from += (from.empty() ? "" : " + ") + std::filesystem::path(path).filename().string();
    meta.items.push_back(CborValue::make_text("subset_of"));
    meta.items.push_back(CborValue::make_text(from));
    meta.items.push_back(CborValue::make_text("subset_of_records"));
    meta.items.push_back(CborValue::make_unsigned(newest.size()));
    if (auto e = writer.finish(std::move(meta)); !e.ok()) {
        std::fprintf(stderr, "%s: %s\n", out_path.c_str(), e.error().message.c_str());
        return 1;
    }
    std::printf("%s: %zu of %zu bodies -> %s (%llu bytes)\n", from.c_str(), wanted.size(),
                newest.size(), out_path.c_str(),
                (unsigned long long)std::filesystem::file_size(out_path));
    return 0;
}
