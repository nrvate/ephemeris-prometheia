// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <prometheia/catalog.hpp>
#include <prometheia/cbor.hpp>

#include <doctest/doctest.h>

using namespace prometheia;
using catalog::BodyClass;
using CborValue = prometheia::CborValue;
using catalog::Names;
using catalog::Reader;
using catalog::Record;
using catalog::RecordFlags;
using catalog::Writer;
using catalog::WriterOptions;

namespace {

std::string temp_path(const char* tag) {
    static int counter = 0;
    return (std::filesystem::temp_directory_path() /
            ("prometheia_test_" + std::string(tag) + "_" + std::to_string(counter++) + ".epm"))
        .string();
}

// Canonical synthetic asteroid. Callers adjust class/flags for the variants
// each test writes.
Record make_asteroid(uint64_t spkid) {
    Record r;
    r.spkid = spkid;
    r.body_class = BodyClass::Asteroid;
    r.flags = uint8_t(RecordFlags::kSigmas | RecordFlags::kHg | RecordFlags::kDiameter);
    r.epoch_jtdb = 2461200.5;
    r.a_au = 2.0 + double(spkid % 1000) / 1000.0;
    r.e = 0.05 + double(spkid % 50) / 1000.0;
    r.inc_rad = 0.1 + double(spkid % 90) / 100.0;
    r.node_rad = 1.0 + double(spkid % 360) / 57.29577951308232;
    r.argp_rad = 2.0 + double(spkid % 180) / 57.29577951308232;
    r.mean_anom_rad = 3.0 + double(spkid % 350) / 57.29577951308232;
    for (int i = 0; i < 6; ++i)
        r.sigmas[i] = 1e-9 * double(spkid % 100 + 1) * double(i + 1);
    r.h_mag = float(8.0 + spkid % 100) / 10.0f;
    r.g_slope = 0.15f;
    r.diameter_km = float(spkid % 900) + 0.5f;
    return r;
}

// The exact variants test roundtrip_small_mixed writes, mirrored on read-back.
Record expected_for(uint64_t spkid) {
    Record r = make_asteroid(spkid);
    if (spkid == 20000002) {
        r.body_class = BodyClass::Comet;
        r.flags = 0;
    } else if (spkid == 20000003) {
        r.flags = uint8_t(RecordFlags::kSigmas | RecordFlags::kHg);
    } else if (spkid == 20000007) {
        r.body_class = BodyClass::Other;
    }
    if (spkid == 20000001 || spkid == 20000003) {
        r.flags |= RecordFlags::kHasName; // writer derives this for named records
    }
    return r;
}

void expect_records_equal(const Record& a, const Record& b) {
    CHECK(a.spkid == b.spkid);
    CHECK(a.body_class == b.body_class);
    CHECK(a.flags == b.flags);
    CHECK(std::memcmp(&a.epoch_jtdb, &b.epoch_jtdb, sizeof(double)) == 0);
    CHECK(a.a_au == b.a_au);
    CHECK(a.e == b.e);
    CHECK(a.inc_rad == b.inc_rad);
    CHECK(a.node_rad == b.node_rad);
    CHECK(a.argp_rad == b.argp_rad);
    CHECK(a.mean_anom_rad == b.mean_anom_rad);
    if (a.has(RecordFlags::kSigmas)) {
        for (int i = 0; i < 6; ++i)
            CHECK(float(a.sigmas[i]) == float(b.sigmas[i]));
    }
    if (a.has(RecordFlags::kHg)) {
        CHECK(a.h_mag == b.h_mag);
        CHECK(a.g_slope == b.g_slope);
    }
    if (a.has(RecordFlags::kDiameter)) {
        CHECK(a.diameter_km == b.diameter_km);
    }
    // name_offset is a pool position, not part of the compared payload.
}

CborValue make_meta() {
    CborValue m = CborValue::make_map();
    m.items.push_back(CborValue::make_text("frame"));
    m.items.push_back(CborValue::make_text("ICRF"));
    m.items.push_back(CborValue::make_text("record_count"));
    m.items.push_back(CborValue::make_unsigned(10));
    return m;
}

} // namespace

