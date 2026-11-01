// SPDX-License-Identifier: GPL-2.0-or-later
//
// One frozen Corpus, six engines on six threads (Engine::open_context): a
// randomized mix of every kind of question, each answer compared bit for bit
// with one engine's answers to the same questions asked in the reverse order.
// Needs DE440 (PROMETHEIA_DE440, else ephe/linux_p1550p2650.440): SKIP when
// absent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <prometheia/engine.hpp>
#include <prometheia/stars.hpp>

#include <doctest/doctest.h>

using namespace prometheia;

namespace {

constexpr double kRadPerDeg = 3.14159265358979323846 / 180.0;

std::string env_or(const char* var, const std::string& fallback) {
    const char* env = std::getenv(var);
    return (env && *env) ? std::string(env) : fallback;
}

struct Task {
    enum Kind { Planet, CatalogBody, Sigma, OrbitPoint, Star, House, Lookup } kind;
    int id = 0;
    double jd_tt = 0.0;
    Engine::ObjectRef ref;
    std::string name;
    double lat_deg = 0.0, lon_deg = 0.0;
};

// Every number an answer carries, so that two answers compare as bytes. An
// error contributes its code.
std::vector<double> answer(Engine& e, const Task& t) {
    std::vector<double> out;
    const auto put_calc = [&](const Result<CalcResult>& r) {
        if (!r.ok()) {
            out.push_back(-1.0 - double(int(r.error().code)));
            return;
        }
        const Position& p = r.value().pos;
        out.insert(out.end(), {p.lon_deg, p.lat_deg, p.dist_au, p.lon_speed, p.lat_speed,
                               p.dist_speed, p.xyz_au[0], p.xyz_au[1], p.xyz_au[2], p.vel_au_day[0],
                               p.vel_au_day[1], p.vel_au_day[2]});
        out.push_back(r.value().sigma_arcsec.value_or(-1.0));
        out.push_back(r.value().provenance.light_time_days);
    };
    CalcOptions o;
    switch (t.kind) {
    case Task::Planet:
        o.sigma = false;
        put_calc(e.calc(t.id, t.jd_tt, o));
        break;
    case Task::CatalogBody:
        o.sigma = false;
        o.center = Center::Heliocentric;
        put_calc(e.calc(t.id, t.jd_tt, o));
        break;
    case Task::Sigma:
        put_calc(e.calc(t.id, t.jd_tt, o));
        break;
    case Task::OrbitPoint:
    case Task::Star:
        put_calc(e.calc_object(t.ref, t.jd_tt, o));
        break;
    case Task::House: {
        auto h = e.houses(houses::System::Placidus, t.jd_tt,
                          frames::GeoSite{t.lon_deg * kRadPerDeg, t.lat_deg * kRadPerDeg, 0.0});
        if (!h.ok()) {
            out.push_back(-1.0 - double(int(h.error().code)));
            break;
        }
        for (double c : h.value().houses.cusp_deg)
            out.push_back(c);
        out.push_back(h.value().houses.angles.asc_deg);
        out.push_back(h.value().houses.angles.mc_deg);
        break;
    }
    case Task::Lookup: {
        auto r = e.lookup(t.name);
        out.push_back(r.ok() ? double(r.value()) : -1.0 - double(int(r.error().code)));
        break;
    }
    }
    return out;
}

} // namespace

