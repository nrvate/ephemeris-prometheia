// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C dataset API (prometheia_dataset_*, prometheia_context_open): its
// failure states, its lifetime, and six contexts on six threads asking the
// same questions at once, each answer compared bit for bit with one engine
// opened the ordinary way. Needs DE440 (PROMETHEIA_DE440, else
// ephe/linux_p1550p2650.440) and the DE440-span perturber kernel
// (PROMETHEIA_SB441_DE440SPAN, else ephe/sb441-n16-de440span.bsp): SKIP when
// either is absent.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <prometheia/prometheia.h>

#include <doctest/doctest.h>

namespace {

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

struct Question {
    enum Kind { Planet, CatalogBody, Sigma, Apsis, Node, Lookup, Bodies, CatalogPath } kind;
    int body = 0;
    double jd_tt = 0.0;
    int point = 0;
    const char* name = nullptr;
};

// Every number an answer carries; a failure contributes its status.
std::vector<double> answer(prometheia_engine* e, const Question& q) {
    std::vector<double> out;
    prometheia_result r;
    prometheia_options o;
    prometheia_options_init(&o);
    prometheia_status s = PROMETHEIA_OK;
    switch (q.kind) {
    case Question::Planet:
        o.sigma = 0;
        s = prometheia_calc(e, q.body, q.jd_tt, &o, &r, nullptr);
        break;
    case Question::CatalogBody:
        o.sigma = 0;
        o.center = PROMETHEIA_CENTER_HELIOCENTRIC;
        s = prometheia_calc(e, q.body, q.jd_tt, &o, &r, nullptr);
        break;
    case Question::Sigma:
        s = prometheia_calc(e, q.body, q.jd_tt, &o, &r, nullptr);
        break;
    case Question::Apsis:
        s = prometheia_calc_orbit_point(e, PROMETHEIA_MOON, q.point,
                                        PROMETHEIA_ELEMENTS_INTERPOLATED, q.jd_tt, &o, &r, nullptr);
        break;
    case Question::Node:
        s = prometheia_calc_orbit_point(e, q.body, q.point, PROMETHEIA_ELEMENTS_OSCULATING, q.jd_tt,
                                        &o, &r, nullptr);
        break;
    case Question::Lookup: {
        int body = 0;
        s = prometheia_engine_lookup(e, q.name, &body, nullptr);
        out.push_back(double(s));
        out.push_back(double(body));
        return out;
    }
    case Question::Bodies: {
        int n = 0;
        s = prometheia_engine_catalog_bodies(e, &n, nullptr);
        out.push_back(double(s));
        out.push_back(double(n));
        return out;
    }
    case Question::CatalogPath: {
        // The path's bytes, read at once: a string shared between contexts
        // could be overwritten by a sibling before it is read.
        int n = 0;
        const char* path = nullptr;
        s = prometheia_engine_catalog_info(e, q.body, &n, &path, nullptr);
        out.push_back(double(s));
        out.push_back(double(n));
        for (const char* c = path; c && *c; ++c)
            out.push_back(double(*c));
        return out;
    }
    }
    out.push_back(double(s));
    if (s != PROMETHEIA_OK)
        return out;
    out.insert(out.end(), {r.lon_deg, r.lat_deg, r.dist_au, r.lon_speed, r.lat_speed, r.dist_speed,
                           r.xyz_au[0], r.xyz_au[1], r.xyz_au[2], r.vel_au_day[0], r.vel_au_day[1],
                           r.vel_au_day[2]});
    out.push_back(double(r.flags));
    out.push_back((r.flags & PROMETHEIA_HAS_SIGMA) ? r.sigma_arcsec : -1.0);
    out.push_back(std::strlen(r.source) > 0 ? double(r.source[0]) : 0.0);
    return out;
}

bool same(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

struct Files {
    std::string de, kernel, sample, cov;
    bool present() const {
        return access(de.c_str(), R_OK) == 0 && access(kernel.c_str(), R_OK) == 0;
    }
};

Files files() {
    const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
    return {env_or("PROMETHEIA_DE440",
                   std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/linux_p1550p2650.440"),
            env_or("PROMETHEIA_SB441_DE440SPAN",
                   std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/sb441-n16-de440span.bsp"),
            data + "sample-100.epm", data + "covariance-7.epm"};
}

} // namespace

TEST_CASE("c_dataset_failures_leave_it_usable") {
    const Files f = files();
    if (!f.present()) {
        MESSAGE("SKIP: no DE440 or perturber kernel at " << f.de << ", " << f.kernel);
        return;
    }
    prometheia_error err;

    // A failed open sets *out to NULL.
    prometheia_dataset* bad = reinterpret_cast<prometheia_dataset*>(&err);
    CHECK(prometheia_dataset_open("/nonexistent/de.440", &bad, &err) == PROMETHEIA_ERROR_IO);
    CHECK(bad == nullptr);
    CHECK(prometheia_dataset_open(nullptr, &bad, &err) == PROMETHEIA_ERROR_ARGUMENT);

    // NULL is an argument error everywhere but close.
    CHECK(prometheia_dataset_freeze(nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_dataset_add_catalog(nullptr, f.sample.c_str(), &err) ==
          PROMETHEIA_ERROR_ARGUMENT);
    prometheia_engine* ctx = reinterpret_cast<prometheia_engine*>(&err);
    CHECK(prometheia_context_open(nullptr, &ctx, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(ctx == nullptr);
    prometheia_dataset_close(nullptr);

    prometheia_dataset* ds = nullptr;
    REQUIRE(prometheia_dataset_open(f.de.c_str(), &ds, &err) == PROMETHEIA_OK);

    // Refused files leave the dataset as it was, and usable.
    CHECK(prometheia_dataset_add_catalog(ds, "/nonexistent.epm", &err) == PROMETHEIA_ERROR_IO);
    CHECK(prometheia_dataset_add_perturbers(ds, f.sample.c_str(), &err) != PROMETHEIA_OK);
    CHECK(prometheia_dataset_add_ephemeris(ds, f.sample.c_str(), &err) != PROMETHEIA_OK);
    CHECK(prometheia_dataset_add_catalog(ds, nullptr, &err) == PROMETHEIA_ERROR_ARGUMENT);
    REQUIRE(prometheia_dataset_add_catalog(ds, f.sample.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_add_perturbers(ds, f.kernel.c_str(), &err) == PROMETHEIA_OK);

    // Not frozen: no context.
    ctx = reinterpret_cast<prometheia_engine*>(&err);
    CHECK(prometheia_context_open(ds, &ctx, &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(ctx == nullptr);

    REQUIRE(prometheia_dataset_freeze(ds, &err) == PROMETHEIA_OK);
    CHECK(prometheia_dataset_freeze(ds, &err) == PROMETHEIA_OK); // twice is fine
    CHECK(prometheia_dataset_add_catalog(ds, f.cov.c_str(), &err) == PROMETHEIA_ERROR_ARGUMENT);
    REQUIRE(prometheia_context_open(ds, &ctx, &err) == PROMETHEIA_OK);
    CHECK(prometheia_engine_add_catalog(ctx, f.cov.c_str(), &err) == PROMETHEIA_ERROR_ARGUMENT);
    CHECK(prometheia_engine_add_hypotheticals(ctx, f.cov.c_str(), &err) ==
          PROMETHEIA_ERROR_ARGUMENT);

    // The same answers as an engine given the same files, whose refusals were
    // never made: the refused files left no trace.
    prometheia_engine* eng = nullptr;
    REQUIRE(prometheia_engine_open(f.de.c_str(), &eng, &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_engine_add_catalog(eng, f.sample.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_engine_add_perturbers(eng, f.kernel.c_str(), &err) == PROMETHEIA_OK);
    for (double jd : {2440000.5, 2460000.5, 2465000.5}) {
        Question q{Question::CatalogBody, 20000001, jd};
        CHECK(same(answer(ctx, q), answer(eng, q)));
        q = {Question::Planet, PROMETHEIA_MARS, jd};
        CHECK(same(answer(ctx, q), answer(eng, q)));
    }

    // Closing the handle leaves the context working.
    prometheia_dataset_close(ds);
    Question q{Question::CatalogBody, 20000002, 2461000.5};
    CHECK(same(answer(ctx, q), answer(eng, q)));
    q = {Question::Lookup, 0, 0.0, 0, "Ceres"};
    CHECK(same(answer(ctx, q), answer(eng, q)));
    prometheia_engine_close(ctx);
    prometheia_engine_close(eng);
}

TEST_CASE("c_dataset_contexts_on_threads_answer_as_one_engine") {
    const Files f = files();
    if (!f.present()) {
        MESSAGE("SKIP: no DE440 or perturber kernel at " << f.de << ", " << f.kernel);
        return;
    }
    prometheia_error err;
    prometheia_dataset* ds = nullptr;
    REQUIRE(prometheia_dataset_open(f.de.c_str(), &ds, &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_add_catalog(ds, f.sample.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_add_catalog(ds, f.cov.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_add_perturbers(ds, f.kernel.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_dataset_freeze(ds, &err) == PROMETHEIA_OK);

    // The reference: an ordinary engine with the same files.
    prometheia_engine* ref = nullptr;
    REQUIRE(prometheia_engine_open(f.de.c_str(), &ref, &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_engine_add_catalog(ref, f.sample.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_engine_add_catalog(ref, f.cov.c_str(), &err) == PROMETHEIA_OK);
    REQUIRE(prometheia_engine_add_perturbers(ref, f.kernel.c_str(), &err) == PROMETHEIA_OK);

    // The questions, seeded. Sigma bodies: the covariance catalog's.
    std::mt19937_64 rng(20261028);
    const auto uniform = [&](double a, double b) {
        return std::uniform_real_distribution<double>(a, b)(rng);
    };
    const auto pick = [&](size_t n) { return int(rng() % n); };
    const int with_cov[] = {20000001, 20000002, 20000004, 20000007, 20000010, 20000065, 20145451};
    const char* names[] = {"Ceres", "pallas", "JUNO", "Vesta", "4", "no such body"};
    std::vector<Question> qs;
    for (int i = 0; i < 240; ++i) {
        Question q;
        q.kind = Question::Kind(pick(6));
        q.jd_tt = uniform(2433282.5, 2469807.5); // 1950-2050
        switch (q.kind) {
        case Question::Planet:
            q.body = pick(11);
            break;
        case Question::CatalogBody:
            q.body = 20000001 + pick(100);
            q.jd_tt = uniform(2433282.5, 2469807.5); // 1950-2050: integrations over decades
            break;
        case Question::Sigma:
            q.body = with_cov[pick(std::size(with_cov))];
            q.jd_tt = uniform(2459580.5, 2462502.5);
            break;
        case Question::Apsis:
            q.point = PROMETHEIA_ORBIT_PERIHELION + pick(2);
            break;
        case Question::Node:
            q.body = pick(2) ? PROMETHEIA_JUPITER : 20000001 + pick(100);
            q.point = pick(4);
            break;
        default:
            q.kind = Question::Lookup;
            q.name = names[pick(std::size(names))];
            break;
        }
        qs.push_back(q);
    }
    std::vector<std::vector<double>> expected(qs.size());
    for (size_t i = 0; i < qs.size(); ++i)
        expected[i] = answer(ref, qs[i]);
    const Question bodies{Question::Bodies};
    const std::vector<double> expected_bodies = answer(ref, bodies);
    std::vector<double> expected_paths[2];
    for (int c = 0; c < 2; ++c)
        expected_paths[c] = answer(ref, Question{Question::CatalogPath, c});

    // Six contexts, each asking every question, in its own order, all at
    // once. They start together on the shared caches built on first use:
    // the catalogs' body count and name indexes.
    constexpr int kThreads = 6;
    std::vector<prometheia_engine*> ctx(kThreads, nullptr);
    for (auto& c : ctx)
        REQUIRE(prometheia_context_open(ds, &c, &err) == PROMETHEIA_OK);
    prometheia_dataset_close(ds); // the contexts keep the files
    std::atomic<int> ready{0};
    std::vector<int> differ(kThreads, 0), first_bad(kThreads, -1);
    std::vector<std::thread> threads;
    for (int k = 0; k < kThreads; ++k) {
        threads.emplace_back([&, k] {
            ready.fetch_add(1);
            while (ready.load() < kThreads) {
            }
            if (!same(answer(ctx[size_t(k)], bodies), expected_bodies))
                ++differ[size_t(k)];
            std::vector<size_t> order(qs.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::shuffle(order.begin(), order.end(), std::mt19937_64(uint64_t(k) + 1));
            for (size_t i : order) {
                if (!same(answer(ctx[size_t(k)], qs[i]), expected[i])) {
                    if (differ[size_t(k)]++ == 0)
                        first_bad[size_t(k)] = int(i);
                }
                if (i % 50 == 0)
                    for (int c = 0; c < 2; ++c)
                        if (!same(answer(ctx[size_t(k)], Question{Question::CatalogPath, c}),
                                  expected_paths[c]))
                            ++differ[size_t(k)];
            }
        });
    }
    for (std::thread& t : threads)
        t.join();
    int refused = 0;
    for (const auto& e : expected)
        refused += e[0] != double(PROMETHEIA_OK);
    MESSAGE(qs.size() << " questions on " << kThreads << " threads, " << refused
                      << " refused (the same in all)");
    for (int k = 0; k < kThreads; ++k) {
        INFO("thread " << k << ", first differing question " << first_bad[size_t(k)]);
        CHECK(differ[size_t(k)] == 0);
    }
    CHECK(refused < int(qs.size()) / 4);
    for (auto* c : ctx)
        prometheia_engine_close(c);
    prometheia_engine_close(ref);
}
