// SPDX-License-Identifier: GPL-2.0-or-later
#include "dataset.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "sha256.hpp"

namespace prometheia::server {
namespace {

// A file's display name: its basename, no server paths on the wire.
std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void hash_file(Sha256& sha, const std::string& tag, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    // The file's ROLE and CONTENTS, not its name: same bytes, same dataset.
    // The trailing newline keeps one file's end off the next one's start.
    sha.update(reinterpret_cast<const uint8_t*>(tag.data()), tag.size());
    sha.update(reinterpret_cast<const uint8_t*>("\0"), 1);
    if (!f) {
        const std::string missing = "missing\n";
        sha.update(reinterpret_cast<const uint8_t*>(missing.data()), missing.size());
        return;
    }
    std::vector<char> buf(1 << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        sha.update(reinterpret_cast<const uint8_t*>(buf.data()), n);
    }
    std::fclose(f);
    sha.update(reinterpret_cast<const uint8_t*>("\n"), 1);
}

} // namespace

Dataset make_dataset(std::string engine, const std::string& ephemeris_path,
                     const std::vector<std::string>& catalog_paths,
                     const std::string& perturbers_path) {
    Dataset d;
    d.engine = std::move(engine);
    d.ephemeris = base_name(ephemeris_path);
    for (const std::string& c : catalog_paths) {
        d.catalogs.push_back(base_name(c));
    }
    if (!perturbers_path.empty()) {
        d.catalogs.push_back(base_name(perturbers_path) + " (perturbers)");
    }
    Sha256 sha;
    sha.update(reinterpret_cast<const uint8_t*>(d.engine.data()), d.engine.size());
    hash_file(sha, "ephemeris", ephemeris_path);
    for (const std::string& c : catalog_paths) {
        hash_file(sha, "catalog", c);
    }
    if (!perturbers_path.empty()) {
        hash_file(sha, "perturbers", perturbers_path);
    }
    const std::string hex = sha.hex();
    d.id = d.engine + "/" + d.ephemeris + "/" +
           (d.catalogs.empty() ? std::string("-")
                               : d.catalogs[0] + (d.catalogs.size() > 1
                                                      ? "+" + std::to_string(d.catalogs.size() - 1)
                                                      : std::string())) +
           "#" + hex.substr(0, 8);
    return d;
}

} // namespace prometheia::server
