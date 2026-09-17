// SPDX-License-Identifier: GPL-2.0-or-later
//
// A synthetic SPK kernel whose bodies move linearly: every engine answer
// on it is exact. Shared by the engine and catalog-overlay tests.
#ifndef PROMETHEIA_TESTS_SYNTHETIC_SPK_HPP
#define PROMETHEIA_TESTS_SYNTHETIC_SPK_HPP

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <prometheia/engine.hpp>

using namespace prometheia;

namespace synth {

namespace fs = std::filesystem;

constexpr double kJ2000 = 2451545.0;

// ---------------------------------------------------------------------------
// Part A2: synthetic SPK kernel with linear motion.
// ---------------------------------------------------------------------------

constexpr double kDay = 86400.0;

struct LinearBody {
    int id;
    double p[3];    // km at et = 0
    double v[3];    // km/s
    int center = 0; // NAIF id of the segment's center
};

// Sun, Earth and Jupiter-system barycentre, all wrt the barycentre.
const LinearBody kBodies[] = {
    {10, {1.0e5, -2.0e5, 3.0e4}, {0.01, 0.012, -0.002}},
    {399, {2.6e7, 1.44e8, 6.2e7}, {-29.7, 5.0, 2.2}},
    {5, {7.4e8, 1.2e8, 3.0e7}, {-2.3, 12.3, 5.2}},
};
constexpr double kHalfSpan = 60.0 * 365.25 * kDay; // one record, +/- 60 yr

void linear_state(const LinearBody& b, double et, double out[6]) {
    for (int i = 0; i < 3; ++i) {
        out[i] = b.p[i] + b.v[i] * et;
        out[3 + i] = b.v[i] * kDay;
    }
}

struct Writer {
    std::string out;
    void f64(double v) { out.append(reinterpret_cast<const char*>(&v), 8); }
    void i32(int32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
    void text(std::string s, size_t n) {
        s.resize(n, ' ');
        out += s;
    }
    void pad_to(size_t n) { out.resize(n, '\0'); }
};

std::string write_linear_spk(const fs::path& path, const std::vector<LinearBody>& bodies) {
    constexpr int kFirstWord = 3 * 1024 / 8 + 1;
    Writer data;
    std::vector<std::pair<int, int>> addr;
    for (const LinearBody& b : bodies) {
        const int begin = kFirstWord + int(data.out.size() / 8);
        // One type-3 record, degree 1: x(tau) = c0 + c1 tau, tau = et / R.
        data.f64(0.0);       // MID
        data.f64(kHalfSpan); // RADIUS
        for (int c = 0; c < 3; ++c) {
            data.f64(b.p[c]);
            data.f64(b.v[c] * kHalfSpan);
        }
        for (int c = 0; c < 3; ++c) {
            data.f64(b.v[c]);
            data.f64(0.0);
        }
        data.f64(-kHalfSpan);      // INIT
        data.f64(2.0 * kHalfSpan); // INTLEN
        data.f64(2.0 + 6 * 2);     // RSIZE
        data.f64(1.0);             // N
        addr.push_back({begin, kFirstWord + int(data.out.size() / 8) - 1});
    }
    Writer f;
    f.text("DAF/SPK ", 8);
    f.i32(2);
    f.i32(6);
    f.text("SYNTHETIC LINEAR", 60);
    f.i32(2);
    f.i32(2);
    f.i32(kFirstWord + int(data.out.size() / 8));
    f.text("LTL-IEEE", 8);
    f.pad_to(699);
    const char ftp[] = "FTPSTR:\r:\n:\r\n:\r\0:\x81:\x10\xce:ENDFTP";
    f.out.append(ftp, sizeof(ftp) - 1);
    f.pad_to(1024);
    // Summary record.
    f.f64(0.0);
    f.f64(0.0);
    f.f64(double(bodies.size()));
    for (size_t i = 0; i < bodies.size(); ++i) {
        f.f64(-kHalfSpan);
        f.f64(kHalfSpan);
        f.i32(bodies[i].id);
        f.i32(bodies[i].center);
        f.i32(1);
        f.i32(3);
        f.i32(addr[i].first);
        f.i32(addr[i].second);
    }
    f.pad_to(2048);
    for (const LinearBody& b : bodies)
        f.text("BODY " + std::to_string(b.id), 40);
    f.pad_to(3072);
    f.out += data.out;
    f.pad_to((f.out.size() + 1023) / 1024 * 1024);
    FILE* fp = std::fopen(path.c_str(), "wb");
    std::fwrite(f.out.data(), 1, f.out.size(), fp);
    std::fclose(fp);
    return path.string();
}

std::string write_linear_spk(const fs::path& path) {
    return write_linear_spk(path, std::vector<LinearBody>(std::begin(kBodies), std::end(kBodies)));
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* stem) {
        path = fs::temp_directory_path() /
               (std::string("prometheia-") + stem + "-" + std::to_string(::getpid()) + ".bsp");
    }
    ~TempFile() { fs::remove(path); }
};

Engine open_synthetic(const TempFile& tf) {
    write_linear_spk(tf.path);
    auto e = Engine::open(tf.path.string());
    if (!e)
        std::printf("  open: %s\n", e.error().message.c_str());
    return std::move(e).value();
}

double et_of_tdb(double jd_tdb) {
    return (jd_tdb - kJ2000) * kDay;
}

} // namespace synth

#endif // PROMETHEIA_TESTS_SYNTHETIC_SPK_HPP