TEST_CASE("engine_threads_share_one_frozen_corpus") {
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de);
        return;
    }
    const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
    auto opened = Corpus::open(de);
    REQUIRE(opened.ok());
    std::shared_ptr<Corpus> corpus = opened.value();
    REQUIRE(corpus->add_catalog(data + "sample-100.epm").ok());
    REQUIRE(corpus->add_catalog(data + "covariance-7.epm").ok());

    // Before freezing, no engine may share it; after, no file may be added.
    CHECK(Engine::open_context(corpus).error().code == ErrorCode::ArgumentError);
    corpus->freeze();
    CHECK(corpus->add_catalog(data + "sample-100.epm").error().code == ErrorCode::ArgumentError);

    // The bodies with a covariance (sigma), from the second catalog.
    auto reference = Engine::open_context(corpus);
    REQUIRE(reference.ok());
    Engine& ref = reference.value();
    CHECK(ref.add_catalog(data + "sample-100.epm").error().code == ErrorCode::ArgumentError);
    std::vector<int> with_cov;
    REQUIRE(
        ref.for_each_catalog_record([&](size_t c, const catalog::Record& r, const catalog::Names&) {
               if (c == 1 && r.has(catalog::RecordFlags::kCovariance))
                   with_cov.push_back(int(r.spkid));
           })
            .ok());
    REQUIRE(!with_cov.empty());

    // The questions: seeded, so a failure reproduces.
    std::mt19937_64 rng(20261028);
    const auto uniform = [&](double a, double b) {
        return std::uniform_real_distribution<double>(a, b)(rng);
    };
    const auto pick = [&](size_t n) { return size_t(rng() % n); };
    const char* names[] = {"Ceres", "pallas", "JUNO", "Vesta", "4", "Hygiea", "no such body"};
    std::vector<Task> tasks;
    for (int i = 0; i < 1200; ++i) {
        Task t;
        t.kind = Task::Kind(pick(7));
        t.jd_tt = uniform(2433282.5, 2469807.5); // 1950-2050
        switch (t.kind) {
        case Task::Planet:
            t.id = int(pick(11)); // the barycentre (0, refused geocentrically) to the Sun
            break;
        case Task::CatalogBody:
            t.id = 20000001 + int(pick(100));
            t.jd_tt = uniform(2455197.5, 2466154.5); // 2010-2040: near the elements' epochs
            break;
        case Task::Sigma:
            t.id = with_cov[pick(with_cov.size())];
            t.jd_tt = uniform(2459580.5, 2462502.5); // 2022-2030
            break;
        case Task::OrbitPoint: {
            t.ref.kind = Engine::ObjectRef::Kind::OrbitPoint;
            const int which = int(pick(4));
            t.ref.id = which == 0 ? 301 : which == 1 ? 499 : which == 2 ? 599 : 20000001;
            t.ref.point = OrbitPoint(pick(4));
            t.ref.method = t.ref.id == 301 ? OrbitElements(pick(3))
                                           : OrbitElements(pick(2)); // natural: the Moon's apsides
            if (t.ref.method == OrbitElements::Interpolated &&
                (t.ref.point == OrbitPoint::AscendingNode ||
                 t.ref.point == OrbitPoint::DescendingNode))
                t.ref.method = OrbitElements::Osculating;
            if (t.ref.id == 20000001)
                t.jd_tt = uniform(2455197.5, 2466154.5);
            break;
        }
        case Task::Star:
            t.ref.kind = Engine::ObjectRef::Kind::Star;
            t.ref.id = long(pick(stars::count()));
            break;
        case Task::House:
            t.lat_deg = uniform(-60.0, 60.0);
            t.lon_deg = uniform(-180.0, 180.0);
            break;
        case Task::Lookup:
            t.name = names[pick(std::size(names))];
            break;
        }
        tasks.push_back(t);
    }

    // The reference: one engine, the questions in reverse order.
    std::vector<std::vector<double>> expected(tasks.size());
    for (size_t i = tasks.size(); i-- > 0;)
        expected[i] = answer(ref, tasks[i]);

    // Six engines, each its share in its own shuffled order, all at once.
    constexpr int kThreads = 6;
    std::vector<Engine> engines;
    for (int k = 0; k < kThreads; ++k) {
        auto e = Engine::open_context(corpus);
        REQUIRE(e.ok());
        engines.push_back(std::move(e).value());
    }
    std::vector<std::vector<double>> got(tasks.size());
    std::vector<std::thread> threads;
    for (int k = 0; k < kThreads; ++k) {
        threads.emplace_back([&, k] {
            std::vector<size_t> mine;
            for (size_t i = size_t(k); i < tasks.size(); i += kThreads)
                mine.push_back(i);
            std::shuffle(mine.begin(), mine.end(), std::mt19937_64(uint64_t(k) + 1));
            for (size_t i : mine)
                got[i] = answer(engines[size_t(k)], tasks[i]);
        });
    }
    for (std::thread& t : threads)
        t.join();

    int differ = 0, errors = 0;
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (got[i].size() != expected[i].size() ||
            std::memcmp(got[i].data(), expected[i].data(), got[i].size() * sizeof(double)) != 0) {
            if (++differ <= 5)
                MESSAGE("task " << i << " (kind " << int(tasks[i].kind) << ") differs");
        }
        if (!expected[i].empty() && expected[i][0] < 0.0 && expected[i].size() == 1 &&
            tasks[i].kind != Task::Lookup)
            ++errors;
    }
    MESSAGE(tasks.size() << " questions, " << errors << " refused (the same in both)");
    CHECK(differ == 0);
    CHECK(errors < int(tasks.size()) / 10); // the mix asks mostly answerable questions
}

