// SPDX-License-Identifier: GPL-2.0-or-later
//
// NAIF DAF/SPK reader. See spk.hpp for the scope and docs/SPK.md for the
// byte maps and validation.
#include "prometheia/spk.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "prometheia/chebyshev.hpp"

namespace prometheia::spk {
namespace {

constexpr size_t kRecordBytes = 1024;
constexpr size_t kWordBytes = 8;

// File record byte map (DAF, record 1).
constexpr size_t kIdWordOffset = 0;   // 8 chars, "DAF/SPK "
constexpr size_t kNdOffset = 8;       // int32 ND (doubles per summary)
constexpr size_t kNiOffset = 12;      // int32 NI (integers per summary)
constexpr size_t kIfnOffset = 16;     // 60 chars internal file name
constexpr size_t kForwardOffset = 76; // int32 first summary record
constexpr size_t kFormatOffset = 88;  // 8 chars "LTL-IEEE" / "BIG-IEEE"
constexpr size_t kFtpOffset = 699;    // FTP transfer validation string
constexpr char kFtpString[] = "FTPSTR:\r:\n:\r\n:\r\0:\x81:\x10\xce:ENDFTP";
constexpr size_t kFtpBytes = sizeof(kFtpString) - 1; // 28

constexpr int kMaxSummaryRecords = 100000;
constexpr int kMaxChainDepth = 32;

constexpr uint64_t bswap64(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
        r |= ((v >> (8 * i)) & 0xFF) << (8 * (7 - i));
    return r;
}

constexpr uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

uint64_t le64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(uint8_t(p[i])) << (8 * i);
    return v;
}

uint32_t le32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(uint8_t(p[i])) << (8 * i);
    return v;
}

double f64_at(const char* p, bool swap) {
    const uint64_t v = le64(p);
    return std::bit_cast<double>(swap ? bswap64(v) : v);
}

int32_t i32_at(const char* p, bool swap) {
    const uint32_t v = le32(p);
    return int32_t(swap ? bswap32(v) : v);
}

std::string trimmed(const char* p, size_t n) {
    size_t end = n;
    while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == '\0'))
        --end;
    return std::string(p, end);
}

bool read_at(std::ifstream& f, uint64_t off, char* buf, size_t n) {
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(buf, std::streamsize(n));
    return uint64_t(f.gcount()) == n;
}

bool is_integral(double v) {
    return std::isfinite(v) && v == std::floor(v);
}

} // namespace

