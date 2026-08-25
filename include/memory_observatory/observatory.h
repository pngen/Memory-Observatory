// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The Observatory: the central, thread-safe, observational runtime. It owns
// observation, measurement, bounded history, snapshots, timelines, correlation,
// diagnostics, provenance, and replay. It never allocates, moves, reclaims,
// schedules, or otherwise mutates external runtime state: it observes only.

// Reentrancy contract: user callbacks (subscribers) are ALWAYS invoked with no
// internal lock held. No callback ever runs under the history/ingestion lock.

#ifndef MEMORY_OBSERVATORY_OBSERVATORY_HPP
#define MEMORY_OBSERVATORY_OBSERVATORY_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/aggregate.h"
#include "memory_observatory/correlation.h"
#include "memory_observatory/counters.h"
#include "memory_observatory/diagnostic.h"
#include "memory_observatory/domain.h"
#include "memory_observatory/error.h"
#include "memory_observatory/explain.h"
#include "memory_observatory/ingest.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/replay.h"
#include "memory_observatory/report.h"
#include "memory_observatory/snapshot.h"
#include "memory_observatory/source.h"
#include "memory_observatory/timeline.h"
#include "memory_observatory/trace.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// Configuration for the diagnostic engine.
struct DiagnosticConfig {
  bool enabled = true;
  bool correlation_enabled = true;
  std::size_t window_samples = 20;
  Duration stale_after = Duration{5000000000LL};  // 5s
  double near_capacity_ratio = 0.90;
  double sustained_growth_ratio = 0.01;           // per-sample fractional growth
  double rapid_growth_ratio = 0.20;               // over rapid_growth_window
  std::size_t rapid_growth_window = 3;
  Duration oscillation_window = Duration{60000000000LL};  // 60s
  std::size_t oscillation_min_transitions = 3;
  std::size_t churn_window = 32;
  double churn_ratio = 0.50;
};

// Configuration for an Observatory. All storage is bounded by explicit config;
// there is no unbounded telemetry accumulation.
struct ObservatoryConfig {
  std::size_t history_capacity = 100000;
  std::size_t dedup_capacity = 200000;
  std::size_t max_sources = 4096;
  std::size_t max_domains = 256;
  std::size_t queue_capacity = 8192;
  bool deduplicate_ids = true;
  bool reject_stale = false;
  bool reject_out_of_order = false;
  DiagnosticConfig diagnostics;
};

// The Observatory runtime.
class Observatory {
 public:
  using SubscriberFn = std::function<void(const Observation&)>;

  explicit Observatory(const ObservatoryConfig& config = {});
  ~Observatory();

  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;

  // Session and epoch identity.
  [[nodiscard]] const ObservatoryConfig& config() const noexcept;
  [[nodiscard]] SessionId session_id() const noexcept;
  [[nodiscard]] Epoch epoch() const noexcept;

  // ----- source registry -----
  Result<SourceId> register_source(const SourceDescriptor& desc);
  Error remove_source(SourceId id);
  Result<SourceInfo> source_info(SourceId id) const;
  std::vector<SourceInfo> sources() const;
  Error advance_generation(SourceId id);
  Error set_source_health(SourceId id, SourceHealth health);

  // ----- domain registry -----
  Result<DomainId> register_domain(const DomainSpec& spec);
  std::vector<MemoryDomain> domains() const;

  // ----- ingestion -----
  IngestResult observe(Observation observation);
  std::vector<IngestResult> observe_batch(std::vector<Observation> batch);

  // ----- accounting / bounds -----
  AccountingCounters counters() const;
  [[nodiscard]] std::size_t retained_count() const noexcept;

  // ----- history query -----
  Result<Timeline> query_timeline(const TimelineFilter& filter) const;
  Result<WindowedResult> aggregate_timeline(const TimelineFilter& filter,
                                            const SampleExtractor& extractor,
                                            const Window& window) const;

  // ----- snapshot -----
  Result<Snapshot> snapshot(const SnapshotFilter& filter = {}) const;

  // ----- diagnostics / correlation -----
  std::vector<Diagnostic> evaluate_diagnostics() const;
  std::vector<Correlation> evaluate_correlations() const;

  // ----- explain -----
  ExplainResult explain(DiagnosticCode code) const;

  // ----- subscriptions (callbacks are never invoked under an internal lock) -----
  SubscriptionId subscribe(SubscriberFn fn);
  Error unsubscribe(SubscriptionId id);

  // ----- trace capture -----
  Result<Trace> capture_trace() const;

  // ----- report -----
  Result<Report> make_report() const;

  // ----- provider lifecycle (see providers.h) -----
  // Providers are registered as Sources and sampled by the Sampler.

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Deterministic offline replay entry point.
ReplayResult replay_trace(Observatory& observatory, const Trace& trace,
                          const ReplayOptions& options = {});

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_OBSERVATORY_HPP