TEST_CASE("roundtrip_small_mixed") {
    const std::string path = temp_path("small");
    {
        auto w = Writer::create(path, WriterOptions{.chunk_records = 3});
        CHECK(w.ok());
        catalog::Writer& writer = w.value();

        CHECK(writer.add(expected_for(20000001), "1", "Ceres").ok());
        CHECK(writer.add(expected_for(20000002), "2P").ok());
        CHECK(writer.add(expected_for(20000003), "3", "Juno").ok());
        for (uint64_t spkid = 20000004; spkid <= 20000010; ++spkid) {
            CHECK(writer.add(expected_for(spkid), std::to_string(spkid - 20000000)).ok());
        }
        CHECK(writer.finish(make_meta()).ok());
        CHECK(writer.record_count() == 10);
    }

    auto r = Reader::open(path);
    REQUIRE(r.ok());
    catalog::Reader& reader = r.value();
    CHECK(reader.record_count() == 10);
    const CborValue* frame = reader.metadata().find("frame");
    CHECK((frame != nullptr && frame->text == "ICRF"));

    int seen = 0;
    auto fe = reader.for_each([&](const Record& rec, const Names& names) {
        expect_records_equal(rec, expected_for(rec.spkid));
        ++seen;
        if (rec.spkid == 20000001) {
            CHECK(names.pdes == "1");
            CHECK(names.name == "Ceres");
            CHECK(rec.has(RecordFlags::kHasName));
        } else if (rec.spkid == 20000002) {
            CHECK(names.pdes == "2P");
            CHECK(names.name.empty());
        }
    });
    CHECK(fe.ok());
    CHECK(seen == 10);

    // Lookups across chunk boundaries (chunk_records = 3 -> 4 chunks).
    for (uint64_t spkid : {20000001ull, 20000003ull, 20000004ull, 20000010ull}) {
        auto rec = reader.lookup(spkid);
        CHECK(rec.ok());
        if (rec.ok())
            expect_records_equal(rec.value(), expected_for(spkid));
    }
    auto missing = reader.lookup(20000011);
    CHECK(!missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    auto below = reader.lookup(1);
    CHECK(!below.ok());
    CHECK(below.error().code == ErrorCode::NotFound);

    std::filesystem::remove(path);
}

TEST_CASE("empty_catalog") {
    const std::string path = temp_path("empty");
    {
        auto w = Writer::create(path, WriterOptions{});
        CHECK(w.ok());
        CHECK(w.value().finish(CborValue::make_map()).ok());
    }
    auto r = Reader::open(path);
    CHECK(r.ok());
    if (r.ok()) {
        CHECK(r.value().record_count() == 0);
        auto rec = r.value().lookup(1);
        CHECK(!rec.ok());
        CHECK(rec.error().code == ErrorCode::NotFound);
        int seen = 0;
        r.value().for_each([&](const Record&, const Names&) { ++seen; });
        CHECK(seen == 0);
    }
    std::filesystem::remove(path);
}

TEST_CASE("writer_rejects_bad_input") {
    const std::string path = temp_path("badinput");
    auto w = Writer::create(path, WriterOptions{});
    CHECK(w.ok());
    catalog::Writer& writer = w.value();

    Record r = make_asteroid(20000005);
    CHECK(writer.add(r, "5").ok());

    auto e = writer.add(make_asteroid(20000005), "5"); // duplicate
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);

    e = writer.add(make_asteroid(20000004), "4"); // descending
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);

    Record nan_r = make_asteroid(20000006);
    nan_r.a_au = std::nan("");
    e = writer.add(nan_r, "6");
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);

    Record bad_e = make_asteroid(20000006);
    bad_e.e = 1.0; // parabolic: elements singular
    e = writer.add(bad_e, "6");
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);

    Record bad_ae = make_asteroid(20000006);
    bad_ae.e = 1.5;
    bad_ae.a_au = 2.0; // hyperbolic e must have a < 0
    e = writer.add(bad_ae, "6");
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);

    Record hyper = make_asteroid(20000006);
    hyper.body_class = BodyClass::Comet;
    hyper.e = 1.2;
    hyper.a_au = -5.0;
    CHECK(writer.add(hyper, "P/2006 W3").ok());

    e = writer.finish(CborValue{}); // metadata must be a map
    CHECK(!e.ok());
    CHECK(e.error().code == ErrorCode::ArgumentError);
    e = writer.finish(make_meta());
    CHECK(e.ok());
    std::filesystem::remove(path);
}

