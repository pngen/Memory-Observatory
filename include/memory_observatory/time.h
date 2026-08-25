// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Time model.
//
// Memory Observatory separates two clock domains:
//   * Monotonic time -- a steady, non-adjustable clock used for all ordering,
//     duration, and rate calculations. Never derived from wall-clock values,
//     so a wall-clock step (NTP, manual change, suspend) cannot corrupt a
//     duration or rate.
//   * Wall time -- a calendar timestamp used only for human correlation and
//     display. Never used for duration math.
//
// A "metric timestamp" is always monotonic. Wall time rides along as optional
// metadata for humans. Source-local ordering is expressed with SequenceNo.
//
// All timestamps are stored as signed 64-bit nanosecond counts (the unit is
// fixed, deterministic, and independent of any platform clock resolution).

#ifndef MEMORY_OBSERVATORY_TIME_HPP
#define MEMORY_OBSERVATORY_TIME_HPP

#include <chrono>
#include <cstdint>
#include <ostream>

namespace memory_observatory {

// A nanosecond duration, independent of any clock.
class Duration {
 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(std::int64_t ns) noexcept : ns_(ns) {}

  [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept {
    return ns_;
  }
  [[nodiscard]] constexpr double seconds() const noexcept {
    return static_cast<double>(ns_) / 1e9;
  }
  [[nodiscard]] constexpr bool zero() const noexcept { return ns_ == 0; }
  [[nodiscard]] constexpr bool negative() const noexcept { return ns_ < 0; }

  constexpr bool operator==(const Duration&) const noexcept = default;
  constexpr auto operator<=>(const Duration&) const noexcept = default;

  constexpr Duration operator+(const Duration& o) const noexcept {
    return Duration{ns_ + o.ns_};
  }
  constexpr Duration operator-(const Duration& o) const noexcept {
    return Duration{ns_ - o.ns_};
  }
  constexpr Duration& operator+=(const Duration& o) noexcept {
    ns_ += o.ns_;
    return *this;
  }
  constexpr Duration& operator-=(const Duration& o) noexcept {
    ns_ -= o.ns_;
    return *this;
  }

  friend std::ostream& operator<<(std::ostream& os, const Duration& d) {
    os << d.nanoseconds() << "ns";
    return os;
  }

 private:
  std::int64_t ns_ = 0;
};

constexpr Duration operator""_ns(unsigned long long v) {
  return Duration{static_cast<std::int64_t>(v)};
}
constexpr Duration operator""_us(unsigned long long v) {
  return Duration{static_cast<std::int64_t>(v) * 1000};
}
constexpr Duration operator""_ms(unsigned long long v) {
  return Duration{static_cast<std::int64_t>(v) * 1000000};
}
constexpr Duration operator""_s(unsigned long long v) {
  return Duration{static_cast<std::int64_t>(v) * 1000000000LL};
}

// Monotonic timestamp: nanosecond counts on a steady clock. Used for all
// ordering and duration math. Callers use now() for the present instant.
class MonotonicTimestamp {
 public:
  constexpr MonotonicTimestamp() noexcept = default;
  constexpr explicit MonotonicTimestamp(std::int64_t ns) noexcept : ns_(ns) {}

  [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept {
    return ns_;
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return ns_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return valid();
  }

  constexpr bool operator==(const MonotonicTimestamp&) const noexcept =
      default;
  constexpr auto operator<=>(const MonotonicTimestamp&) const noexcept =
      default;

  MonotonicTimestamp& operator+=(const Duration& d) noexcept {
    ns_ += d.nanoseconds();
    return *this;
  }
  MonotonicTimestamp& operator-=(const Duration& d) noexcept {
    ns_ -= d.nanoseconds();
    return *this;
  }

  friend MonotonicTimestamp operator+(MonotonicTimestamp t, const Duration& d) {
    t += d;
    return t;
  }
  friend MonotonicTimestamp operator-(MonotonicTimestamp t, const Duration& d) {
    t -= d;
    return t;
  }
  friend Duration operator-(const MonotonicTimestamp& a,
                            const MonotonicTimestamp& b) {
    return Duration{a.ns_ - b.ns_};
  }

  // Returns the current monotonic instant on the steady clock.
  static MonotonicTimestamp now() noexcept {
    using namespace std::chrono;
    const auto n = steady_clock::now().time_since_epoch().count();
    return MonotonicTimestamp{static_cast<std::int64_t>(n)};
  }

  friend std::ostream& operator<<(std::ostream& os,
                                  const MonotonicTimestamp& t) {
    os << t.nanoseconds();
    return os;
  }

 private:
  std::int64_t ns_ = 0;
};

// Wall-clock timestamp: nanoseconds since the Unix epoch (1970-01-01T00:00:00Z).
// Display/human correlation only -- never used for duration math.
class WallTimestamp {
 public:
  constexpr WallTimestamp() noexcept = default;
  constexpr explicit WallTimestamp(std::int64_t ns_since_epoch) noexcept
      : ns_(ns_since_epoch) {}

  [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept {
    return ns_;
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return ns_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return valid();
  }

  constexpr bool operator==(const WallTimestamp&) const noexcept = default;
  constexpr auto operator<=>(const WallTimestamp&) const noexcept = default;

  static WallTimestamp now() noexcept {
    using namespace std::chrono;
    const auto n = system_clock::now().time_since_epoch().count();
    return WallTimestamp{static_cast<std::int64_t>(n)};
  }

  friend std::ostream& operator<<(std::ostream& os, const WallTimestamp& t) {
    os << t.nanoseconds();
    return os;
  }

 private:
  std::int64_t ns_ = 0;
};

// Source-local ordering: a per-producer counter that gives total order within
// a single producer without depending on clocks.
class SequenceNo {
 public:
  constexpr SequenceNo() noexcept = default;
  constexpr explicit SequenceNo(std::int64_t v) noexcept : v_(v) {}

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return v_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return v_ != 0; }
  constexpr bool operator==(const SequenceNo&) const noexcept = default;
  constexpr auto operator<=>(const SequenceNo&) const noexcept = default;

  SequenceNo& operator++() noexcept {
    ++v_;
    return *this;
  }
  SequenceNo operator++(int) noexcept {
    SequenceNo old = *this;
    ++v_;
    return old;
  }

 private:
  std::int64_t v_ = 0;
};

// A generation is a monotonic counter owned by a source. It is advanced on
// source reset / restart, letting observers distinguish "the same source
// before and after a restart". Generations do not imply a global epoch.
class Generation {
 public:
  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t v) noexcept : v_(v) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return v_; }
  constexpr bool operator==(const Generation&) const noexcept = default;
  constexpr auto operator<=>(const Generation&) const noexcept = default;

  Generation& operator++() noexcept {
    ++v_;
    return *this;
  }

 private:
  std::uint64_t v_ = 0;
};

// An epoch is an OBSERVATORY-WIDE counter advanced when the global ingestion
// ordering volume is intentionally reset (e.g. after a trace replay boundary or
// a session reset). Distinct from a per-source Generation.
class Epoch {
 public:
  constexpr Epoch() noexcept = default;
  constexpr explicit Epoch(std::uint64_t v) noexcept : v_(v) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return v_; }
  constexpr bool operator==(const Epoch&) const noexcept = default;
  constexpr auto operator<=>(const Epoch&) const noexcept = default;

 private:
  std::uint64_t v_ = 0;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_TIME_HPP
