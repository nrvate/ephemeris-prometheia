// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <ctime>

namespace prometheia::server {

std::optional<LogLevel> parse_log_level(std::string_view s) {
    if (s == "quiet") {
        return LogLevel::Quiet;
    }
    if (s == "info") {
        return LogLevel::Info;
    }
    if (s == "debug") {
        return LogLevel::Debug;
    }
    return std::nullopt;
}

namespace {

void emit(std::FILE* out, const char* fmt, va_list ap) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const long ms =
        long(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
             1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char line[1024];
    int n = std::snprintf(line, sizeof(line), "prometheiad %04d-%02d-%02dT%02d:%02d:%02d.%03ldZ ",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                          tm.tm_sec, ms);
    const int m = std::vsnprintf(line + n, sizeof(line) - size_t(n) - 1, fmt, ap);
    n = (m < 0) ? n : std::min<int>(n + m, int(sizeof(line)) - 2);
    line[n] = '\n';
    line[n + 1] = '\0';
    std::fputs(line, out);
    std::fflush(out);
}

} // namespace

void Log::write(LogLevel l, const char* fmt, ...) const {
    if (!enabled(l)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    emit(out_, fmt, ap);
    va_end(ap);
}

void Log::always(const char* fmt, ...) const {
    if (!out_) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    emit(out_, fmt, ap);
    va_end(ap);
}

std::string log_addr(std::string_view addr) {
    constexpr std::string_view kMapped = "0000:0000:0000:0000:0000:ffff:";
    if (addr.size() == kMapped.size() + 9 && addr.substr(0, kMapped.size()) == kMapped &&
        addr[kMapped.size() + 4] == ':') {
        unsigned hi = 0, lo = 0;
        if (std::sscanf(std::string(addr.substr(kMapped.size())).c_str(), "%4x:%4x", &hi, &lo) ==
            2) {
            return std::to_string(hi >> 8) + "." + std::to_string(hi & 0xff) + "." +
                   std::to_string(lo >> 8) + "." + std::to_string(lo & 0xff);
        }
    }
    return std::string(addr);
}

std::string log_safe(std::string_view s, size_t max) {
    std::string out;
    for (const char c : s.substr(0, max)) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((u < 0x20 || u > 0x7e || c == '"' || c == '\\') ? '?' : c);
    }
    return out;
}

} // namespace prometheia::server
