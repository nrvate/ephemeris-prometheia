// SPDX-License-Identifier: GPL-2.0-or-later
//
// Positional file reads (prometheia/io.hpp).
#include "prometheia/io.hpp"

#include <algorithm>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace prometheia::io {

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept
#if defined(_WIN32)
    : handle_(std::exchange(other.handle_, nullptr)),
#else
    : fd_(std::exchange(other.fd_, -1)),
#endif
      size_(std::exchange(other.size_, 0)) {
}

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
    if (this != &other) {
        close();
#if defined(_WIN32)
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

RandomAccessFile::~RandomAccessFile() {
    close();
}

bool RandomAccessFile::read_exact(uint64_t offset, void* buf, size_t n) const {
    auto r = read_at(offset, buf, n);
    return r.ok() && r.value() == n;
}

#if defined(_WIN32)

Result<RandomAccessFile> RandomAccessFile::open(const std::string& path) {
    // The path is UTF-8; the wide API opens any name the ANSI one cannot.
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                         int(path.size()), nullptr, 0);
    if (wlen <= 0 && !path.empty())
        return make_error(ErrorCode::IoError, "cannot open '" + path + "': not UTF-8");
    std::wstring wpath(size_t(wlen), L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), int(path.size()),
                            wpath.data(), wlen);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return make_error(ErrorCode::IoError, "cannot open '" + path + "'");
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return make_error(ErrorCode::IoError, "cannot read the size of '" + path + "'");
    }
    RandomAccessFile f;
    f.handle_ = h;
    f.size_ = uint64_t(size.QuadPart);
    return f;
}

bool RandomAccessFile::is_open() const {
    return handle_ != nullptr;
}

void RandomAccessFile::close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

Result<size_t> RandomAccessFile::read_at(uint64_t offset, void* buf, size_t n) const {
    if (!handle_)
        return make_error(ErrorCode::IoError, "read from a closed file");
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        const uint64_t at = offset + done;
        ov.Offset = DWORD(at & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(at >> 32);
        const DWORD want = DWORD(std::min<size_t>(n - done, 0x40000000u)); // 1 GiB a call
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), static_cast<char*>(buf) + done, want, &got,
                      &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF)
                break;
            return make_error(ErrorCode::IoError, "read failed");
        }
        if (got == 0)
            break; // the end of the file
        done += got;
    }
    return done;
}

#else

Result<RandomAccessFile> RandomAccessFile::open(const std::string& path) {
    int fd;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return make_error(ErrorCode::IoError,
                          "cannot open '" + path + "': " + std::strerror(errno));
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return make_error(ErrorCode::IoError,
                          "cannot read the size of '" + path + "': " + std::strerror(err));
    }
    RandomAccessFile f;
    f.fd_ = fd;
    f.size_ = uint64_t(st.st_size);
    return f;
}

bool RandomAccessFile::is_open() const {
    return fd_ >= 0;
}

void RandomAccessFile::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<size_t> RandomAccessFile::read_at(uint64_t offset, void* buf, size_t n) const {
    if (fd_ < 0)
        return make_error(ErrorCode::IoError, "read from a closed file");
    static_assert(sizeof(off_t) >= 8, "64-bit file offsets are required");
    size_t done = 0;
    while (done < n) {
        const ssize_t got =
            ::pread(fd_, static_cast<char*>(buf) + done, n - done, off_t(offset + done));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return make_error(ErrorCode::IoError,
                              std::string("read failed: ") + std::strerror(errno));
        }
        if (got == 0)
            break; // the end of the file
        done += size_t(got);
    }
    return done;
}

#endif

} // namespace prometheia::io
