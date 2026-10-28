// SPDX-License-Identifier: GPL-2.0-or-later
//
// EPM1 — the Ephemeris Prometheia catalog container, version 1.
//
// A catalog stores, per small body, one osculating-elements record at one
// epoch plus 1-sigma element uncertainties and physical parameters. There is
// deliberately no time axis: the engine integrates from these initial
// conditions on demand, so file size scales with the number of objects, not
// with objects x time-span. See docs/FORMAT.md for the byte-level spec.
//
// Threading: a Writer is single-threaded. A Reader is safe for concurrent
// const use only if every thread opens its own instance (the chunk cache is
// mutable); the engine hands out per-context readers, mirroring the
// no-shared-mutable-state rule of the library.
#ifndef PROMETHEIA_CATALOG_HPP
#define PROMETHEIA_CATALOG_HPP

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <prometheia/cbor.hpp>
#include <prometheia/error.hpp>
#include <prometheia/io.hpp>

namespace prometheia::catalog {

inline constexpr uint32_t kMagic = 0x314D5045u; // "EPM1" little-endian
inline constexpr uint16_t kFormatMajor = 1;
// 1.1: kCovariance records; 1.2: kOrbitClass and kHasProvDes.
inline constexpr uint16_t kFormatMinor = 2;
inline constexpr uint32_t kHeaderSize = 64;
inline constexpr uint32_t kFooterSize = 48;

// Header flag bits.
inline constexpr uint32_t kFlagZstd = 1u << 0; // chunks are zstd-compressed

enum class BodyClass : uint8_t {
    Asteroid = 0,
    Comet = 1,
    Planet = 2, // reserved for future bundled planetary packs
    Other = 255,
};

// Record flag bits (byte `flags` in the record).
struct RecordFlags {
    static constexpr uint8_t kSigmas = 1u << 0;     // 6 element sigmas present
    static constexpr uint8_t kHg = 1u << 1;         // H magnitude + G slope present
    static constexpr uint8_t kDiameter = 1u << 2;   // diameter present
    static constexpr uint8_t kHasName = 1u << 3;    // a proper name follows pdes in the pool
    static constexpr uint8_t kCovariance = 1u << 4; // full orbit covariance present (1.1)
    static constexpr uint8_t kOrbitClass = 1u << 5; // SBDB orbit class byte present (1.2)
    static constexpr uint8_t kHasProvDes = 1u << 6; // a provisional designation follows (1.2)
};

// SBDB's orbit classes (its `class` field), coded in one byte (1.2). The
// codes are fixed for the life of the format; 0 is "not recorded". Every
// class in the 2026-10 SBDB holdings is here, and PAA, which SBDB documents.
enum class OrbitClass : uint8_t {
    Unknown = 0,
    AST = 1,  // asteroid, unclassified
    IEO = 2,  // Atira: orbit inside the Earth's
    ATE = 3,  // Aten
    APO = 4,  // Apollo
    AMO = 5,  // Amor
    MCA = 6,  // Mars-crosser
    IMB = 7,  // inner main belt
    MBA = 8,  // main belt
    OMB = 9,  // outer main belt
    TJN = 10, // Jupiter trojan
    CEN = 11, // centaur
    TNO = 12, // trans-Neptunian object
    PAA = 13, // parabolic asteroid
    HYA = 14, // hyperbolic asteroid
    JFc = 20, // Jupiter-family comet
    JFC = 21, // Jupiter-family comet, by the classical period criterion
    HTC = 22, // Halley-type comet
    ETc = 23, // Encke-type comet
    CTc = 24, // Chiron-type comet
    COM = 25, // comet, unclassified
    PAR = 26, // parabolic comet
    HYP = 27, // hyperbolic comet
};

// SBDB's code ("MBA", "JFc") for a class, and back; nullopt for a code the
// table does not hold. Case matters: SBDB distinguishes JFc from JFC.
std::string_view orbit_class_code(OrbitClass c);
std::optional<OrbitClass> orbit_class_from_code(std::string_view code);

// Index of element pair (i, j), 0 <= i <= j < 6, in the packed upper
// triangle of a symmetric 6x6 matrix (row-major: (0,0), (0,1), .., (5,5)).
inline constexpr int packed_index(int i, int j) {
    if (i > j) {
        const int t = i;
        i = j;
        j = t;
    }
    return i * 6 - i * (i - 1) / 2 + (j - i);
}

struct Record {
    uint64_t spkid = 0;
    BodyClass body_class = BodyClass::Asteroid;
    uint8_t flags = 0;
    double epoch_jtdb = 0.0; // JD, TDB, equinox J2000
    // Osculating elements: semimajor axis (AU, negative for hyperbolic comets),
    // eccentricity, inclination, ascending node, argument of perihelion, mean
    // anomaly at epoch — all angles in radians.
    double a_au = 0.0, e = 0.0, inc_rad = 0.0, node_rad = 0.0, argp_rad = 0.0, mean_anom_rad = 0.0;
    double sigmas[6] = {0, 0, 0, 0, 0, 0}; // 1-sigma of {a, e, i, node, argp, M}
    float h_mag = 0.0f, g_slope = 0.0f;
    float diameter_km = 0.0f;
    // The orbit solution's full covariance (flag kCovariance), as published:
    // at its own epoch (which need not equal epoch_jtdb) and in cometary
    // elements {e, q [AU], tp [JD TDB], node [rad], peri [rad], i [rad]},
    // with the nominal values of those elements at that epoch. Covariance
    // is the packed upper triangle (packed_index), same units squared.
    double cov_epoch_jtdb = 0.0;
    double cov_elements[6] = {0, 0, 0, 0, 0, 0};
    double covariance[21] = {};
    OrbitClass orbit_class = OrbitClass::Unknown; // flag kOrbitClass (1.2)
    uint64_t name_offset = 0;                     // offset into the container's string pool