Result<SpkFile> SpkFile::open(const std::string& path) {
    SpkFile out;
    out.path_ = path;
    out.file_.open(path, std::ios::binary);
    if (!out.file_.is_open()) {
        return make_error(ErrorCode::IoError, "cannot open '" + path + "'");
    }
    out.file_.seekg(0, std::ios::end);
    const std::streamoff end = out.file_.tellg();
    const uint64_t size = end < 0 ? 0 : uint64_t(end);

    char fr[kRecordBytes];
    if (size < kRecordBytes || !read_at(out.file_, 0, fr, kRecordBytes)) {
        return make_error(ErrorCode::FormatError, "file too small to be a DAF: '" + path + "'");
    }
    if (std::memcmp(fr + kIdWordOffset, "DAF/SPK ", 8) != 0) {
        return make_error(ErrorCode::FormatError, "not a DAF/SPK file: '" + path + "'");
    }

    // Byte order from the format word; ND/NI must then read as SPK's 2/6.
    const std::string fmt(fr + kFormatOffset, 8);
    bool swap;
    if (fmt == "LTL-IEEE") {
        swap = false;
    } else if (fmt == "BIG-IEEE") {
        swap = true;
    } else {
        // Pre-format-word files: infer from ND.
        swap = i32_at(fr + kNdOffset, false) != 2;
    }
    const int32_t nd = i32_at(fr + kNdOffset, swap);
    const int32_t ni = i32_at(fr + kNiOffset, swap);
    if (nd != 2 || ni != 6) {
        return make_error(ErrorCode::FormatError,
                          "unexpected SPK summary shape ND=" + std::to_string(nd) +
                              " NI=" + std::to_string(ni) + " in '" + path + "'");
    }
    // A file sent through an ASCII-mode transfer has its line-ending bytes
    // rewritten; DAF carries a probe string to detect exactly that.
    if (fr[kFtpOffset] != '\0' && std::memcmp(fr + kFtpOffset, kFtpString, kFtpBytes) != 0) {
        return make_error(ErrorCode::CorruptionError,
                          "DAF FTP validation string damaged (ASCII-mode transfer?): '" + path +
                              "'");
    }
    out.swap_ = swap;
    out.internal_name_ = trimmed(fr + kIfnOffset, 60);
    out.forward_ = i32_at(fr + kForwardOffset, swap);

    const size_t summary_words = size_t(nd) + size_t((ni + 1) / 2); // 5
    const size_t summary_bytes = summary_words * kWordBytes;
    int32_t record = i32_at(fr + kForwardOffset, swap);
    int visited = 0;
    char sr[kRecordBytes], nr[kRecordBytes];
    while (record != 0) {
        if (record < 2 || ++visited > kMaxSummaryRecords ||
            uint64_t(record) * kRecordBytes > size) {
            return make_error(ErrorCode::CorruptionError, "bad DAF summary record pointer " +
                                                              std::to_string(record) + " in '" +
                                                              path + "'");
        }
        const uint64_t off = uint64_t(record - 1) * kRecordBytes;
        if (!read_at(out.file_, off, sr, kRecordBytes) ||
            !read_at(out.file_, off + kRecordBytes, nr, kRecordBytes)) {
            return make_error(ErrorCode::CorruptionError,
                              "truncated DAF summary/name record in '" + path + "'");
        }
        const double next = f64_at(sr, swap);
        const double count = f64_at(sr + 16, swap);
        const size_t per_record = (kRecordBytes - 3 * kWordBytes) / summary_bytes;
        if (!is_integral(next) || !is_integral(count) || count < 0 || count > double(per_record)) {
            return make_error(ErrorCode::CorruptionError,
                              "bad DAF summary record control words in '" + path + "'");
        }
        for (size_t k = 0; k < size_t(count); ++k) {
            const char* s = sr + 3 * kWordBytes + k * summary_bytes;
            Segment seg;
            seg.start_et = f64_at(s, swap);
            seg.end_et = f64_at(s + 8, swap);
            seg.target = i32_at(s + 16, swap);
            seg.center = i32_at(s + 20, swap);
            seg.frame = i32_at(s + 24, swap);
            seg.type = i32_at(s + 28, swap);
            const int32_t begin = i32_at(s + 32, swap);
            const int32_t finish = i32_at(s + 36, swap);
            seg.name = trimmed(nr + k * summary_bytes, summary_bytes);
            if (begin < 1 || finish < begin || uint64_t(finish) * kWordBytes > size ||
                !(seg.end_et >= seg.start_et)) {
                return make_error(ErrorCode::CorruptionError, "bad SPK segment summary (target " +
                                                                  std::to_string(seg.target) +
                                                                  ") in '" + path + "'");
            }
            seg.begin_word = uint64_t(begin);
            seg.end_word = uint64_t(finish);

            if (seg.type == 2 || seg.type == 3) {
                std::vector<double> dir;
                Result<void> r = out.read_words(seg.end_word - 3, 4, dir);
                if (!r.ok())
                    return r.error();
                const int components = seg.type == 2 ? 3 : 6;
                const double rsize = dir[2], n = dir[3];
                const bool shape_ok = is_integral(rsize) && is_integral(n) && n >= 1 &&
                                      rsize >= 2 + components &&
                                      std::fmod(rsize - 2, components) == 0 && dir[1] > 0 &&
                                      rsize * n + 4 == double(seg.end_word - seg.begin_word + 1);
                if (!shape_ok) {
                    return make_error(ErrorCode::CorruptionError,
                                      "bad type " + std::to_string(seg.type) +
                                          " directory in segment for target " +
                                          std::to_string(seg.target) + " in '" + path + "'");
                }
                seg.init_et = dir[0];
                seg.interval_s = dir[1];
                seg.record_words = int(rsize);
                seg.record_count = uint64_t(n);
                seg.degree = int((rsize - 2) / components) - 1;
            }
            out.segments_.push_back(std::move(seg));
        }
        record = int32_t(next);
    }
    out.cached_index_.assign(out.segments_.size(), ~uint64_t{0});
    out.cached_record_.assign(out.segments_.size(), {});
    return out;
}

Result<void> SpkFile::read_words(uint64_t first_word, size_t count,
                                 std::vector<double>& out) const {
    std::string buf(count * kWordBytes, '\0');
    if (!read_at(file_, (first_word - 1) * kWordBytes, buf.data(), buf.size())) {
        return make_error(ErrorCode::CorruptionError, "truncated DAF data at word " +
                                                          std::to_string(first_word) + " in '" +
                                                          path_ + "'");
    }
    out.resize(count);
    for (size_t i = 0; i < count; ++i)
        out[i] = f64_at(buf.data() + i * kWordBytes, swap_);
    return {};
}