TEST_CASE("corruption_detected") {
    // Build, then flip a byte inside chunk data and expect a CRC failure.
    const std::string path = temp_path("corrupt");
    {
        auto w = Writer::create(path, WriterOptions{.chunk_records = 4});
        CHECK(w.ok());
        for (uint64_t spkid = 20000001; spkid <= 20000012; ++spkid) {
            CHECK(w.value().add(make_asteroid(spkid), std::to_string(spkid - 20000000)).ok());
        }
        CHECK(w.value().finish(make_meta()).ok());
    }
    {
        std::string bytes;
        bytes.resize(std::filesystem::file_size(path));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        CHECK(got == bytes.size());
        bytes[70] = char(bytes[70] ^ 0x5A); // first chunk's payload
        std::FILE* g = std::fopen(path.c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), g);
        std::fclose(g);
    }
    auto r = Reader::open(path);
    CHECK(r.ok()); // header/index fine; corruption is in chunk payload
    if (r.ok()) {
        auto fe = r.value().for_each([&](const Record&, const Names&) {});
        CHECK(!fe.ok());
        CHECK(fe.error().code == ErrorCode::CorruptionError);
        auto lk = r.value().lookup(20000003);
        CHECK(!lk.ok());
        CHECK(lk.error().code == ErrorCode::CorruptionError);
    }
    std::filesystem::remove(path);
}

TEST_CASE("truncation_and_magic") {
    const std::string path = temp_path("trunc");
    {
        auto w = Writer::create(path, WriterOptions{});
        CHECK(w.ok());
        for (uint64_t spkid = 20000001; spkid <= 20000005; ++spkid) {
            CHECK(w.value().add(make_asteroid(spkid), std::to_string(spkid - 20000000)).ok());
        }
        CHECK(w.value().finish(make_meta()).ok());
    }

    const std::string trunc = temp_path("truncshort");
    {
        const auto size = std::filesystem::file_size(path);
        std::string bytes;
        bytes.resize(size / 2);
        std::FILE* f = std::fopen(path.c_str(), "rb");
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        CHECK(got == bytes.size());
        std::FILE* g = std::fopen(trunc.c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), g);
        std::fclose(g);
    }
    auto rt = Reader::open(trunc);
    CHECK(!rt.ok());
    CHECK((rt.error().code == ErrorCode::CorruptionError ||
           rt.error().code == ErrorCode::FormatError || rt.error().code == ErrorCode::IoError));
    std::filesystem::remove(trunc);

    const std::string badmagic = temp_path("badmagic");
    {
        std::string bytes;
        bytes.resize(std::filesystem::file_size(path));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        CHECK(got == bytes.size());
        bytes[0] = 'X';
        std::FILE* g = std::fopen(badmagic.c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), g);
        std::fclose(g);
    }
    auto bm = Reader::open(badmagic);
    CHECK(!bm.ok());
    CHECK(bm.error().code == ErrorCode::FormatError);
    std::filesystem::remove(badmagic);
    std::filesystem::remove(path);
}

