// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-catalog-subset — cut an EPM1 catalog down to a list of bodies,
// for a client that ships a few named bodies rather than 1.5 million
// (docs/INGESTION.md, "A catalog of named bodies"). Each record is copied as
// it is, and the output names no time, so the same catalog and list always
// give the same bytes.
//
// The list has one body per line: its number or primary designation as SBDB
// gives it ("1", "2060", "2002 AW197") or its name ("Ceres"), matched ASCII
// case-insensitively; '#' starts a comment. A line that matches no body, or
// more than one, stops the cut and is named: nothing is guessed.
//
// Usage: prometheia-catalog-subset <in.epm> <list.txt> -o <out.epm>
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
    std::fprintf(stderr, "usage: %s <in.epm> <list.txt> -o <out.epm>\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string in_path, list_path, out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (in_path.empty()) {
            in_path = a;
        } else if (list_path.empty()) {
            list_path = a;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (in_path.empty() || list_path.empty() || out_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    auto opened = catalog::Reader::open(in_path);
    if (!opened.ok()) {
        std::fprintf(stderr, "%s: %s\n", in_path.c_str(), opened.error().message.c_str());
        return 1;
    }
    const catalog::Reader& reader = opened.value();

    // Every key a line may use, to the SPK-IDs it names.
    std::map<std::string, std::set<uint64_t>> keys;
    auto indexed = reader.for_each_name([&](uint64_t spkid, const catalog::Names& n) {
        if (!n.pdes.empty())
            keys[fold(std::string(n.pdes))].insert(spkid);
        if (!n.name.empty())
            keys[fold(std::string(n.name))].insert(spkid);
    });
    if (!indexed.ok()) {
        std::fprintf(stderr, "%s: %s\n", in_path.c_str(), indexed.error().message.c_str());
        return 1;
    }

    std::ifstream list(list_path);
    if (!list) {
        std::fprintf(stderr, "cannot read %s\n", list_path.c_str());
        return 1;
    }
    std::set<uint64_t> wanted;
    std::vector<std::string> problems;
    std::string line;
    for (int lineno = 1; std::getline(list, line); ++lineno) {
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
            wanted.insert(*it->second.begin());
        }
    }
    for (const std::string& p : problems)
        std::fprintf(stderr, "%s\n", p.c_str());
    if (!problems.empty())
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
        auto r = reader.lookup(spkid);
        if (!r.ok()) {
            std::fprintf(stderr, "spkid %llu: %s\n", (unsigned long long)spkid,
                         r.error().message.c_str());
            return 1;
        }
        const catalog::Names n = catalog::record_names(reader.name_pool(), r.value());
        if (auto e = writer.add(r.value(), n.pdes, n.name); !e.ok()) {
            std::fprintf(stderr, "spkid %llu: %s\n", (unsigned long long)spkid,
                         e.error().message.c_str());
            return 1;
        }
    }

    // The source's metadata, then what was cut and from what. No time and
    // no tool version, so the output depends only on the catalog and list.
    CborValue meta = reader.metadata();
    if (meta.type != CborValue::Type::Map)
        meta = CborValue::make_map();
    meta.items.push_back(CborValue::make_text("subset_of"));
    meta.items.push_back(CborValue::make_text(std::filesystem::path(in_path).filename().string()));
    meta.items.push_back(CborValue::make_text("subset_of_records"));
    meta.items.push_back(CborValue::make_unsigned(reader.record_count()));
    if (auto e = writer.finish(std::move(meta)); !e.ok()) {
        std::fprintf(stderr, "%s: %s\n", out_path.c_str(), e.error().message.c_str());
        return 1;
    }
    std::printf("%s: %zu of %llu bodies -> %s (%llu bytes)\n", in_path.c_str(), wanted.size(),
                (unsigned long long)reader.record_count(), out_path.c_str(),
                (unsigned long long)std::filesystem::file_size(out_path));
    return 0;
}