Result<void> SpkFile::segment_state_et(size_t index, double et, double out[6]) const {
    if (index >= segments_.size()) {
        return make_error(ErrorCode::ArgumentError,
                          "segment index out of range: " + std::to_string(index));
    }
    const Segment& seg = segments_[index];
    if (seg.type != 2 && seg.type != 3) {
        return make_error(ErrorCode::FormatError,
                          "SPK segment type " + std::to_string(seg.type) + " is not supported");
    }
    if (!std::isfinite(et)) {
        return make_error(ErrorCode::ArgumentError, "epoch is not finite");
    }
    if (et < seg.start_et || et > seg.end_et) {
        return make_error(ErrorCode::ArgumentError, "epoch outside segment coverage for target " +
                                                        std::to_string(seg.target));
    }

    double pos = std::floor((et - seg.init_et) / seg.interval_s);
    if (pos < 0.0)
        pos = 0.0;
    if (pos > double(seg.record_count - 1))
        pos = double(seg.record_count - 1);
    const uint64_t rec = uint64_t(pos);
    std::vector<double>& words = cached_record_[index];
    if (cached_index_[index] != rec) {
        cached_index_[index] = ~uint64_t{0};
        Result<void> r = read_words(seg.begin_word + rec * uint64_t(seg.record_words),
                                    size_t(seg.record_words), words);
        if (!r.ok())
            return r;
        cached_index_[index] = rec;
    }

    // Record: MID, RADIUS, then ncoeff coefficients per component.
    const double mid = words[0], radius = words[1];
    if (!(radius > 0.0)) {
        return make_error(ErrorCode::CorruptionError,
                          "non-positive record radius in segment for target " +
                              std::to_string(seg.target));
    }
    double tau = (et - mid) / radius;
    tau = std::max(-1.0, std::min(1.0, tau));
    const int n = seg.degree + 1;
    for (int c = 0; c < 3; ++c) {
        double value = 0.0, deriv = 0.0;
        chebyshev_eval(&words[2 + size_t(c) * size_t(n)], n, tau, value, deriv);
        out[c] = value;
        if (seg.type == 2) {
            out[3 + c] = deriv / radius * 86400.0; // km/s -> km/day
        } else {
            double v = 0.0, unused = 0.0;
            chebyshev_eval(&words[2 + size_t(3 + c) * size_t(n)], n, tau, v, unused);
            out[3 + c] = v * 86400.0;
        }
    }
    return {};
}

Result<std::vector<double>> SpkFile::segment_records(size_t index, uint64_t first,
                                                     uint64_t count) const {
    if (index >= segments_.size())
        return make_error(ErrorCode::ArgumentError, "segment index out of range");
    const Segment& seg = segments_[index];
    if (seg.type != 2 && seg.type != 3)
        return make_error(ErrorCode::FormatError,
                          "SPK segment type " + std::to_string(seg.type) + " has no records");
    if (first > seg.record_count || count > seg.record_count - first)
        return make_error(ErrorCode::ArgumentError, "record range outside the segment");
    std::vector<double> words;
    if (count == 0)
        return words;
    Result<void> r = read_words(seg.begin_word + first * uint64_t(seg.record_words),
                                size_t(count) * size_t(seg.record_words), words);
    if (!r.ok())
        return r.error();
    return words;
}

Result<std::string> SpkFile::comments() const {
    std::string text;
    for (int32_t rec = 2; rec < forward_; ++rec) {
        char buf[kRecordBytes];
        if (!read_at(file_, uint64_t(rec - 1) * kRecordBytes, buf, kRecordBytes))
            return make_error(ErrorCode::CorruptionError, "truncated DAF comment record");
        for (size_t i = 0; i < 1000; ++i) { // 1000 characters per comment record
            if (buf[i] == '\x04')
                return text;
            text.push_back(buf[i] == '\0' ? '\n' : buf[i]);
        }
    }
    return text;
}

namespace {

void put_le64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(char((v >> (8 * i)) & 0xFF));
}
void put_f64le(std::string& out, double v) {
    put_le64(out, std::bit_cast<uint64_t>(v));
}
void put_i32le(std::string& out, int32_t v) {
    const uint32_t u = uint32_t(v);
    for (int i = 0; i < 4; ++i)
        out.push_back(char((u >> (8 * i)) & 0xFF));
}
void put_text(std::string& out, const std::string& s, size_t n) {
    std::string t = s.substr(0, n);
    t.resize(n, ' ');
    out += t;
}
void pad_record(std::string& out) {
    out.resize((out.size() + kRecordBytes - 1) / kRecordBytes * kRecordBytes, '\0');
}

} // namespace

