// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Offline comparison engine. Compares two traces (or replayed sessions) and
// produces a labeled report of measured deltas, derived statistics, and heuristic
// interpretations. It never declares a universal winner.
//
// Comparison is deterministic: identical inputs produce identical output.

#ifndef MEMORY_OBSERVATORY_COMPARE_HPP
#define MEMORY_OBSERVATORY_COMPARE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/aggregate.h"
#include "memory_observatory/domain.h"
#include "memory_observatory/error.h"
#include "memory_observatory/trace.h"

namespace memory_observatory {

// Epistemic kind of a comparison finding.
enum class CompareKind : std::int32_t {
  kMeasured = 0,
  kDerived,
  kHeuristic,
};

inline const char* to_string(CompareKind k) noexcept {
  switch (k) {
    case CompareKind::kMeasured: return "measured";
    case CompareKind::kDerived: return "derived";
    case CompareKind::kHeuristic: return "heuristic";
  }
  return "measured";
}

// A labeled comparison finding (measured / derived / heuristic).
struct ComparisonFinding {
  CompareKind kind = CompareKind::kDerived;
  std::string category;
  std::string detail;
  friend bool operator==(const ComparisonFinding&, const ComparisonFinding&) = default;
};

// A metric-level diff for one MemoryKind.
struct MetricDiff {
  MemoryKind kind = MemoryKind::kUnknown;
  Scale scale = Scale::kBytes;
  std::uint64_t left_count = 0;
  std::uint64_t right_count = 0;
  std::optional<double> left_min, right_min;
  std::optional<double> left_max, right_max;
  std::optional<double> left_mean, right_mean;
  std::optional<double> left_p50, right_p50;
  std::optional<double> left_p95, right_p95;
  std::optional<double> left_p99, right_p99;
  std::optional<double> mean_delta;
  friend bool operator==(const MetricDiff&, const MetricDiff&) = default;
};

// Offline comparison of two traces.
struct ComparisonReport {
  bool identical_inputs = false;
  std::string left_label = "left";
  std::string right_label = "right";

  std::uint64_t left_observations = 0;
  std::uint64_t right_observations = 0;
  std::uint64_t left_events = 0;
  std::uint64_t right_events = 0;
  std::uint64_t left_dropped = 0;
  std::uint64_t right_dropped = 0;
  std::uint64_t left_rejected = 0;
  std::uint64_t right_rejected = 0;

  std::vector<MetricDiff> metrics;

  std::vector<std::string> domains_only_left;
  std::vector<std::string> domains_only_right;

  std::vector<std::string> diagnostics_only_left;
  std::vector<std::string> diagnostics_only_right;
  std::vector<std::string> diagnostics_severity_changed;

  std::vector<std::string> provenance_diff;

  std::vector<ComparisonFinding> findings;

  // source health counts
  std::uint64_t left_source_healthy = 0;
  std::uint64_t right_source_healthy = 0;
};

// Build a comparison of two traces. Deterministic for identical inputs.
Result<ComparisonReport> compare_traces(const Trace& left, const Trace& right);

// Render the comparison in text / JSON / CSV. Pure; never mutates state.
std::string render_compare_text(const ComparisonReport& report);
std::string render_compare_json(const ComparisonReport& report);
std::string render_compare_csv(const ComparisonReport& report);

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_COMPARE_HPP
