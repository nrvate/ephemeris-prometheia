// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheiad's log: one line per event, timestamped in UTC, at a level the
// operator chooses. Lines carry a connection id ("c=<loop>.<n>") and a
// request id ("req=<id>") so one request can be followed from open to close
// (docs/SERVER.md, "Logging").
//
// What a line never carries: request instants, observer sites, tokens, or
// query strings. A request is someone's birth data; the log says how many
// objects and rows, which codes and how long, not when or where. This is
// the same rule the protocol sets for ERROR text (3.8).
#ifndef PROMETHEIA_SERVER_LOG_HPP
#define PROMETHEIA_SERVER_LOG_HPP

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace prometheia::server {

enum class LogLevel : int {
    Quiet = 0, // nothing but startup and fatal lines, which bypass the log
    Info = 1,  // connections, HELLOs, requests, errors: one line per event
    Debug = 2, // also what is routine: PINGs, CANCELs of finished requests
};

// "quiet", "info" or "debug"; nothing for anything else.
std::optional<LogLevel> parse_log_level(std::string_view s);

class Log {
public:
    explicit Log(LogLevel level = LogLevel::Info, std::FILE* out = stderr)
        : level_(level), out_(out) {}

    bool enabled(LogLevel l) const { return out_ && int(l) <= int(level_) && l != LogLevel::Quiet; }
    LogLevel level() const { return level_; }

    // One line: "prometheiad <UTC time> <message>". Formatted whole and
    // written with one call, so lines from different loops never interleave.
    void write(LogLevel l, const char* fmt, ...) const __attribute__((format(printf, 3, 4)));

private:
    LogLevel level_;
    std::FILE* out_;
};

// Text made safe for a log line: printable ASCII only, quotes and
// backslashes replaced, at most `max` characters (a client's name gets 64).
std::string log_safe(std::string_view s, size_t max = 64);

// A peer address as uWS gives it, with an IPv4-mapped IPv6 address
// ("0000:...:ffff:7f00:0001") shown as the IPv4 address it is.
std::string log_addr(std::string_view addr);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_LOG_HPP