Result<void> write_spk(const std::string& path, const std::string& internal_name,
                       const std::string& comments, const std::vector<WriteSegment>& segments) {
    constexpr size_t kPerSummaryRecord = 25; // (1024 - 3 * 8) / 40
    if (segments.empty())
        return make_error(ErrorCode::ArgumentError, "no segments to write");
    for (const WriteSegment& s : segments) {
        const int components = s.type == 2 ? 3 : (s.type == 3 ? 6 : 0);
        if (components == 0 || s.record_words < 2 + components ||
            (s.record_words - 2) % components != 0 || s.records.empty() ||
            s.records.size() % size_t(s.record_words) != 0 || !(s.interval_s > 0.0) ||
            !(s.end_et >= s.start_et))
            return make_error(ErrorCode::ArgumentError,
                              "invalid type 2/3 segment for target " + std::to_string(s.target));
    }

    // Comment area: NUL line separators, EOT terminator, 1000 chars/record.
    std::string comment_bytes;
    if (!comments.empty()) {
        for (char c : comments)
            comment_bytes.push_back(c == '\n' ? '\0' : c);
        comment_bytes.push_back('\x04');
    }
    const size_t comment_records = (comment_bytes.size() + 999) / 1000;
    const size_t summary_records = (segments.size() + kPerSummaryRecord - 1) / kPerSummaryRecord;
    const size_t forward = 2 + comment_records;
    const size_t data_record = forward + 2 * summary_records;

    // Data words: every segment's records followed by its 4-word directory.
    std::vector<std::pair<uint64_t, uint64_t>> addresses;
    uint64_t word = uint64_t(data_record - 1) * (kRecordBytes / kWordBytes) + 1;
    std::string data;
    for (const WriteSegment& s : segments) {
        const uint64_t begin = word;
        for (double v : s.records)
            put_f64le(data, v);
        put_f64le(data, s.init_et);
        put_f64le(data, s.interval_s);
        put_f64le(data, double(s.record_words));
        put_f64le(data, double(s.records.size() / size_t(s.record_words)));
        word += uint64_t(s.records.size()) + 4;
        addresses.emplace_back(begin, word - 1);
    }
    if (word - 1 > uint64_t(INT32_MAX))
        return make_error(ErrorCode::ArgumentError, "SPK file too large for DAF addresses");

    std::string out;
    out += "DAF/SPK ";
    put_i32le(out, 2);
    put_i32le(out, 6);
    put_text(out, internal_name, 60);
    put_i32le(out, int32_t(forward));
    put_i32le(out, int32_t(forward + 2 * (summary_records - 1)));
    put_i32le(out, int32_t(word));
    out += "LTL-IEEE";
    out.resize(kFtpOffset, '\0');
    out.append(kFtpString, kFtpBytes);
    pad_record(out);

    for (size_t r = 0; r < comment_records; ++r) {
        std::string rec = comment_bytes.substr(r * 1000, 1000);
        rec.resize(kRecordBytes, '\0');
        out += rec;
    }
    for (size_t r = 0; r < summary_records; ++r) {
        const size_t first = r * kPerSummaryRecord;
        const size_t count = std::min(kPerSummaryRecord, segments.size() - first);
        const size_t this_record = forward + 2 * r;
        put_f64le(out, r + 1 < summary_records ? double(this_record + 2) : 0.0);
        put_f64le(out, r > 0 ? double(this_record - 2) : 0.0);
        put_f64le(out, double(count));
        std::string names;
        for (size_t k = first; k < first + count; ++k) {
            const WriteSegment& s = segments[k];
            put_f64le(out, s.start_et);
            put_f64le(out, s.end_et);
            put_i32le(out, s.target);
            put_i32le(out, s.center);
            put_i32le(out, s.frame);
            put_i32le(out, s.type);
            put_i32le(out, int32_t(addresses[k].first));
            put_i32le(out, int32_t(addresses[k].second));
            put_text(names, s.name, 40);
        }
        pad_record(out);
        names.resize(kRecordBytes, ' ');
        out += names;
    }
    out += data;
    pad_record(out);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return make_error(ErrorCode::IoError, "cannot create '" + path + "'");
    f.write(out.data(), std::streamsize(out.size()));
    if (!f)
        return make_error(ErrorCode::IoError, "write failed for '" + path + "'");
    return {};
}

