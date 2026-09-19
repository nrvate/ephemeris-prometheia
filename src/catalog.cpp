// SPDX-License-Identifier: GPL-2.0-or-later
#include <prometheia/catalog.hpp>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <zstd.h>

#include <prometheia/crc32.hpp>
#include <prometheia/varint.hpp>

namespace prometheia::catalog {

namespace {

constexpr uint32_t kFlagChunkZstd = kFlagZstd;
constexpr size_t kIndexEntrySize = 40; // 4x u64, 2x u32
constexpr uint8_t kKnownFlagMask =
    uint8_t(RecordFlags::kSigmas | RecordFlags::kHg | RecordFlags::kDiameter |
            RecordFlags::kHasName | RecordFlags::kCovariance);

bool body_class_valid(uint8_t c) {
    return c == uint8_t(BodyClass::Asteroid) || c == uint8_t(BodyClass::Comet) ||
           c == uint8_t(BodyClass::Planet) || c == uint8_t(BodyClass::Other);
}

void append_chunk_index_entry(std::string& out, uint64_t data_offset, uint64_t stored_size,
                              uint64_t raw_size, uint32_t crc, uint32_t record_count,
                              uint64_t first_spkid) {
    put_u64(out, data_offset);
    put_u64(out, stored_size);
    put_u64(out, raw_size);
    put_u32(out, crc);
    put_u32(out, record_count);
    put_u64(out, first_spkid);
}

void encode_record(std::string& out, const Record& r) {
    put_uvarint(out, r.spkid);
    out.push_back(char(uint8_t(r.body_class)));
    out.push_back(char(r.flags));
    put_f64(out, r.epoch_jtdb);
    put_f64(out, r.a_au);
    put_f64(out, r.e);
    put_f64(out, r.inc_rad);
    put_f64(out, r.node_rad);
    put_f64(out, r.argp_rad);
    put_f64(out, r.mean_anom_rad);
    if (r.has(RecordFlags::kSigmas)) {
        for (double s : r.sigmas)
            put_f32(out, float(s));
    }
    if (r.has(RecordFlags::kHg)) {
        put_f32(out, r.h_mag);
        put_f32(out, r.g_slope);
    }
    if (r.has(RecordFlags::kDiameter))
        put_f32(out, r.diameter_km);
    if (r.has(RecordFlags::kCovariance)) {
        put_f64(out, r.cov_epoch_jtdb);
        for (double v : r.cov_elements)
            put_f64(out, v);
        for (double v : r.covariance)
            put_f64(out, v);
    }
    put_uvarint(out, r.name_offset);
}

// Decodes one record.
const char* decode_record(const char* p, const char* end, std::string_view pool, Record& r,
                          Error& err) {
    auto fail = [&](const char* what) {
        err = make_error(ErrorCode::CorruptionError, std::string("record: ") + what);
        return nullptr;
    };
    uint64_t spkid;
    if (!(p = get_uvarint(p, end, spkid)))
        return fail("truncated spkid");
    if (end - p < 2)
        return fail("truncated class/flags");
    const uint8_t body_class = uint8_t(p[0]);
    const uint8_t flags = uint8_t(p[1]);
    p += 2;
    if (!body_class_valid(body_class))
        return fail("unknown body class");
    if (flags & ~kKnownFlagMask)
        return fail("unknown record flags");

    r = Record{};
    r.spkid = spkid;
    r.body_class = BodyClass(body_class);
    r.flags = flags;
    if (!(p = get_f64(p, end, r.epoch_jtdb)))
        return fail("truncated epoch");
    if (!(p = get_f64(p, end, r.a_au)))
        return fail("truncated elements");
    if (!(p = get_f64(p, end, r.e)))
        return fail("truncated elements");
    if (!(p = get_f64(p, end, r.inc_rad)))
        return fail("truncated elements");
    if (!(p = get_f64(p, end, r.node_rad)))
        return fail("truncated elements");
    if (!(p = get_f64(p, end, r.argp_rad)))
        return fail("truncated elements");
    if (!(p = get_f64(p, end, r.mean_anom_rad)))
        return fail("truncated elements");
    if (flags & RecordFlags::kSigmas) {
        for (double& s : r.sigmas) {
            float f32v;
            if (!(p = get_f32(p, end, f32v)))
                return fail("truncated sigmas");
            s = f32v;
        }
    }
    if (flags & RecordFlags::kHg) {
        if (!(p = get_f32(p, end, r.h_mag)))
            return fail("truncated H");
        if (!(p = get_f32(p, end, r.g_slope)))
            return fail("truncated G");
    }
    if (flags & RecordFlags::kDiameter) {
        if (!(p = get_f32(p, end, r.diameter_km)))
            return fail("truncated diameter");
    }
    if (flags & RecordFlags::kCovariance) {
        if (!(p = get_f64(p, end, r.cov_epoch_jtdb)))
            return fail("truncated covariance");
        for (double& v : r.cov_elements) {
            if (!(p = get_f64(p, end, v)))
                return fail("truncated covariance");
        }
        for (double& v : r.covariance) {
            if (!(p = get_f64(p, end, v)))
                return fail("truncated covariance");
        }
    }
    if (!(p = get_uvarint(p, end, r.name_offset)))
        return fail("truncated name offset");
    if (r.name_offset > pool.size())
        return fail("name offset out of pool range");
    return p;
}

} // namespace

Names record_names(std::string_view pool, const Record& r) {
    if (r.name_offset >= pool.size())
        return {};
    std::string_view rest = pool.substr(r.name_offset);
    const size_t nul = rest.find('\0');
    if (nul == std::string_view::npos)
        return {rest, {}};
    Names names;
    names.pdes = rest.substr(0, nul);
    if (r.has(RecordFlags::kHasName) && nul + 1 < rest.size()) {
        std::string_view after = rest.substr(nul + 1);
        const size_t nul2 = after.find('\0');
        names.name = nul2 == std::string_view::npos ? after : after.substr(0, nul2);
    }
    return names;
}

// ---------------------------------------------------------------- Writer

Result<Writer> Writer::create(const std::string& path, WriterOptions options) {
    if (options.chunk_records == 0) {
        return make_error(ErrorCode::ArgumentError, "chunk_records must be nonzero");
    }
    Writer w;
    w.f_ = std::fopen(path.c_str(), "wb");
    if (w.f_ == nullptr) {
        return make_error(ErrorCode::IoError, "cannot open '" + path + "' for writing");
    }
    w.opts_ = options;
    // Reserve the header; the real one is written by finish().
    std::string zeros(kHeaderSize, '\0');
    if (std::fwrite(zeros.data(), 1, zeros.size(), w.f_) != zeros.size()) {
        std::fclose(w.f_);
        return make_error(ErrorCode::IoError, "short write reserving header");
    }
    w.file_pos_ = kHeaderSize;
    return w;
}

Result<void> Writer::flush_chunk() {
    if (chunk_buf_.empty())
        return {};
    const uint64_t raw_size = chunk_buf_.size();
    const uint32_t crc = crc32(chunk_buf_.data(), chunk_buf_.size());
    std::string stored;
    if (opts_.compress) {
        const size_t bound = ZSTD_compressBound(raw_size);
        std::vector<char> dst(bound);
        const size_t zsize =
            ZSTD_compress(dst.data(), bound, chunk_buf_.data(), raw_size, opts_.zstd_level);
        if (ZSTD_isError(zsize)) {
            return make_error(ErrorCode::IoError,
                              std::string("zstd compress failed: ") + ZSTD_getErrorName(zsize));
        }
        stored.assign(dst.data(), zsize);
    } else {
        stored = chunk_buf_;
    }
    const uint64_t first_spkid = first_spkid_of_chunk_;
    if (std::fwrite(stored.data(), 1, stored.size(), f_) != stored.size()) {
        return make_error(ErrorCode::IoError, "short write of chunk data");
    }
    entries_.push_back(ChunkEntry{file_pos_, stored.size(), raw_size, first_spkid, crc,
                                  uint32_t(chunk_count_in_buf_)});
    file_pos_ += stored.size();
    chunk_buf_.clear();
    chunk_count_in_buf_ = 0;
    first_spkid_of_chunk_ = 0;
    return {};
}

Result<void> Writer::add(const Record& r, std::string_view pdes, std::string_view name) {
    if (f_ == nullptr)
        return make_error(ErrorCode::ArgumentError, "writer is closed");
    if (pdes.empty())
        return make_error(ErrorCode::ArgumentError, "pdes must be non-empty");
    if (have_last_ && r.spkid <= last_spkid_) {
        return make_error(ErrorCode::ArgumentError, "spkid " + std::to_string(r.spkid) +
                                                        " not strictly ascending (previous " +
                                                        std::to_string(last_spkid_) + ")");
    }
    if (r.flags & ~kKnownFlagMask) {
        return make_error(ErrorCode::ArgumentError, "unknown record flag bits");
    }
    const bool finite = std::isfinite(r.epoch_jtdb) && std::isfinite(r.a_au) &&
                        std::isfinite(r.e) && std::isfinite(r.inc_rad) &&
                        std::isfinite(r.node_rad) && std::isfinite(r.argp_rad) &&
                        std::isfinite(r.mean_anom_rad);
    if (!finite)
        return make_error(ErrorCode::ArgumentError, "non-finite element");
    if (r.e < 0 || r.e == 1.0) {
        return make_error(ErrorCode::ArgumentError, "eccentricity must be >= 0 and != 1");
    }
    if (r.a_au == 0 || (r.e < 1 && r.a_au <= 0) || (r.e > 1 && r.a_au >= 0)) {
        return make_error(ErrorCode::ArgumentError, "a/e inconsistency (hyperbolic a<0)");
    }
    if (r.has(RecordFlags::kCovariance)) {
        bool ok = std::isfinite(r.cov_epoch_jtdb);
        for (double v : r.cov_elements)
            ok = ok && std::isfinite(v);
        for (double v : r.covariance)
            ok = ok && std::isfinite(v);
        for (int i = 0; i < 6; ++i)
            ok = ok && r.covariance[packed_index(i, i)] >= 0.0;
        // Cometary elements: e >= 0 and != 1 (as above), perihelion q > 0.
        ok = ok && r.cov_elements[0] >= 0.0 && r.cov_elements[0] != 1.0 && r.cov_elements[1] > 0.0;
        if (!ok)
            return make_error(ErrorCode::ArgumentError,
                              "covariance block: non-finite value, negative variance, "
                              "or invalid e/q");
    }
    if (pdes.find('\0') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return make_error(ErrorCode::ArgumentError, "NUL in pdes/name");
    }

    Record out = r;
    out.body_class = r.body_class;
    out.name_offset = pool_.size();
    if (name.empty()) {
        out.flags &= uint8_t(~RecordFlags::kHasName);
        pool_.append(pdes.data(), pdes.size());
        pool_.push_back('\0');
    } else {
        out.flags |= RecordFlags::kHasName;
        pool_.append(pdes.data(), pdes.size());
        pool_.push_back('\0');
        pool_.append(name.data(), name.size());
        pool_.push_back('\0');
    }

    if (chunk_count_in_buf_ == 0)
        first_spkid_of_chunk_ = out.spkid;
    encode_record(chunk_buf_, out);
    ++chunk_count_in_buf_;
    ++count_;
    last_spkid_ = out.spkid;
    have_last_ = true;
    if (uint32_t(chunk_count_in_buf_) >= opts_.chunk_records)
        return flush_chunk();
    return {};
}

Result<void> Writer::finish(CborValue metadata) {
    if (f_ == nullptr)
        return make_error(ErrorCode::ArgumentError, "writer is closed");
    if (metadata.type != CborValue::Type::Map) {
        return make_error(ErrorCode::ArgumentError, "metadata must be a CBOR map");
    }
    if (auto e = flush_chunk(); !e.ok())
        return e;

    const uint64_t pool_offset = file_pos_;
    if (!pool_.empty() && std::fwrite(pool_.data(), 1, pool_.size(), f_) != pool_.size()) {
        return make_error(ErrorCode::IoError, "short write of string pool");
    }
    const uint64_t pool_size = pool_.size();
    file_pos_ += pool_size;

    const uint64_t index_offset = file_pos_;
    {
        std::string index;
        index.reserve(entries_.size() * kIndexEntrySize);
        for (const auto& e : entries_) {
            append_chunk_index_entry(index, e.data_offset, e.stored_size, e.raw_size, e.crc,
                                     e.record_count, e.first_spkid);
        }
        if (!index.empty() && std::fwrite(index.data(), 1, index.size(), f_) != index.size()) {
            return make_error(ErrorCode::IoError, "short write of chunk index");
        }
        file_pos_ += index.size();
    }

    const uint64_t meta_offset = file_pos_;
    std::string meta;
    cbor_encode(meta, metadata);
    if (!meta.empty() && std::fwrite(meta.data(), 1, meta.size(), f_) != meta.size()) {
        return make_error(ErrorCode::IoError, "short write of metadata");
    }
    const uint64_t meta_size = meta.size();
    file_pos_ += meta_size;

    // Footer.
    {
        std::string footer;
        footer.reserve(kFooterSize);
        put_u64(footer, index_offset);
        put_u64(footer, meta_offset);
        put_u64(footer, meta_size);
        put_u64(footer, pool_offset);
        put_u64(footer, pool_size);
        put_u32(footer, kMagic);
        put_u16(footer, kFormatMajor);
        put_u16(footer, kFormatMinor);
        if (std::fwrite(footer.data(), 1, footer.size(), f_) != footer.size()) {
            return make_error(ErrorCode::IoError, "short write of footer");
        }
        file_pos_ += footer.size();
    }

    // Final header.
    {
        std::string header;
        header.reserve(kHeaderSize);
        put_u32(header, kMagic);
        put_u16(header, kFormatMajor);
        put_u16(header, kFormatMinor);
        put_u32(header, kHeaderSize);
        put_u32(header, opts_.compress ? kFlagChunkZstd : 0);
        put_u64(header, count_);
        put_u32(header, opts_.chunk_records);
        put_u32(header, 0); // reserved
        put_u64(header, index_offset);
        put_u64(header, meta_offset);
        put_u64(header, meta_size);
        put_u64(header, pool_offset);
        header.resize(kHeaderSize, '\0');
        if (std::fseek(f_, 0, SEEK_SET) != 0 ||
            std::fwrite(header.data(), 1, header.size(), f_) != header.size()) {
            return make_error(ErrorCode::IoError, "failed to write final header");
        }
    }

    std::fclose(f_);
    f_ = nullptr;
    return {};
}

// ---------------------------------------------------------------- Reader

Result<Reader> Reader::open(const std::string& path) {
    Reader r;
    r.f_ = std::fopen(path.c_str(), "rb");
    if (r.f_ == nullptr) {
        return make_error(ErrorCode::IoError, "cannot open '" + path + "' for reading");
    }
    if (std::fseek(r.f_, 0, SEEK_END) != 0) {
        return make_error(ErrorCode::IoError, "seek failed");
    }
    const long fsize = std::ftell(r.f_);
    if (fsize < long(kHeaderSize + kFooterSize)) {
        return make_error(ErrorCode::FormatError, "file too small to be EPM1");
    }

    auto read_at = [&](uint64_t off, size_t n, std::string& buf) -> Result<void> {
        if (std::fseek(r.f_, long(off), SEEK_SET) != 0)
            return make_error(ErrorCode::IoError, "seek failed");
        buf.resize(n);
        if (std::fread(buf.data(), 1, n, r.f_) != n)
            return make_error(ErrorCode::IoError, "short read");
        return {};
    };

    std::string header;
    if (auto e = read_at(0, kHeaderSize, header); !e.ok())
        return Result<Reader>(e.error());
    if (get_u32(header.data()) != kMagic)
        return make_error(ErrorCode::FormatError, "bad magic (not an EPM1 file)");
    if (get_u16(header.data() + 4) != kFormatMajor)
        return make_error(ErrorCode::FormatError, "unsupported EPM major version");
    if (get_u32(header.data() + 8) != kHeaderSize)
        return make_error(ErrorCode::FormatError, "unexpected header size");

    r.compressed_ = (get_u32(header.data() + 12) & kFlagChunkZstd) != 0;
    r.record_count_ = get_u64(header.data() + 16);
    r.chunk_records_ = get_u32(header.data() + 24);
    const uint64_t index_offset = get_u64(header.data() + 32);
    const uint64_t meta_offset = get_u64(header.data() + 40);
    const uint64_t meta_size = get_u64(header.data() + 48);
    const uint64_t pool_offset = get_u64(header.data() + 56);

    std::string footer;
    if (auto e = read_at(uint64_t(fsize) - kFooterSize, kFooterSize, footer); !e.ok())
        return Result<Reader>(e.error());
    if (get_u32(footer.data() + 40) != kMagic || get_u16(footer.data() + 44) != kFormatMajor)
        return make_error(ErrorCode::CorruptionError, "footer invalid (truncated file?)");
    if (get_u64(footer.data()) != index_offset || get_u64(footer.data() + 8) != meta_offset)
        return make_error(ErrorCode::CorruptionError, "header/footer offsets disagree");
    const uint64_t pool_size = get_u64(footer.data() + 32);

    const uint64_t file_size = uint64_t(fsize);
    if (meta_offset + meta_size > file_size || pool_offset + pool_size > file_size ||
        pool_offset + pool_size > index_offset || index_offset > meta_offset) {
        return make_error(ErrorCode::CorruptionError, "section offsets out of range");
    }
    if (r.chunk_records_ == 0 && r.record_count_ > 0) {
        return make_error(ErrorCode::CorruptionError, "zero chunk size with records present");
    }

    const uint64_t chunk_count =
        r.record_count_ == 0 ? 0 : (r.record_count_ + r.chunk_records_ - 1) / r.chunk_records_;
    const uint64_t index_size = chunk_count * kIndexEntrySize;
    if (index_offset + index_size > file_size) {
        return make_error(ErrorCode::CorruptionError, "chunk index out of range");
    }

    std::string meta_bytes;
    if (auto e = read_at(meta_offset, size_t(meta_size), meta_bytes); !e.ok())
        return Result<Reader>(e.error());
    auto meta = cbor_decode(meta_bytes.data(), meta_bytes.size());
    if (!meta)
        return Result<Reader>(std::move(meta.error()));
    r.metadata_ = std::move(meta.value());

    r.entries_.resize(size_t(chunk_count));
    if (chunk_count > 0) {
        std::string index;
        if (auto e = read_at(index_offset, size_t(index_size), index); !e.ok())
            return Result<Reader>(e.error());
        const char* p = index.data();
        uint64_t prev_first = 0;
        uint64_t total_records = 0;
        for (uint64_t i = 0; i < chunk_count; ++i, p += kIndexEntrySize) {
            ChunkEntry& e = r.entries_[size_t(i)];
            e.data_offset = get_u64(p);
            e.stored_size = get_u64(p + 8);
            e.raw_size = get_u64(p + 16);
            e.crc = get_u32(p + 24);
            e.record_count = get_u32(p + 28);
            e.first_spkid = get_u64(p + 32);
            if (e.record_count == 0 || e.raw_size == 0 || e.stored_size == 0) {
                return make_error(ErrorCode::CorruptionError, "degenerate chunk entry");
            }
            if (e.data_offset < kHeaderSize || e.data_offset + e.stored_size > file_size) {
                return make_error(ErrorCode::CorruptionError, "chunk data out of range");
            }
            if (i > 0 && e.first_spkid <= prev_first) {
                return make_error(ErrorCode::CorruptionError, "chunk index not strictly ascending");
            }
            prev_first = e.first_spkid;
            total_records += e.record_count;
        }
        if (total_records != r.record_count_) {
            return make_error(ErrorCode::CorruptionError,
                              "chunk record counts disagree with header");
        }
    }

    if (pool_size > 0) {
        if (auto e = read_at(pool_offset, size_t(pool_size), r.pool_); !e.ok())
            return Result<Reader>(e.error());
    }
    return r;
}

Result<const std::string*> Reader::load_chunk(uint64_t chunk_index) const {
    if (chunk_index == cached_chunk_) {
        ++stats_.chunk_cache_hits;
        return &cached_data_;
    }
    const ChunkEntry& e = entries_[size_t(chunk_index)];
    std::string stored;
    if (std::fseek(f_, long(e.data_offset), SEEK_SET) != 0)
        return make_error(ErrorCode::IoError, "seek to chunk failed");
    stored.resize(size_t(e.stored_size));
    if (std::fread(stored.data(), 1, stored.size(), f_) != stored.size())
        return make_error(ErrorCode::IoError, "short read of chunk");

    std::string raw;
    if (compressed_) {
        raw.resize(size_t(e.raw_size));
        const size_t rc = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
        if (ZSTD_isError(rc) || rc != e.raw_size) {
            return make_error(ErrorCode::CorruptionError, "zstd decompress failed");
        }
    } else {
        raw = std::move(stored);
    }
    if (crc32(raw.data(), raw.size()) != e.crc) {
        return make_error(ErrorCode::CorruptionError, "chunk CRC mismatch");
    }
    cached_chunk_ = chunk_index;
    cached_data_ = std::move(raw);
    ++stats_.chunks_read;
    return &cached_data_;
}

Result<Record> Reader::lookup(uint64_t spkid) const {
    if (entries_.empty() || spkid < entries_.front().first_spkid) {
        return make_error(ErrorCode::NotFound, "spkid below first chunk");
    }
    // Last chunk whose first_spkid <= spkid.
    uint64_t lo = 0, hi = entries_.size();
    while (lo + 1 < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (entries_[size_t(mid)].first_spkid <= spkid) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    auto chunk = load_chunk(lo);
    if (!chunk.ok())
        return Result<Record>(chunk.error());
    const char* p = chunk.value()->data();
    const char* end = p + chunk.value()->size();
    Error err;
    for (uint32_t i = 0; i < entries_[size_t(lo)].record_count; ++i) {
        Record r;
        const char* next = decode_record(p, end, pool_, r, err);
        if (next == nullptr)
            return Result<Record>(err);
        if (r.spkid == spkid)
            return r;
        if (r.spkid > spkid)
            break; // sorted: cannot appear later
        p = next;
    }
    return make_error(ErrorCode::NotFound, "spkid not in catalog");
}

Result<void>
Reader::for_each_name(const std::function<void(uint64_t spkid, const Names&)>& fn) const {
    Record r; // only spkid, flags and name_offset are read into it
    for (uint64_t i = 0; i < entries_.size(); ++i) {
        auto chunk = load_chunk(i);
        if (!chunk.ok())
            return chunk.error();
        const char* p = chunk.value()->data();
        const char* end = p + chunk.value()->size();
        for (uint32_t k = 0; k < entries_[size_t(i)].record_count; ++k) {
            // The layout decode_record reads, with the fixed-size fields
            // stepped over by what the flags say is present.
            if (!(p = get_uvarint(p, end, r.spkid)) || end - p < 2)
                return make_error(ErrorCode::CorruptionError, "record: truncated");
            if (!body_class_valid(uint8_t(p[0])) || (uint8_t(p[1]) & ~kKnownFlagMask))
                return make_error(ErrorCode::CorruptionError, "record: bad class or flags");
            r.flags = uint8_t(p[1]);
            size_t skip = 2 + 7 * 8; // class, flags; epoch and six elements
            if (r.has(RecordFlags::kSigmas))
                skip += 6 * 4;
            if (r.has(RecordFlags::kHg))
                skip += 2 * 4;
            if (r.has(RecordFlags::kDiameter))
                skip += 4;
            if (r.has(RecordFlags::kCovariance))
                skip += (1 + 6 + 21) * 8;
            if (end - p < std::ptrdiff_t(skip))
                return make_error(ErrorCode::CorruptionError, "record: truncated");
            p += skip;
            if (!(p = get_uvarint(p, end, r.name_offset)) || r.name_offset > pool_.size())
                return make_error(ErrorCode::CorruptionError, "record: bad name offset");
            fn(r.spkid, record_names(pool_, r));
        }
    }
    return {};
}

Result<void> Reader::verify() const {
    for (uint64_t i = 0; i < entries_.size(); ++i) {
        auto c = load_chunk(i);
        if (!c)
            return c.error();
    }
    return {};
}

Result<void> Reader::for_each(const std::function<void(const Record&, const Names&)>& fn) const {
    for (uint64_t i = 0; i < entries_.size(); ++i) {
        auto chunk = load_chunk(i);
        if (!chunk.ok())
            return chunk.error();
        const char* p = chunk.value()->data();
        const char* end = p + chunk.value()->size();
        Error err;
        for (uint32_t k = 0; k < entries_[size_t(i)].record_count; ++k) {
            Record r;
            const char* next = decode_record(p, end, pool_, r, err);
            if (next == nullptr)
                return err;
            fn(r, record_names(pool_, r));
            p = next;
        }
    }
    return {};
}

} // namespace prometheia::catalog
