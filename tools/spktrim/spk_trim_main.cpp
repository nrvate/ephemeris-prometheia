// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-spk-trim — cut a NAIF SPK kernel (type 2/3 segments) down to a
// time span, copying the covering records verbatim. Used to ship JPL's
// asteroid perturber kernel (sb441-n16.bsp, 616 MB over 17,000 years) for
// the DE440 span only; states inside the span are bit-identical.
//
// Usage: prometheia-spk-trim <in.bsp> <out.bsp> --from <JD TDB> --to <JD TDB>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <prometheia/prometheia.hpp>
#include <prometheia/spk.hpp>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <in.bsp> <out.bsp> --from <JD TDB> --to <JD TDB>\n", argv0);
}

bool parse_jd(const char* s, double& out) {
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end != s && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::string in, out;
    double jd0 = 0.0, jd1 = 0.0;
    bool have0 = false, have1 = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from" && i + 1 < argc) {
            have0 = parse_jd(argv[++i], jd0);
        } else if (a == "--to" && i + 1 < argc) {
            have1 = parse_jd(argv[++i], jd1);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (in.empty()) {
            in = a;
        } else if (out.empty()) {
            out = a;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (in.empty() || out.empty() || !have0 || !have1 || !(jd1 > jd0)) {
        usage(argv[0]);
        return 2;
    }

    auto file = prometheia::spk::SpkFile::open(in);
    if (!file.ok()) {
        std::fprintf(stderr, "%s\n", file.error().message.c_str());
        return 1;
    }
    auto original = file.value().comments();
    if (!original.ok()) {
        std::fprintf(stderr, "%s\n", original.error().message.c_str());
        return 1;
    }
    const double et0 = prometheia::spk::SpkFile::et_from_jd(jd0);
    const double et1 = prometheia::spk::SpkFile::et_from_jd(jd1);
    auto cut = prometheia::spk::trim_segments(file.value(), et0, et1);
    if (!cut.ok()) {
        std::fprintf(stderr, "%s\n", cut.error().message.c_str());
        return 1;
    }
    if (cut.value().empty()) {
        std::fprintf(stderr, "no segment of %s overlaps JD %.1f .. %.1f\n", in.c_str(), jd0, jd1);
        return 1;
    }

    char span[160];
    std::snprintf(span, sizeof span, "JD %.1f .. %.1f TDB", jd0, jd1);
    const std::string name = std::filesystem::path(in).filename().string();
    std::string comments = "Trimmed by prometheia-spk-trim " +
                           std::string(prometheia::version_string) + " from " + name + " to " +
                           span + " (whole records, copied verbatim).\n";
    if (!original.value().empty())
        comments += "Original comments follow.\n\n" + original.value();
    std::string internal = file.value().internal_name();
    if (auto r = prometheia::spk::write_spk(out, internal, comments, cut.value()); !r.ok()) {
        std::fprintf(stderr, "%s\n", r.error().message.c_str());
        return 1;
    }
    std::printf("%s: %zu of %zu segments, %.1f MB -> %s (%.1f MB)\n", name.c_str(),
                cut.value().size(), file.value().segments().size(),
                double(std::filesystem::file_size(in)) / 1e6, out.c_str(),
                double(std::filesystem::file_size(out)) / 1e6);
    return 0;
}
