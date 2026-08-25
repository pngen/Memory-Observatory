// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Report model. A Report is a pure, self-contained value: it captures a
// session, its environment, providers, domains, counts, metric summaries,
// diagnostics, correlations, a timeline excerpt, provenance, and explicit
// limitations. Rendering to text/JSON/CSV never mutates Observatory state.

#ifndef MEMORY_OBSERVATORY_REPORT_HPP
#define MEMORY_OBSERVATORY_REPORT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/aggregate.h"
#include "memory_observatory/correlation.h"
#include "memory_observatory/counters.h"
#include "memory_observatory/diagnostic.h"
#include "memory_observatory/domain.h"
#include "memory_observatory/provenance.h"
#include "memory_observatory/source.h"
#include "memory_observatory/timeline.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

enum class ReportFormat : std::int32_t {
  kText = 0,
  kJson,
  kCsv,
};

inline const char* to_string(ReportFormat f) noexcept {
  switch (f) {
    case ReportFormat::kText: return "text";
    case ReportFormat::kJson: return "json";
    case ReportFormat::kCsv: return "csv";
  }
  return "text";
}

// A named aggregate over one metric kind.
struct MetricSummary {
  MemoryKind kind = MemoryKind::kUnknown;
  Scale scale = Scale::kUnknown;
  Aggregate aggregate;
  Percentiles percentiles;
  std::optional<Rate> rate;
  std::uint64_t sample_count = 0;
  friend bool operator==(const MetricSummary&, const MetricSummary&) = default;
};

// Sampling engine health.
struct SamplingHealth {
  bool active = false;
  std::uint64_t samples_taken = 0;
  std::uint64_t provider_failures = 0;
  std::uint64_t provider_recoveries = 0;
  double mean_jitter_ms = 0.0;
  friend bool operator==(const SamplingHealth&, const SamplingHealth&) = default;
};

// Experiment / replay metadata carried in a report.
struct ReplayInfo {
  bool is_replay = false;
  bool is_experiment = false;
  std::string label;
  std::optional<TraceId> trace;
  std::string seed;
  friend bool operator==(const ReplayInfo&, const ReplayInfo&) = default;
};

// A machine-readable, self-contained session report.
struct Report {
  std::string title;
  std::string version;
  SessionId session;
  MonotonicTimestamp generated_at;
  std::optional<WallTimestamp> wall;

  std::string os;
  std::string architecture;
  std::string toolchain;
  std::string runtime_version;

  std::vector<SourceInfo> sources;
  std::vector<MemoryDomain> domains;
  AccountingCounters counters;
  std::uint64_t retained = 0;

  std::vector<MetricSummary> metric_summaries;
  SamplingHealth sampling;
  std::vector<Diagnostic> diagnostics;
  std::vector<Correlation> correlations;
  Timeline timeline_excerpt;
  std::vector<Provenance> provenance;
  ReplayInfo replay;
  std::vector<std::string> limitations;
};

// Renderers are pure functions: they return text and never mutate state.
std::string render_report_text(const Report& report);
std::string render_report_json(const Report& report);
std::string render_report_csv(const Report& report);

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_REPORT_HPP
