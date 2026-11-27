// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-de-trim — cut a JPL DE binary (DE440, DE441, ...) down to the
// whole 32-day records covering a span, copied verbatim (docs/DE.md,
// "Cutting a span"). Made for shipping part of DE441: its 2.8 GB cover
// 30,000 years, and a client wanting the centuries before 1550 needs a few
// hundred MB of it. States inside the cut are the source's to the bit.
//
// Usage: prometheia-de-trim <in> <out> --from <JD TDB> --to <JD TDB>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <prometheia/de.hpp>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <in> <out> --from <JD TDB> --to <JD TDB>\n", argv0);
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
    auto cut = prometheia::de::trim(in, out, jd0, jd1);
    if (!cut.ok()) {
        std::fprintf(stderr, "%s\n", cut.error().message.c_str());
        return 1;
    }
    std::printf("%s: %llu records, JD %.1f .. %.1f TDB, %.1f MB -> %s (%.1f MB)\n",
                std::filesystem::path(in).filename().string().c_str(),
                static_cast<unsigned long long>(cut.value().records), cut.value().start_jed,
                cut.value().end_jed, double(std::filesystem::file_size(in)) / 1e6, out.c_str(),
                double(std::filesystem::file_size(out)) / 1e6);
    return 0;
}
