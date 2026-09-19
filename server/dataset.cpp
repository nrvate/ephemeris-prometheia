// SPDX-License-Identifier: GPL-2.0-or-later
#include "dataset.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "prometheia/hypotheticals.hpp"
#include "sha256.hpp"

namespace prometheia::server {
namespace {

// A file's display name: its basename, no server paths on the wire.
std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A file's digest, computed in pieces on every core: SHA-256 over the
// file's size and the SHA-256 of each kPiece-byte piece in order. The same
// bytes give the same digest, as a single stream would; unlike one stream it
// is not bound to one core (one stream was 97% of starting up, 2026-09-19).
constexpr size_t kPiece = size_t(8) << 20;

bool file_digest(const std::string& path, std::array<uint8_t, 32>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long end = std::ftell(f);
    std::fclose(f);
    if (end < 0)
        return false;
    const uint64_t size = uint64_t(end);
    const size_t pieces = size_t((size + kPiece - 1) / kPiece);
    std::vector<std::array<uint8_t, 32>> digests(pieces);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    const auto work = [&] {
        std::FILE* g = std::fopen(path.c_str(), "rb");
        if (!g) {
            failed = true;
            return;
        }
        std::vector<uint8_t> buf(kPiece);
        for (size_t i; (i = next++) < pieces;) {
            const uint64_t at = uint64_t(i) * kPiece;
            const size_t want = size_t(std::min<uint64_t>(kPiece, size - at));
            if (std::fseek(g, long(at), SEEK_SET) != 0 ||
                std::fread(buf.data(), 1, want, g) != want) {
                failed = true;
                break;
            }
            Sha256 h;
            h.update(buf.data(), want);
            digests[i] = h.bytes();
        }
        std::fclose(g);
    };
    const size_t n_threads = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()),
                                              std::max<size_t>(pieces, 1));
    std::vector<std::thread> threads;
    for (size_t t = 1; t < n_threads; ++t)
        threads.emplace_back(work);
    work();
    for (std::thread& t : threads)
        t.join();
    if (failed)
        return false;
    Sha256 whole;
    uint8_t sz[8];
    for (int i = 0; i < 8; ++i)
        sz[i] = uint8_t(size >> (56 - 8 * i));
    whole.update(sz, 8);
    for (const auto& d : digests)
        whole.update(d.data(), d.size());
    out = whole.bytes();
    return true;
}

void hash_file(Sha256& sha, const std::string& tag, const std::string& path) {
    // The file's ROLE and CONTENTS, not its name: same bytes, same dataset.
    // The trailing newline keeps one file's end off the next one's start.
    sha.update(reinterpret_cast<const uint8_t*>(tag.data()), tag.size());
    sha.update(reinterpret_cast<const uint8_t*>("\0"), 1);
    std::array<uint8_t, 32> d;
    if (!file_digest(path, d)) {
        const std::string missing = "missing\n";
        sha.update(reinterpret_cast<const uint8_t*>(missing.data()), missing.size());
        return;
    }
    sha.update(d.data(), d.size());
    sha.update(reinterpret_cast<const uint8_t*>("\n"), 1);
}

} // namespace

Dataset make_dataset(std::string engine, const std::string& ephemeris_path,
                     const std::vector<std::string>& catalog_paths,
                     const std::string& perturbers_path,
                     const std::vector<std::string>& hypothetical_paths) {
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
    // The shipped element set rides in the library, not a file, so its text
    // is hashed directly; an element file after it, like a catalog.
    const std::string_view shipped = hypotheticals::shipped();
    const std::string tag = "shipped hypotheticals";
    sha.update(reinterpret_cast<const uint8_t*>(tag.data()), tag.size());
    sha.update(reinterpret_cast<const uint8_t*>("\0"), 1);
    sha.update(reinterpret_cast<const uint8_t*>(shipped.data()), shipped.size());
    sha.update(reinterpret_cast<const uint8_t*>("\n"), 1);
    for (const std::string& h : hypothetical_paths) {
        hash_file(sha, "hypotheticals", h);
        d.hypotheticals.push_back(base_name(h));
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