Result<std::vector<WriteSegment>> trim_segments(const SpkFile& file, double et0, double et1) {
    if (!(et1 > et0))
        return make_error(ErrorCode::ArgumentError, "empty trim span");
    std::vector<WriteSegment> out;
    const auto& segs = file.segments();
    for (size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        if (s.end_et < et0 || s.start_et > et1)
            continue;
        if (s.type != 2 && s.type != 3)
            return make_error(ErrorCode::FormatError,
                              "cannot trim SPK segment type " + std::to_string(s.type));
        const double k0f = std::floor((et0 - s.init_et) / s.interval_s);
        const double k1f = std::ceil((et1 - s.init_et) / s.interval_s);
        const uint64_t k0 = k0f <= 0.0 ? 0 : std::min(uint64_t(k0f), s.record_count);
        const uint64_t k1 = k1f <= 0.0 ? 0 : std::min(uint64_t(k1f), s.record_count);
        if (k1 <= k0)
            continue;
        auto words = file.segment_records(i, k0, k1 - k0);
        if (!words.ok())
            return words.error();
        WriteSegment w;
        w.name = s.name;
        w.target = s.target;
        w.center = s.center;
        w.frame = s.frame;
        w.type = s.type;
        w.init_et = s.init_et + double(k0) * s.interval_s;
        w.interval_s = s.interval_s;
        w.record_words = s.record_words;
        w.start_et = std::max(s.start_et, w.init_et);
        w.end_et = std::min(s.end_et, s.init_et + double(k1) * s.interval_s);
        w.records = std::move(words).value();
        out.push_back(std::move(w));
    }
    return out;
}

long SpkFile::find_segment(int body, double et) const {
    for (size_t i = segments_.size(); i-- > 0;) {
        const Segment& s = segments_[i];
        if (s.target == body && et >= s.start_et && et <= s.end_et &&
            (s.type == 2 || s.type == 3)) {
            return long(i);
        }
    }
    return -1;
}

// chain[k] is the k-th ancestor of body (chain[0] = body); states holds,
// for each, body's state relative to that ancestor (zeros for k = 0).
Result<void> SpkFile::state_to_root(int body, double et, std::vector<int>& chain,
                                    std::vector<double>& states) const {
    chain.assign(1, body);
    states.assign(6, 0.0);
    int current = body;
    for (int depth = 0; depth < kMaxChainDepth; ++depth) {
        const long seg = find_segment(current, et);
        if (seg < 0) {
            // A body with segments, none of them evaluable at et, is an
            // error rather than a tree root.
            bool listed = false;
            for (const Segment& s : segments_) {
                if (s.target != current)
                    continue;
                if (et >= s.start_et && et <= s.end_et) {
                    return make_error(ErrorCode::FormatError,
                                      "SPK segment type " + std::to_string(s.type) + " for body " +
                                          std::to_string(current) + " is not supported");
                }
                listed = true;
            }
            if (listed) {
                return make_error(ErrorCode::ArgumentError,
                                  "epoch outside SPK coverage for body " + std::to_string(current));
            }
            return {};
        }
        double s[6];
        Result<void> r = segment_state_et(size_t(seg), et, s);
        if (!r.ok())
            return r;
        const size_t prev = states.size() - 6;
        for (int i = 0; i < 6; ++i)
            states.push_back(states[prev + size_t(i)] + s[i]);
        current = segments_[size_t(seg)].center;
        if (std::find(chain.begin(), chain.end(), current) != chain.end()) {
            return make_error(ErrorCode::CorruptionError,
                              "cyclic SPK segment chain at body " + std::to_string(current));
        }
        chain.push_back(current);
    }
    return make_error(ErrorCode::CorruptionError,
                      "SPK segment chain deeper than " + std::to_string(kMaxChainDepth));
}

Result<void> SpkFile::state_et(int target, int center, double et, double out[6]) const {
    if (!std::isfinite(et)) {
        return make_error(ErrorCode::ArgumentError, "epoch is not finite");
    }
    std::vector<int> tc, cc;
    std::vector<double> ts, cs;
    Result<void> r = state_to_root(target, et, tc, ts);
    if (!r.ok())
        return r;
    r = state_to_root(center, et, cc, cs);
    if (!r.ok())
        return r;
    for (size_t i = 0; i < tc.size(); ++i) {
        const auto hit = std::find(cc.begin(), cc.end(), tc[i]);
        if (hit == cc.end())
            continue;
        const size_t j = size_t(hit - cc.begin());
        for (int k = 0; k < 6; ++k)
            out[k] = ts[i * 6 + size_t(k)] - cs[j * 6 + size_t(k)];
        return {};
    }
    return make_error(ErrorCode::NotFound, "no SPK segment path from body " +
                                               std::to_string(target) + " to body " +
                                               std::to_string(center) + " at the requested epoch");
}

} // namespace prometheia::spk
