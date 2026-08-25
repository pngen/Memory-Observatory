// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Accounting counters. These satisfy the accounting invariant:
//   ingested_total == retained + dropped_overflow + backpressured
//                    + rejected_* + duplicate_suppressed
// Nothing is ever silently discarded.

#ifndef MEMORY_OBSERVATORY_COUNTERS_HPP
#define MEMORY_OBSERVATORY_COUNTERS_HPP

#include <cstdint>

namespace memory_observatory {

struct AccountingCounters {
  std::uint64_t ingested_total = 0;
  std::uint64_t retained = 0;
  std::uint64_t rejected_malformed = 0;
  std::uint64_t rejected_invalid = 0;
  std::uint64_t rejected_stale = 0;
  std::uint64_t rejected_out_of_order = 0;
  std::uint64_t dropped_overflow = 0;
  std::uint64_t duplicate_suppressed = 0;
  std::uint64_t backpressured = 0;
  // Observations that were accepted and retained, then evicted because the
  // bounded history capacity was reached. These were accepted, not dropped.
  std::uint64_t evicted = 0;

  // Total observations that entered the pipeline (a Tally of accepted + all
  // non-accepted dispositions).
  [[nodiscard]] std::uint64_t total_touched() const noexcept {
    return ingested_total;
  }

  // Sum of all non-accepted dispositions that were counted as "handled".
  // Accepted observations are those either still retained or already evicted.
  [[nodiscard]] std::uint64_t total_accepted() const noexcept {
    return retained + evicted;
  }

  // Sum of all non-accepted dispositions that were counted as "handled".
  [[nodiscard]] std::uint64_t total_non_accepted() const noexcept {
    return rejected_malformed + rejected_invalid + rejected_stale +
           rejected_out_of_order + dropped_overflow + duplicate_suppressed +
           backpressured;
  }

  // The fundamental accounting invariant: every observation that entered the
  // pipeline is accounted for as either accepted (retained or evicted) or
  // rejected/dropped.
  [[nodiscard]] bool is_balanced() const noexcept {
    return ingested_total == total_accepted() + total_non_accepted();
  }

  friend bool operator==(const AccountingCounters&,
                         const AccountingCounters&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_COUNTERS_HPP
