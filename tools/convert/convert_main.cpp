// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-convert — build an EPM1 catalog from sbdb-raw v1 TSV shards
// produced by tools/fetch/sbdb_fetch.py.
//
// Usage: prometheia-convert <sbdb-raw-dir> -o <out.epm> [--chunk N] [--level N]
//                           [--no-compress]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>
#include <prometheia/prometheia.hpp>

namespace {

using prometheia::CborValue;
namespace catalog = prometheia::catalog;

constexpr double kDegToRad = 0.017453292519943295769236;

struct FieldMap {
    std::vector<std::string> names;
    std::map<std::string, size_t> index;

    static std::optional<FieldMap> parse(const std::string& header_line) {
        const std::string prefix = "# fields: ";
        if (header_line.rfind(prefix, 0) != 0)
            return std::nullopt;
        FieldMap fm;
        size_t pos = prefix.size();
        while (pos <= header_line.size()) {
            size_t next = header_line.find('\t', pos);
            if (next == std::string::npos)
                next = header_line.size();
            fm.index[header_line.substr(pos, next - pos)] = fm.names.size();
            fm.names.push_back(header_line.substr(pos, next - pos));
            pos = next + 1;
        }
        return fm;
    }
    std::optional<size_t> col(const char* name) const {
        auto it = index.find(name);
        if (it == index.end())
            return std::nullopt;
        return it->second;
    }
};

struct ParseOutcome {
    bool ok = false;
    bool skipped = false; // e.g. hyperbolic a>0 inconsistency in source data
    catalog::Record record;
    std::string pdes, name;
};

double parse_double_or(const std::string& s, double fallback) {
    if (s.empty())
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

ParseOutcome parse_row(const std::vector<std::string>& cols, const FieldMap& fm, uint64_t line_no) {
    ParseOutcome out;
    auto col = [&](const char* n) -> const std::string& {
        static const std::string kEmpty;
        auto c = fm.col(n);
        return c && *c < cols.size() ? cols[*c] : kEmpty;
    };

    const std::string& spkid_s = col("spkid");
    if (spkid_s.empty())
        return out;
    char* end = nullptr;
    const uint64_t spkid = std::strtoull(spkid_s.c_str(), &end, 10);
    if (end == spkid_s.c_str() || spkid == 0) {
        std::fprintf(stderr, "skipping line %llu: bad spkid '%s'\n", (unsigned long long)line_no,
                     spkid_s.c_str());
        out.skipped = true;
        return out;
    }

    const std::string& epoch_s = col("epoch");
    const std::string& a_s = col("a");
    const std::string& e_s = col("e");
    const std::string& inc_s = col("i");
    const std::string& node_s = col("om");
    const std::string& argp_s = col("w");
    const std::string& ma_s = col("ma");
    if (epoch_s.empty() || a_s.empty() || e_s.empty()) {
        std::fprintf(stderr, "skipping spkid %llu: missing epoch/a/e\n", (unsigned long long)spkid);
        out.skipped = true;
        return out;
    }

    catalog::Record r;
    r.spkid = spkid;
    // kind column: query API gives bare "a"/"c"; the single-object API
    // (delta overlays) gives subtypes "an"/"au"/"cn"/"cu". Classify by the
    // leading letter.
    const std::string& kind = col("kind");
    r.body_class = (!kind.empty() && kind[0] == 'c') ? catalog::BodyClass::Comet
                                                     : catalog::BodyClass::Asteroid;
    r.epoch_jtdb = parse_double_or(epoch_s, 0.0);
    r.a_au = parse_double_or(a_s, 0.0);
    r.e = parse_double_or(e_s, 0.0);
    r.inc_rad = parse_double_or(inc_s, 0.0) * kDegToRad;
    r.node_rad = parse_double_or(node_s, 0.0) * kDegToRad;
    r.argp_rad = parse_double_or(argp_s, 0.0) * kDegToRad;
    r.mean_anom_rad = parse_double_or(ma_s, 0.0) * kDegToRad;

    bool have_sigma = false;
    static const char* kSigmaCols[6] = {"sigma_a",  "sigma_e", "sigma_i",
                                        "sigma_om", "sigma_w", "sigma_ma"};
    // SBDB delivers angle sigmas in degrees; the record stores them in the
    // elements' units (radians), per docs/FORMAT.md.
    static constexpr double kSigmaScale[6] = {1.0, 1.0, kDegToRad, kDegToRad, kDegToRad, kDegToRad};
    for (int k = 0; k < 6; ++k) {
        const std::string& s = col(kSigmaCols[k]);
        if (!s.empty()) {
            r.sigmas[k] = parse_double_or(s, -1.0) * kSigmaScale[k];
            if (r.sigmas[k] < 0)
                r.sigmas[k] = 0.0; // treat missing as zero, not negative
            have_sigma = true;
        } else {
            r.sigmas[k] = 0.0;
        }
    }
    if (have_sigma)
        r.flags |= catalog::RecordFlags::kSigmas;

    const std::string& h_s = col("H");
    const std::string& g_s = col("G");
    if (!h_s.empty() || !g_s.empty()) {
        r.h_mag = float(parse_double_or(h_s, 0.0));
        r.g_slope = float(parse_double_or(g_s, 0.15));
        r.flags |= catalog::RecordFlags::kHg;
    }
    const std::string& dia_s = col("diameter");
    if (!dia_s.empty()) {
        const double d = parse_double_or(dia_s, 0.0);
        if (d > 0) {
            r.diameter_km = float(d);
            r.flags |= catalog::RecordFlags::kDiameter;
        }
    }

    // Covariance overlay columns (sbdb_fetch.py --covariance): JPL's full
    // orbit covariance at its own epoch in cometary elements, angles in
    // degrees as delivered; stored with angles in radians.
    const std::string& cov_epoch_s = col("cov_epoch");
    if (!cov_epoch_s.empty()) {
        static const char* kCovElementCols[6] = {"cov_e",  "cov_q", "cov_tp",
                                                 "cov_om", "cov_w", "cov_i"};
        static constexpr double kScale[6] = {1.0, 1.0, 1.0, kDegToRad, kDegToRad, kDegToRad};
        r.cov_epoch_jtdb = parse_double_or(cov_epoch_s, NAN);
        for (int k = 0; k < 6; ++k)
            r.cov_elements[k] = parse_double_or(col(kCovElementCols[k]), NAN) * kScale[k];
        for (int i = 0; i < 6; ++i) {
            for (int j = i; j < 6; ++j) {
                const std::string name = "cov_" + std::to_string(i) + std::to_string(j);
                r.covariance[catalog::packed_index(i, j)] =
                    parse_double_or(col(name.c_str()), NAN) * kScale[i] * kScale[j];
            }
        }
        r.flags |= catalog::RecordFlags::kCovariance;
    }

    out.pdes = col("pdes");
    out.name = col("name");
    out.record = r;
    out.ok = true;
    return out;
}

std::map<std::string, std::string> read_provenance(const std::filesystem::path& dir) {
    std::map<std::string, std::string> kv;
    std::ifstream in(dir / "provenance.txt");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <sbdb-raw-dir> -o <out.epm> [--chunk N] [--level N] "
                 "[--no-compress]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string dir, out_path;
    catalog::WriterOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--chunk" && i + 1 < argc) {
            opts.chunk_records = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--level" && i + 1 < argc) {
            opts.zstd_level = int(std::strtol(argv[++i], nullptr, 10));
        } else if (arg == "--no-compress") {
            opts.compress = false;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (dir.empty()) {
            dir = arg;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (dir.empty() || out_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    const auto prov = read_provenance(dir);

    std::vector<std::filesystem::path> shards;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tsv") {
            shards.push_back(entry.path());
        }
    }
    std::sort(shards.begin(), shards.end());
    if (shards.empty()) {
        std::fprintf(stderr, "no .tsv shards in %s\n", dir.c_str());
        return 1;
    }

    std::vector<catalog::Record> records;
    records.reserve(1'600'000);
    std::vector<std::string> pdes_all, name_all;
    pdes_all.reserve(1'600'000);
    name_all.reserve(1'600'000);
    uint64_t skipped = 0, lines = 0;
    bool first_shard = true;

    for (const auto& shard : shards) {
        std::ifstream in(shard);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", shard.c_str());
            return 1;
        }
        std::string line;
        if (!std::getline(in, line)) {
            std::fprintf(stderr, "empty shard %s\n", shard.c_str());
            return 1;
        }
        auto fm = FieldMap::parse(line);
        if (!fm) {
            std::fprintf(stderr, "bad header in %s\n", shard.c_str());
            return 1;
        }
        if (first_shard) {
            std::printf("fields (%zu):", fm->names.size());
            for (const auto& n : fm->names)
                std::printf(" %s", n.c_str());
            std::printf("\n");
            first_shard = false;
        }
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            ++lines;
            std::vector<std::string> cols;
            size_t pos = 0;
            while (pos <= line.size()) {
                size_t next = line.find('\t', pos);
                if (next == std::string::npos)
                    next = line.size();
                cols.push_back(line.substr(pos, next - pos));
                pos = next + 1;
            }
            ParseOutcome p = parse_row(cols, *fm, lines);
            if (p.skipped) {
                ++skipped;
                continue;
            }
            if (p.ok) {
                records.push_back(p.record);
                pdes_all.push_back(std::move(p.pdes));
                name_all.push_back(std::move(p.name));
            }
        }
    }
    std::printf("parsed %llu rows, %llu skipped, %llu kept\n", (unsigned long long)lines,
                (unsigned long long)skipped, (unsigned long long)records.size());

