// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Structured errors. Memory Observatory does not throw across public API
// boundaries: operations that can fail return Result<T>. There is no hidden
// global error state.

#ifndef MEMORY_OBSERVATORY_ERROR_HPP
#define MEMORY_OBSERVATORY_ERROR_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace memory_observatory {

enum class ErrorCode : std::int32_t {
  kNone = 0,
  kInvalidArgument,
  kOutOfRange,
  kBoundExceeded,
  kInvalidState,
  kNotInitialized,
  kProviderUnavailable,
  kBackendUnavailable,
  kProviderError,
  kNotImplemented,
  kMalformedInput,
  kTruncatedInput,
  kCorruption,
  kInvalidEnum,
  kInvalidTimestamp,
  kNegativeCapacity,
  kImpossibleAccounting,
  kOverflow,
  kAllocationFailure,
  kUnsupportedVersion,
  kSourceNotFound,
  kDuplicateSource,
  kQueueFull,
  kQueueOverflow,
  kNoBackend,
  kUnknownField,
  kIoError,
  kIncompatibleVersion,
  kSessionError,
  kInvalidTrace,
  kShutdown,
  kTimeout,
  kRejected,
  kInternal,
};

// Human-readable, stable string for an error code. Used by the CLI and by
// reports. Not a locale-aware message; callers may localize independently.
const char* to_string(ErrorCode code) noexcept;

// A structured, value-error. Carries a code and a short, human-readable
// message. Will never carry a raw pointer or a platform handle.
class Error {
 public:
  Error() noexcept = default;
  Error(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::int32_t code_value() const noexcept {
    return static_cast<std::int32_t>(code_);
  }
  [[nodiscard]] const std::string& message() const noexcept {
    return message_;
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kNone; }
  [[nodiscard]] bool failed() const noexcept { return !ok(); }

  // Builds a convenience error from a code and a printf-free message.
  static Error make(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
  }

  friend bool operator==(const Error&, const Error&) = default;

 private:
  ErrorCode code_ = ErrorCode::kNone;
  std::string message_;
};

// A result that is either a value or a structured error. No exceptions cross
// the public API boundary; every fallible operation returns Result<T>.
template <typename T>
class Result {
 public:
  Result() = default;

  Result(const T& value) : value_(value) {}
  Result(T&& value) : value_(std::move(value)) {}

  // Implicitly construct a failed result from an Error.
  Result(const Error& error) : error_(error) {}
  Result(Error&& error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool failed() const noexcept { return !value_.has_value(); }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] const T& error_value() const& { return *value_; }
  [[nodiscard]] const Error& error() const& { return error_; }

  // Returns the value or a default if failed.
  [[nodiscard]] T value_or(const T& fallback) const& {
    return value_ ? *value_ : fallback;
  }
  [[nodiscard]] T value_or(T&& fallback) const& {
    return value_ ? *value_ : std::move(fallback);
  }

  // Expected-style access: throws std::logic_error only if the caller
  // contracts a precondition the value violates (accessing value() when
  // failed). This is the single sanctioned exception and is documented; it
  // does not cross an API boundary unless the caller deliberately triggers it.
  T& operator*() const& {
    if (!value_) {
      // Deliberately unreachable under correct use.
      throw std::runtime_error("Result::operator*: access on failed Result");
    }
    return *const_cast<T*>(&*value_);
  }

  // Forwards through using Result as a monadic value.
  template <typename F>
  auto map(F&& f) const& {
    using U = decltype(f(*value_));
    if (value_) {
      return Result<U>{f(*value_)};
    }
    return Result<U>{error_};
  }

 private:
  std::optional<T> value_;
  Error error_;
};

// Factory helpers to preserve intent at call sites.
template <typename T>
Result<T> ok(T value) {
  return Result<T>{std::move(value)};
}

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

// Helper producing a failed Result<T> of a given code/message.
template <typename T>
Result<T> fail(ErrorCode code, std::string message) {
  return Result<T>{Error{code, std::move(message)}};
}

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_ERROR_HPP