TEST_CASE("uncompressed_variant") {
    const std::string path = temp_path("raw");
    {
        auto w = Writer::create(path, WriterOptions{.compress = false});
        CHECK(w.ok());
        for (uint64_t spkid = 20000001; spkid <= 20000006; ++spkid) {
            CHECK(w.value().add(make_asteroid(spkid), std::to_string(spkid - 20000000)).ok());
        }
        CHECK(w.value().finish(make_meta()).ok());
    }
    auto r = Reader::open(path);
    CHECK(r.ok());
    if (r.ok()) {
        CHECK(r.value().record_count() == 6);
        auto rec = r.value().lookup(20000004);
        CHECK(rec.ok());
        if (rec.ok())
            expect_records_equal(rec.value(), make_asteroid(20000004));
    }
    std::filesystem::remove(path);
}

TEST_CASE("medium_scale_and_size") {
    const std::string path = temp_path("medium");
    const uint64_t kCount = 50000;
    {
        auto w = Writer::create(path, WriterOptions{.chunk_records = 4096});
        CHECK(w.ok());
        for (uint64_t spkid = 20000001; spkid < 20000001 + kCount; ++spkid) {
            Record r = make_asteroid(spkid);
            r.flags = uint8_t(RecordFlags::kSigmas | RecordFlags::kHg);
            CHECK(w.value().add(r, std::to_string(spkid - 20000000)).ok());
        }
        CHECK(w.value().finish(make_meta()).ok());
    }
    const auto file_bytes = std::filesystem::file_size(path);
    const double per_record = double(file_bytes) / double(kCount);
    std::printf("  medium catalog: %llu records, %llu bytes, %.1f bytes/record\n",
                (unsigned long long)kCount, (unsigned long long)file_bytes, per_record);
    CHECK(per_record < 128.0); // spec target: well under with zstd

    auto r = Reader::open(path);
    CHECK(r.ok());
    if (r.ok()) {
        catalog::Reader& reader = r.value();
        auto expected = [](uint64_t spkid) {
            Record g = make_asteroid(spkid);
            g.flags = uint8_t(RecordFlags::kSigmas | RecordFlags::kHg);
            return g;
        };
        // Scattered lookups; the repeated probe exercises the chunk cache path.
        for (uint64_t probe : {20000001ull, 20012345ull, 20025000ull, 20049999ull, 20012345ull}) {
            auto rec = reader.lookup(probe);
            CHECK(rec.ok());
            if (rec.ok())
                expect_records_equal(rec.value(), expected(probe));
        }
        uint64_t scanned = 0;
        auto fe = reader.for_each([&](const Record& rec, const Names&) { ++scanned; });
        CHECK(fe.ok());
        CHECK(scanned == kCount);
        CHECK(reader.stats().chunks_read > 0);
    }
    std::filesystem::remove(path);
}

TEST_CASE("real_data_fixture") {
    // Committed 100-body catalog built from the real SBDB pull (Ceres..100).
    // Guards the reader against real-world data the synthetic tests cannot:
    // full-precision elements, actual sigma magnitudes, genuine name pool.
    const std::string path =
        (std::filesystem::path(__FILE__).parent_path() / "data" / "sample-100.epm").string();
    auto r = Reader::open(path);
    REQUIRE(r.ok());
    catalog::Reader& reader = r.value();
    CHECK(reader.record_count() == 100);

    // Ceres: spkid 20000001, full-precision elements from SBDB 2026-09-16.
    auto rec = reader.lookup(20000001);
    CHECK(rec.ok());
    if (rec.ok()) {
        CHECK(rec.value().epoch_jtdb == 2461200.5);
        CHECK(rec.value().a_au == 2.765552595034094);
        CHECK(rec.value().e == 0.07969229514816586);
        CHECK(rec.value().body_class == BodyClass::Asteroid);
        CHECK(rec.value().has(RecordFlags::kSigmas));
        CHECK(rec.value().has(RecordFlags::kHg));
        CHECK(rec.value().has(RecordFlags::kDiameter));
    }
    auto fe = reader.for_each([](const Record&, const Names&) {});
    CHECK(fe.ok());
}