    bool has(uint8_t bit) const { return (flags & bit) != 0; }
};

struct Names {
    std::string_view pdes;     // primary designation, e.g. "1" or "2003 SB220"
    std::string_view name;     // proper name, e.g. "Ceres"; empty when absent
    std::string_view prov_des; // provisional designation of a numbered body (1.2),
                               // e.g. "2013 FZ27"; empty when absent
};

// Splits the pool string a record points at into {pdes, name}.
Names record_names(std::string_view pool, const Record& r);

struct WriterOptions {
    uint32_t chunk_records = 4096; // records per chunk (index granularity)
    bool compress = true;          // zstd-compress chunks
    int zstd_level = 3;
};

class Writer {
public:
    // Creates/truncates the file and writes a placeholder header. The final
    // header (with offsets) is written by finish().
    static Result<Writer> create(const std::string& path, WriterOptions options);

    // Appends one record. Records must arrive in strictly ascending spkid
    // order. `pdes` is the primary designation and must be non-empty; `name`
    // is the optional proper name; `prov_des` the optional provisional
    // designation (kept only when it differs from pdes). None may contain
    // NUL. The kHasName and kHasProvDes flags are derived here, and
    // kOrbitClass from r.orbit_class, so callers need not set them.
    Result<void> add(const Record& r, std::string_view pdes, std::string_view name = {},
                     std::string_view prov_des = {});

    // Writes the string pool, chunk index, metadata, final header and footer,
    // and closes. The Writer is unusable afterwards.
    Result<void> finish(CborValue metadata);

    uint64_t record_count() const { return count_; }

private:
    Result<void> flush_chunk();

    struct ChunkEntry {
        uint64_t data_offset, stored_size, raw_size, first_spkid;
        uint32_t crc;
        uint32_t record_count;
    };

    std::FILE* f_ = nullptr;
    WriterOptions opts_{};
    uint64_t count_ = 0;
    uint64_t last_spkid_ = 0;
    bool have_last_ = false;
    uint64_t file_pos_ = 0;           // absolute file position after header
    uint64_t chunk_count_in_buf_ = 0; // records in the open chunk
    uint64_t first_spkid_of_chunk_ = 0;
    std::string chunk_buf_; // raw (uncompressed) records of the current chunk
    std::string pool_;
    std::vector<ChunkEntry> entries_;
};

struct ReaderStats {
    uint64_t chunks_read = 0;
    uint64_t chunk_cache_hits = 0;
};

class Reader {
public:
    static Result<Reader> open(const std::string& path);

    const CborValue& metadata() const { return metadata_; }
    uint64_t record_count() const { return record_count_; }
    const std::string& name_pool() const { return pool_; }
    uint32_t chunk_records() const { return chunk_records_; }

    // Exact match on spkid. Uses a one-chunk cache: repeated lookups in the
    // same chunk do not re-read from disk.
    Result<Record> lookup(uint64_t spkid) const;

    // Streams every record in file order (ascending spkid), verifying each
    // chunk's CRC. Returns the first error encountered.
    Result<void> for_each(const std::function<void(const Record&, const Names&)>& fn) const;

    // Streams every record's SPK-ID and names, in file order, verifying each
    // chunk's CRC like for_each but decoding nothing else: what a name
    // index needs, at a fraction of the cost.
    Result<void> for_each_name(const std::function<void(uint64_t spkid, const Names&)>& fn) const;

    // Reads every chunk and checks its CRC, decoding no record: the whole
    // container's integrity at the cost of decompression alone.
    Result<void> verify() const;

    const ReaderStats& stats() const { return stats_; }

private:
    Result<const std::string*> load_chunk(uint64_t chunk_index) const;

    struct ChunkEntry {
        uint64_t data_offset, stored_size, raw_size, first_spkid;
        uint32_t crc;
        uint32_t record_count;
    };

    io::RandomAccessFile f_; // positional reads: no shared file position
    bool compressed_ = false;
    uint64_t record_count_ = 0;
    uint32_t chunk_records_ = 0;
    CborValue metadata_;
    std::string pool_;
    std::vector<ChunkEntry> entries_;
    // Single-chunk cache.
    mutable uint64_t cached_chunk_ = UINT64_MAX;
    mutable std::string cached_data_;
    mutable ReaderStats stats_;
};

} // namespace prometheia::catalog

#endif // PROMETHEIA_CATALOG_HPP
