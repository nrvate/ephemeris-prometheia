// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-catalog-bench — the whole-catalog cost measurement (M5): every
// body of an EPM1 catalog answers one engine query `--days` after its own
// element epoch (geocentric astrometric, no rates, no sigma), which
// integrates its trajectory across that span with the full force model.
// Bodies are split over `--threads` workers, one Engine each (engines are
// not shared across threads); each worker releases its memoized
// trajectories every `--chunk` bodies (Engine::release_small_bodies) so
// memory stays bounded.
//
// Usage: prometheia-catalog-bench <ephemeris> <catalog.epm> [--threads N]
//        [--days D] [--limit N] [--chunk N] [--perturbers FILE]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <prometheia/catalog.hpp>
#include <prometheia/engine.hpp>

using namespace prometheia;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <ephemeris> <catalog.epm> [--threads N] [--days D] [--limit N] "
                     "[--chunk N] [--perturbers FILE]\n",
                     argv[0]);
        return 2;
    }
    const std::string ephemeris = argv[1], catalog_path = argv[2];
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    double days = 365.25;
    size_t limit = 0, chunk = 1000;
    std::string perturbers;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--threads")
            threads = unsigned(std::strtoul(argv[i + 1], nullptr, 10));
        else if (a == "--days")
            days = std::strtod(argv[i + 1], nullptr);
        else if (a == "--limit")
            limit = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--chunk")
            chunk = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--perturbers")
            perturbers = argv[i + 1];
    }

    struct Job {
        int spkid;
        double epoch;
    };
    std::vector<Job> jobs;
    {
        auto reader = catalog::Reader::open(catalog_path);
        if (!reader.ok()) {
            std::fprintf(stderr, "%s\n", reader.error().message.c_str());
            return 1;
        }
        auto fe = reader.value().for_each([&](const catalog::Record& r, const catalog::Names&) {
            if (limit == 0 || jobs.size() < limit)
                jobs.push_back({int(r.spkid), r.epoch_jtdb});
        });
        if (!fe.ok()) {
            std::fprintf(stderr, "%s\n", fe.error().message.c_str());
            return 1;
        }
    }
    std::printf("%zu bodies, %u threads, query at element epoch + %.2f d%s\n", jobs.size(), threads,
                days, perturbers.empty() ? "" : ", with asteroid perturbers");

    std::atomic<size_t> next{0}, done{0}, failed{0};
    std::atomic<long long> setup_ns{0}, setups{0};
    const auto t0 = std::chrono::steady_clock::now();
    auto worker = [&]() {
        std::unique_ptr<Engine> e;
        size_t in_engine = 0;
        CalcOptions o = CalcOptions::astrometric();
        o.speed = false;
        o.sigma = false;
        for (;;) {
            const size_t k = next.fetch_add(1);
            if (k >= jobs.size())
                return;
            if (in_engine >= chunk) {
                e->release_small_bodies();
                in_engine = 0;
            }
            if (!e) {
                const auto s0 = std::chrono::steady_clock::now();
                auto opened = Engine::open(ephemeris);
                if (!opened.ok() || !opened.value().add_catalog(catalog_path).ok() ||
                    (!perturbers.empty() && !opened.value().add_perturbers(perturbers).ok())) {
                    std::fprintf(stderr, "engine setup failed\n");
                    std::exit(1);
                }
                e = std::make_unique<Engine>(std::move(opened).value());
                in_engine = 0;
                setup_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - s0)
                                .count();
                ++setups;
            }
            ++in_engine;
            // TT ~ TDB to the millisecond: irrelevant for a cost measurement.
            if (!e->calc(jobs[k].spkid, jobs[k].epoch + days, o).ok())
                ++failed;
            const size_t d = ++done;
            if (d % 100000 == 0) {
                const double s =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                long pages = 0, resident = 0;
                if (FILE* f = std::fopen("/proc/self/statm", "r")) {
                    if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
                        resident = 0;
                    std::fclose(f);
                }
                std::printf("  %zu bodies, %.0f s, RSS %.0f MB\n", d, s,
                            double(resident) * 4096.0 / 1048576.0);
                std::fflush(stdout);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = double(ru.ru_utime.tv_sec) + 1e-6 * double(ru.ru_utime.tv_usec) +
                       double(ru.ru_stime.tv_sec) + 1e-6 * double(ru.ru_stime.tv_usec);
    const double setup_s = double(setup_ns.load()) * 1e-9;
    std::printf("done: %zu bodies (%zu failed), wall %.1f s, cpu %.1f s; engine setup %lld x "
                "%.2f s (open + add_catalog%s); integration + query %.3f ms/body/core; "
                "peak RSS %.0f MB\n",
                jobs.size(), size_t(failed), wall, cpu, setups.load(),
                setups ? setup_s / double(setups) : 0.0,
                perturbers.empty() ? "" : " + add_perturbers",
                1e3 * (cpu - setup_s) / double(jobs.size()), double(ru.ru_maxrss) / 1024.0);
    return 0;
}
