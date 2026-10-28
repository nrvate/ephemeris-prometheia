// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia::io — a file read at explicit offsets, for the DE, SPK and
// catalog readers. POSIX: open(2) and pread(2), retried on EINTR, with
// 64-bit offsets. Windows: CreateFileW on the UTF-8 path widened (so a path
// outside the ANSI code page opens) and ReadFile with an OVERLAPPED offset.
// No file position is kept, so one file may be read from several threads at
// once; each read is complete in itself.
#ifndef PROMETHEIA_IO_HPP
#define PROMETHEIA_IO_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "prometheia/error.hpp"

namespace prometheia::io {

class RandomAccessFile {
public:
    // IoError when the file cannot be opened or its size read.
    static Result<RandomAccessFile> open(const std::string& path);

    RandomAccessFile() = default;
    RandomAccessFile(RandomAccessFile&& other) noexcept;
    RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    ~RandomAccessFile();

    bool is_open() const;
    uint64_t size() const { return size_; } // at open

    // Reads n bytes at offset into buf. Returns how many were read: n, or
    // fewer only at the end of the file. IoError on a failed read.
    Result<size_t> read_at(uint64_t offset, void* buf, size_t n) const;
    // True when exactly n bytes were read.
    bool read_exact(uint64_t offset, void* buf, size_t n) const;

private:
    void close();
#if defined(_WIN32)
    void* handle_ = nullptr; // a HANDLE; nullptr when closed
#else
    int fd_ = -1;
#endif
    uint64_t size_ = 0;
};

} // namespace prometheia::io

#endif // PROMETHEIA_IO_HPP
