// SPDX-License-Identifier: GPL-2.0-or-later
//
// Positional file reads (prometheia/io.hpp): the readers of DE, SPK and
// catalog files share one file object across reads, so a read must carry
// its own offset and be complete in itself.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <prometheia/io.hpp>

#include <doctest/doctest.h>

using prometheia::ErrorCode;
using prometheia::io::RandomAccessFile;
namespace fs = std::filesystem;

namespace {

struct TempPath {
    fs::path path;
    explicit TempPath(const char* stem) {
        path = fs::temp_directory_path() /
               (std::string("prometheia-io-") + stem + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ~TempPath() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

// Byte i of the test pattern.
unsigned char pattern(uint64_t i) {
    return static_cast<unsigned char>((i * 2654435761u) >> 13);
}

} // namespace

TEST_CASE("io_reads_at_offsets") {
    TempPath t("basic");
    {
        std::FILE* f = std::fopen(t.path.string().c_str(), "wb");
        REQUIRE(f);
        for (uint64_t i = 0; i < 100000; ++i)
            std::fputc(pattern(i), f);
        std::fclose(f);
    }
    auto opened = RandomAccessFile::open(t.path.string());
    REQUIRE(opened.ok());
    RandomAccessFile f = std::move(opened).value();
    CHECK(f.is_open());
    CHECK(f.size() == 100000);
    unsigned char buf[64];
    REQUIRE(f.read_exact(12345, buf, sizeof buf));
    for (size_t k = 0; k < sizeof buf; ++k)
        CHECK(buf[k] == pattern(12345 + k));
    // The end of the file: a short read, not an error; past it, nothing.
    auto tail = f.read_at(100000 - 10, buf, sizeof buf);
    REQUIRE(tail.ok());
    CHECK(tail.value() == 10);
    CHECK(!f.read_exact(100000 - 10, buf, sizeof buf));
    auto past = f.read_at(200000, buf, sizeof buf);
    REQUIRE(past.ok());
    CHECK(past.value() == 0);
    // A moved-from file is closed and refuses reads.
    RandomAccessFile g = std::move(f);
    CHECK(g.is_open());
    CHECK(!f.is_open());
    CHECK(f.read_at(0, buf, 1).error().code == ErrorCode::IoError);
    // Nothing to open.
    CHECK(RandomAccessFile::open(t.path.string() + ".missing").error().code == ErrorCode::IoError);
}

TEST_CASE("io_writers_and_readers_take_utf8_paths") {
    // The writers open through open_file and native_path, the readers through
    // RandomAccessFile: one UTF-8 name, written and read back. On Windows the
    // name is widened for _wfopen and CreateFileW (docs/EMBEDDING.md,
    // "Platforms"); here it is bytes.
    TempPath t("utf8");
    const fs::path named = t.path.parent_path() / (t.path.filename().string() + "-Ünïcödé-ε");
    const std::string utf8 = named.string();
    std::FILE* f = prometheia::io::open_file(utf8, "wb");
    REQUIRE(f);
    std::fputs("by open_file", f);
    std::fclose(f);
    CHECK(prometheia::io::native_path(utf8) == named);
    auto r = RandomAccessFile::open(utf8);
    REQUIRE(r.ok());
    char buf[13] = {};
    CHECK(r.value().read_exact(0, buf, 12));
    CHECK(std::string(buf) == "by open_file");
    std::error_code ec;
    fs::remove(named, ec);
}

TEST_CASE("io_one_file_from_two_threads") {
    // Two threads read interleaved offsets through the same object: with a
    // shared file position (seek, then read) they would read each other's
    // bytes; positional reads cannot.
    TempPath t("threads");
    constexpr uint64_t kSize = 1 << 20;
    {
        std::FILE* f = std::fopen(t.path.string().c_str(), "wb");
        REQUIRE(f);
        for (uint64_t i = 0; i < kSize; ++i)
            std::fputc(pattern(i), f);
        std::fclose(f);
    }
    auto opened = RandomAccessFile::open(t.path.string());
    REQUIRE(opened.ok());
    const RandomAccessFile& f = opened.value();
    std::atomic<int> wrong{0}, failed{0};
    const auto worker = [&](uint64_t start) {
        unsigned char buf[97];
        for (uint64_t n = 0; n < 20000; ++n) {
            const uint64_t off = (start + n * 7919) % (kSize - sizeof buf);
            if (!f.read_exact(off, buf, sizeof buf)) {
                ++failed;
                continue;
            }
            for (size_t k = 0; k < sizeof buf; ++k)
                if (buf[k] != pattern(off + k)) {
                    ++wrong;
                    break;
                }
        }
    };
    std::thread a(worker, 0), b(worker, 500000);
    a.join();
    b.join();
    CHECK(failed == 0);
    CHECK(wrong == 0);
}

#if !defined(_WIN32)
TEST_CASE("io_offsets_above_4_gb") {
    // A sparse file: a few bytes written past 4 GB, nothing stored below.
    TempPath t("large");
    const uint64_t far = (uint64_t(5) << 30) + 123; // 5 GiB and a bit
    {
        const int fd = ::open(t.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        REQUIRE(fd >= 0);
        const char mark[] = "beyond4GB";
        const bool wrote = ::pwrite(fd, mark, sizeof mark, off_t(far)) == ssize_t(sizeof mark);
        ::close(fd);
        if (!wrote) {
            MESSAGE("SKIP: this filesystem refused a sparse write past 4 GB");
            return;
        }
    }
    auto opened = RandomAccessFile::open(t.path.string());
    REQUIRE(opened.ok());
    CHECK(opened.value().size() == far + 10);
    char buf[10] = {};
    REQUIRE(opened.value().read_exact(far, buf, sizeof buf));
    CHECK(std::string(buf) == "beyond4GB");
    // The hole reads as zeros.
    REQUIRE(opened.value().read_exact(uint64_t(1) << 32, buf, 4));
    CHECK((buf[0] | buf[1] | buf[2] | buf[3]) == 0);
}
#endif
