// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROMETHEIA_ERROR_HPP
#define PROMETHEIA_ERROR_HPP

#include <string>
#include <utility>

namespace prometheia {

enum class ErrorCode {
  Ok = 0,
  IoError,         // open/read/write/seek failed
  FormatError,     // container is not EPM1 or structurally invalid
  CorruptionError, // checksum mismatch or truncation mid-structure
  ArgumentError,   // caller passed invalid input (unsorted, non-finite, ...)
  NotFound,        // requested body is not in the catalog
};

struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(std::move(error)) {}
  bool ok() const { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const { return ok(); }
  const T& value() const& { return value_; }
  T& value() & { return value_; }
  T&& value() && { return std::move(value_); }
  const Error& error() const { return error_; }

 private:
  T value_{};
  Error error_{};
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}
  bool ok() const { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return error_; }

 private:
  Error error_{};
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

}  // namespace prometheia

#endif  // PROMETHEIA_ERROR_HPP