TEST_CASE("engine_contexts_share_small_body_windows") {
    // A window is a function of the body and the corpus, so the corpus keeps
    // each once: a context that finds one integrates nothing and gets the
    // same doubles, sigma tracks included, in any order and across threads.
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    if (access(de.c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 at " << de);
        return;
    }
    const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
    auto opened = Corpus::open(de);
    REQUIRE(opened.ok());
    std::shared_ptr<Corpus> corpus = opened.value();
    REQUIRE(corpus->add_catalog(data + "covariance-7.epm").ok());
    corpus->freeze();

    auto first_ctx = Engine::open_context(corpus);
    REQUIRE(first_ctx.ok());
    Engine& first = first_ctx.value();
    std::vector<int> bodies;
    REQUIRE(
        first
            .for_each_catalog_record([&](size_t, const catalog::Record& r, const catalog::Names&) {
                bodies.push_back(int(r.spkid));
            })
            .ok());
    REQUIRE(bodies.size() >= 4);
    bodies.resize(4); // 13 tracks each: enough, and quick under ASan

    // Twelve years either side of the 2020s epochs, so several windows of
    // every track in both directions, in a scrambled order.
    std::vector<Task> tasks;
    std::mt19937_64 rng(20261101);
    for (int i = 0; i < 80; ++i) {
        Task t;
        t.kind = Task::Sigma;
        t.id = bodies[size_t(rng() % bodies.size())];
        t.jd_tt = std::uniform_real_distribution<double>(2456000.0, 2465000.0)(rng);
        tasks.push_back(t);
    }

    std::vector<std::vector<double>> want;
    for (const Task& t : tasks)
        want.push_back(answer(first, t));
    CHECK(first.small_body_windows_integrated() > 0);
    const size_t kept = first.small_body_windows_kept();
    CHECK(kept > 0);
    // Every window this context integrated is kept once: 13 tracks each.
    CHECK(uint64_t(kept) == first.small_body_windows_integrated());
    bool sigma_seen = false;
    for (const std::vector<double>& a : want)
        sigma_seen = sigma_seen || (a.size() > 12 && a[12] > 0.0);
    CHECK(sigma_seen);

    // A second context, in the reverse order, integrates nothing.
    auto second_ctx = Engine::open_context(corpus);
    REQUIRE(second_ctx.ok());
    Engine& second = second_ctx.value();
    for (size_t i = tasks.size(); i-- > 0;)
        CHECK(answer(second, tasks[i]) == want[i]);
    CHECK(second.small_body_windows_integrated() == 0);
    CHECK(second.small_body_windows_kept() == kept);

    // Nor after it lets its own go.
    second.release_small_bodies();
    for (size_t i = 0; i < tasks.size(); ++i)
        CHECK(answer(second, tasks[i]) == want[i]);
    CHECK(second.small_body_windows_integrated() == 0);

    // An engine with a corpus of its own, which shares nothing, agrees.
    auto alone = Engine::open(de);
    REQUIRE(alone.ok());
    REQUIRE(alone.value().add_catalog(data + "covariance-7.epm").ok());
    for (size_t i = tasks.size(); i-- > 0;)
        CHECK(answer(alone.value(), tasks[i]) == want[i]);
    CHECK(alone.value().small_body_windows_integrated() == uint64_t(kept));

    // New years, from six threads at once: whichever integrates a window
    // first, each gets the doubles the lone engine gets.
    std::vector<Task> fresh = tasks;
    for (Task& t : fresh)
        t.jd_tt -= 5000.0;
    std::vector<std::vector<double>> lone;
    for (const Task& t : fresh)
        lone.push_back(answer(alone.value(), t));
    std::vector<int> wrong(6, 0);
    std::vector<std::thread> threads;
    for (int k = 0; k < 6; ++k)
        threads.emplace_back([&, k] {
            auto c = Engine::open_context(corpus);
            if (!c.ok()) {
                wrong[size_t(k)] = -1;
                return;
            }
            for (size_t i = 0; i < fresh.size(); ++i) {
                const size_t at = (i * 7 + size_t(k) * 13) % fresh.size();
                if (answer(c.value(), fresh[at]) != lone[at])
                    ++wrong[size_t(k)];
            }
        });
    for (std::thread& t : threads)
        t.join();
    for (int w : wrong)
        CHECK(w == 0);

    // A number finds its body straight, and anything else that is digits
    // answers as it always did.
    CHECK(first.lookup("1").value() == 20000001);
    CHECK(first.lookup("00001").error().code == ErrorCode::NotFound);
    CHECK(first.lookup("0").error().code == ErrorCode::NotFound);
    CHECK(first.lookup("9999999").error().code == ErrorCode::NotFound);
    CHECK(first.lookup("123456789012345678901234567890").error().code == ErrorCode::NotFound);
}

TEST_CASE("engine_files_drop_the_kept_windows") {
    // Each file added can change what a window is: a catalog the elements, a
    // kernel the masses, an ephemeris the span.
    const std::string de = env_or("PROMETHEIA_DE440", std::string(PROMETHEIA_SOURCE_DIR) +
                                                          "/ephe/linux_p1550p2650.440");
    const std::string kernel =
        env_or("PROMETHEIA_SB441_DE440SPAN",
               std::string(PROMETHEIA_SOURCE_DIR) + "/ephe/sb441-n16-de440span.bsp");
    if (access(de.c_str(), R_OK) != 0 || access(kernel.c_str(), R_OK) != 0) {
        MESSAGE("SKIP: no DE440 or perturber kernel");
        return;
    }
    const std::string data = std::string(PROMETHEIA_SOURCE_DIR) + "/tests/data/";
    auto opened = Engine::open(de);
    REQUIRE(opened.ok());
    Engine& e = opened.value();
    REQUIRE(e.add_catalog(data + "sample-100.epm").ok());
    CalcOptions o;
    o.sigma = false;
    const auto ask = [&] {
        REQUIRE(e.calc(20000001, 2456000.5, o).ok());
        CHECK(e.small_body_windows_kept() > 0);
    };
    ask();
    REQUIRE(e.add_catalog(data + "covariance-7.epm").ok());
    CHECK(e.small_body_windows_kept() == 0);
    ask();
    REQUIRE(e.add_perturbers(kernel).ok());
    CHECK(e.small_body_windows_kept() == 0);
    ask();
    REQUIRE(e.add_ephemeris(de).ok());
    CHECK(e.small_body_windows_kept() == 0);
}
