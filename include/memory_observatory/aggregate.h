// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Aggregation primitives: windows, aggregates, rates, deltas, histograms, and
// percentiles. The aggregation engine uses numerically safe arithmetic,
// detects overflow, and explicitly distinguishes a counter reset from a huge
// negative delta.

#ifndef MEMORY_OBSERVATORY_AGGREGATE_HPP
#define MEMORY_OBSERVATORY_AGGREGATE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "memory_observatory/time.h"

namespace memory_observatory {

// A percentile summary over a set of rates/values. Each is optional.
struct Percentiles {
  std::optional<double> p50;
  std::optional<double> p90;
  std::optional<double> p95;
  std::optional<double> p99;

  [[nodiscard]] bool empty() const noexcept {
    return !p50 && !p90 && !p95 && !p99;
  }
  friend bool operator==(const Percentiles&, const Percentiles&) = default;
};

// A fixed-width histogram over an arbitrary numeric domain. Buckets are
// ascending boundaries; counts buckets[i] covers [buckets[i], buckets[i+1]).
struct Histogram {
  std::vector<double> buckets;
  std::vector<std::uint64_t> counts;
  std::optional<double> min;
  std::optional<double> max;

  [[nodiscard]] bool empty() const noexcept {
    return buckets.empty() || counts.empty();
  }
  friend bool operator==(const Histogram&, const Histogram&) = default;
};

// A basic aggregate over a window.
struct Aggregate {
  std::optional<double> min;
  std::optional<double> max;
  std::optional<double> mean;
  std::optional<double> sum;
  std::optional<std::uint64_t> count;

  [[nodiscard]] bool empty() const noexcept {
    return !min && !max && !mean && !sum && !count;
  }
  friend bool operator==(const Aggregate&, const Aggregate&) = default;
};

// A rate (a per-second value). Optional keeps "unknown" unknown.
struct Rate {
  std::optional<double> per_second;
  Duration over;

  friend bool operator==(const Rate&, const Rate&) = default;
};

// A delta between two observations. `reset` records that the underlying counter
// was reset so callers never treat a wrap-around as an enormous delta.
struct Delta {
  std::optional<double> value;
  bool reset = false;

  friend bool operator==(const Delta&, const Delta&) = default;
};

// A time window over the monotonic domain. All queries over history use a
// window; a window is always bounded.
struct Window {
  MonotonicTimestamp begin;
  Duration length;

  [[nodiscard]] MonotonicTimestamp end() const noexcept {
    return begin + length;
  }
  [[nodiscard]] bool contains(MonotonicTimestamp t) const noexcept {
    return t >= begin && t < end();
  }
  friend bool operator==(const Window&, const Window&) = default;
};

// Compute percentiles from an ordered or unordered vector of samples.
// Non-finite samples are ignored (treated as unknown) and callers should not
// use a percentile over a near-empty set. Returns an empty Percentiles when
// there is no usable data.
inline Percentiles compute_percentiles(std::vector<double> samples) {
  Percentiles out;
  std::vector<double> clean;
  clean.reserve(samples.size());
  for (double s : samples) {
    if (std::isfinite(s)) {
      clean.push_back(s);
    }
  }
  if (clean.empty()) {
    return out;
  }
  std::sort(clean.begin(), clean.end());
  const auto at = [&](double q) -> double {
    const auto denom = static_cast<double>(clean.size() - 1);
    const double pos = q * denom;
    const auto idx = static_cast<std::size_t>(
        pos > denom ? denom : pos);
    return clean[idx];
  };
  out.p50 = at(0.50);
  out.p90 = at(0.90);
  out.p95 = at(0.95);
  out.p99 = at(0.99);
  return out;
}

// A bounded, numerically safe online aggregator. It accumulates min/max/sum/count
// and renders an Aggregate. NaN/Inf inputs are ignored, mirroring "unknown stays
// unknown" and guarding against corrupt payloads.
class BoundedAggregator {
 public:
  void add(double v) noexcept {
    if (!std::isfinite(v)) {
      return;
    }
    if (!count_) {
      min_ = v;
      max_ = v;
      count_ = 1;
    } else {
      if (v < *min_) {
        min_ = v;
      }
      if (v > *max_) {
        max_ = v;
      }
      ++*count_;
    }
    // Accumulate sum with Kahan-style compensation to reduce drift.
    const double y = v - compensation_;
    const double t = sum_ + y;
    compensation_ = (t - sum_) - y;
    sum_ = t;
  }

  [[nodiscard]] Aggregate to_aggregate() const noexcept {
    Aggregate a;
    if (count_ && *count_ > 0) {
      a.min = min_;
      a.max = max_;
      a.sum = sum_;
      a.count = count_;
      a.mean = sum_ / static_cast<double>(*count_);
    }
    return a;
  }

  [[nodiscard]] std::uint64_t count() const noexcept {
    return count_ ? *count_ : 0;
  }

 private:
  std::optional<double> min_;
  std::optional<double> max_;
  double sum_ = 0.0;
  double compensation_ = 0.0;
  std::optional<std::uint64_t> count_;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_AGGREGATE_HPP
