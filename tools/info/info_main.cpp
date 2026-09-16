// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-info — inspect an EPM1 catalog: header, metadata, per-class
// counts, size efficiency, and sample records.
//
// Usage: prometheia-info <catalog.epm> [--sample N]
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>
#include <prometheia/prometheia.hpp>

namespace {

const char* class_name(prometheia::catalog::BodyClass c) {
    switch (c) {
    case prometheia::catalog::BodyClass::Asteroid:
        return "asteroid";
    case prometheia::catalog::BodyClass::Comet:
        return "comet";
    case prometheia::catalog::BodyClass::Planet:
        return "planet";
    default:
        return "other";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    int sample = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sample" && i + 1 < argc) {
            sample = int(std::strtol(argv[++i], nullptr, 10));
        } else if (path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr, "usage: %s <catalog.epm> [--sample N]\n", argv[0]);
            return 2;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s <catalog.epm> [--sample N]\n", argv[0]);
        return 2;
    }

    auto r = prometheia::catalog::Reader::open(path);
    if (!r.ok()) {
        std::fprintf(stderr, "open: %s\n", r.error().message.c_str());
        return 1;
    }
    prometheia::catalog::Reader& reader = r.value();

    const auto bytes = std::filesystem::file_size(path);
    std::printf("file:            %s\n", path.c_str());
    std::printf("size:            %llu bytes\n", (unsigned long long)bytes);
    std::printf("records:         %llu\n", (unsigned long long)reader.record_count());
    std::printf("chunk_records:   %u\n", reader.chunk_records());
    if (reader.record_count() > 0) {
        std::printf("bytes/record:    %.1f (compressed container incl. index, meta, pool)\n",
                    double(bytes) / double(reader.record_count()));
    }

    std::printf("metadata:\n%s\n", prometheia::cbor_to_debug_string(reader.metadata(), 2).c_str());

    uint64_t counts[4] = {0, 0, 0, 0};
    uint64_t with_sigma = 0, with_hg = 0, with_diameter = 0, with_name = 0;
    double epoch_min = 1e18, epoch_max = -1e18;
    uint64_t sampled = 0;
    const auto emit_sample = [&](const prometheia::catalog::Record& rec,
                                 const prometheia::catalog::Names& names) {
        if (sampled >= uint64_t(sample < 0 ? 0 : sample))
            return;
        if (sampled == 0)
            std::printf("sample records:\n");
        ++sampled;
        std::printf("  spkid %llu  pdes %-16s name %-20s class %-8s epoch %.1f\n",
                    (unsigned long long)rec.spkid, std::string(names.pdes).c_str(),
                    std::string(names.name).c_str(), class_name(rec.body_class), rec.epoch_jtdb);
        std::printf("    a=%.8f AU  e=%.8f  i=%.6f rad  node=%.6f  argp=%.6f  M=%.6f\n", rec.a_au,
                    rec.e, rec.inc_rad, rec.node_rad, rec.argp_rad, rec.mean_anom_rad);
        if (rec.has(prometheia::catalog::RecordFlags::kSigmas))
            std::printf("    sigma_a=%.3g sigma_e=%.3g\n", rec.sigmas[0], rec.sigmas[1]);
        if (rec.has(prometheia::catalog::RecordFlags::kHg))
            std::printf("    H=%.2f G=%.2f\n", rec.h_mag, rec.g_slope);
        if (rec.has(prometheia::catalog::RecordFlags::kDiameter))
            std::printf("    diameter=%.1f km\n", rec.diameter_km);
    };

    if (auto e = reader.for_each(
            [&](const prometheia::catalog::Record& rec, const prometheia::catalog::Names& names) {
                const int ci = int(rec.body_class) < 3 ? int(rec.body_class) : 3;
                ++counts[ci];
                if (rec.has(prometheia::catalog::RecordFlags::kSigmas))
                    ++with_sigma;
                if (rec.has(prometheia::catalog::RecordFlags::kHg))
                    ++with_hg;
                if (rec.has(prometheia::catalog::RecordFlags::kDiameter))
                    ++with_diameter;
                if (rec.has(prometheia::catalog::RecordFlags::kHasName))
                    ++with_name;
                epoch_min = std::fmin(epoch_min, rec.epoch_jtdb);
                epoch_max = std::fmax(epoch_max, rec.epoch_jtdb);
                emit_sample(rec, names);
            });
        !e.ok()) {
        std::fprintf(stderr, "scan: %s\n", e.error().message.c_str());
        return 1;
    }

    std::printf("classes:            asteroid=%llu comet=%llu planet=%llu other=%llu\n",
                (unsigned long long)counts[0], (unsigned long long)counts[1],
                (unsigned long long)counts[2], (unsigned long long)counts[3]);
    if (reader.record_count() > 0) {
        std::printf("field coverage:     sigmas=%llu H/G=%llu diameter=%llu name=%llu\n",
                    (unsigned long long)with_sigma, (unsigned long long)with_hg,
                    (unsigned long long)with_diameter, (unsigned long long)with_name);
        std::printf("epoch range (TDB):  %.1f .. %.1f\n", epoch_min, epoch_max);
    }
    const auto& st = reader.stats();
    std::printf("reader stats:       chunks_read=%llu cache_hits=%llu\n",
                (unsigned long long)st.chunks_read, (unsigned long long)st.chunk_cache_hits);
    return 0;
}