    std::vector<uint64_t> order(records.size());
    for (uint64_t i = 0; i < records.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](uint64_t a, uint64_t b) { return records[a].spkid < records[b].spkid; });

    uint64_t dupes = 0;
    auto w = catalog::Writer::create(out_path, opts);
    if (!w.ok()) {
        std::fprintf(stderr, "writer: %s\n", w.error().message.c_str());
        return 1;
    }
    catalog::Writer& writer = w.value();
    for (uint64_t k = 0; k < order.size(); ++k) {
        const uint64_t i = order[k];
        if (k > 0 && records[i].spkid == records[order[k - 1]].spkid) {
            ++dupes;
            continue; // keep first occurrence
        }
        auto e = writer.add(records[i], pdes_all[i], name_all[i]);
        if (!e.ok()) {
            std::fprintf(stderr, "add spkid %llu failed: %s\n",
                         (unsigned long long)records[i].spkid, e.error().message.c_str());
            return 1;
        }
    }

    CborValue meta = CborValue::make_map();
    auto put_kv = [&meta](const char* k, CborValue v) {
        meta.items.push_back(CborValue::make_text(k));
        meta.items.push_back(std::move(v));
    };
    put_kv("format", CborValue::make_text("EPM1"));
    put_kv("generator",
           CborValue::make_text(std::string("prometheia-convert ") + prometheia::version_string));
    put_kv("frame", CborValue::make_text("ICRF-equinox-J2000"));
    put_kv("time_scale", CborValue::make_text("TDB"));
    put_kv("elements", CborValue::make_text("a,e,i,om,w,ma (AU,rad; angles from SBDB deg)"));
    put_kv("source",
           CborValue::make_text(prov.count("source") ? prov.at("source") : "JPL SBDB Query API"));
    put_kv("source_url", CborValue::make_text(prov.count("url") ? prov.at("url") : ""));
    put_kv("fetched_started_utc",
           CborValue::make_text(prov.count("started_utc") ? prov.at("started_utc") : ""));
    put_kv("fetched_finished_utc",
           CborValue::make_text(prov.count("finished_utc") ? prov.at("finished_utc") : ""));
    put_kv("sbdb_count", CborValue::make_unsigned(std::strtoull(
                             prov.count("count") ? prov.at("count").c_str() : "0", nullptr, 10)));
    put_kv("rows_skipped", CborValue::make_unsigned(skipped));
    put_kv("spkid_dupes", CborValue::make_unsigned(dupes));

    if (auto e = writer.finish(std::move(meta)); !e.ok()) {
        std::fprintf(stderr, "finish: %s\n", e.error().message.c_str());
        return 1;
    }

    const auto bytes = std::filesystem::file_size(out_path);
    std::printf("wrote %s: %llu bodies, %llu dupes dropped, %llu bytes (%.1f B/body)\n",
                out_path.c_str(), (unsigned long long)writer.record_count(),
                (unsigned long long)dupes, (unsigned long long)bytes,
                writer.record_count() ? double(bytes) / double(writer.record_count()) : 0.0);
    return 0;
}
