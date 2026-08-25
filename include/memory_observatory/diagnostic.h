// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Structured diagnostic engine. Every diagnostic is fully explainable: it
// exposes its summary, severity, affected entities, evidence, relevant
// timestamps, metric deltas, confidence, provenance, freshness, the rule or
// algorithm that produced it, its assumptions, its limitations, and a
// human-readable explanation.

#ifndef MEMORY_OBSERVATORY_DIAGNOSTIC_HPP
#define MEMORY_OBSERVATORY_DIAGNOSTIC_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/aggregate.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/provenance.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// The set of diagnostics the engine understands. Each has a deterministic rule.
enum class DiagnosticCode : std::int32_t {
  kNone = 0,
  kSustainedCapacityGrowth,
  kNearCapacity,
  kRapidGrowth,
  kRepeatedThresholdCrossing,
  kOscillationFlapping,
  kAllocationChurn,
  kReclaimChurn,
  kMigrationChurn,
  kPossibleLeakPattern,
  kFragmentationGrowth,
  kContentionGrowth,
  kStaleTelemetry,
  kMissingProvider,
  kInconsistentCounters,
  kImpossibleAccounting,
  kCapacityDiscontinuity,
  kSourceReset,
  kSourceGenerationMismatch,
  kEventLoss,
  kQueueOverflow,
  kHighSamplingJitter,
  kAbnormalLatencyIncrease,
  kRecoveryAfterPressure,
  kPersistentPressure,
  kPressureWithoutRelief,
  kDivergentSourceObservations,
};

inline const char* to_string(DiagnosticCode c) noexcept {
  switch (c) {
    case DiagnosticCode::kNone: return "none";
    case DiagnosticCode::kSustainedCapacityGrowth: return "sustained_capacity_growth";
    case DiagnosticCode::kNearCapacity: return "near_capacity";
    case DiagnosticCode::kRapidGrowth: return "rapid_growth";
    case DiagnosticCode::kRepeatedThresholdCrossing: return "repeated_threshold_crossing";
    case DiagnosticCode::kOscillationFlapping: return "oscillation_flapping";
    case DiagnosticCode::kAllocationChurn: return "allocation_churn";
    case DiagnosticCode::kReclaimChurn: return "reclaim_churn";
    case DiagnosticCode::kMigrationChurn: return "migration_churn";
    case DiagnosticCode::kPossibleLeakPattern: return "possible_leak_pattern";
    case DiagnosticCode::kFragmentationGrowth: return "fragmentation_growth";
    case DiagnosticCode::kContentionGrowth: return "contention_growth";
    case DiagnosticCode::kStaleTelemetry: return "stale_telemetry";
    case DiagnosticCode::kMissingProvider: return "missing_provider";
    case DiagnosticCode::kInconsistentCounters: return "inconsistent_counters";
    case DiagnosticCode::kImpossibleAccounting: return "impossible_accounting";
    case DiagnosticCode::kCapacityDiscontinuity: return "capacity_discontinuity";
    case DiagnosticCode::kSourceReset: return "source_reset";
    case DiagnosticCode::kSourceGenerationMismatch: return "source_generation_mismatch";
    case DiagnosticCode::kEventLoss: return "event_loss";
    case DiagnosticCode::kQueueOverflow: return "queue_overflow";
    case DiagnosticCode::kHighSamplingJitter: return "high_sampling_jitter";
    case DiagnosticCode::kAbnormalLatencyIncrease: return "abnormal_latency_increase";
    case DiagnosticCode::kRecoveryAfterPressure: return "recovery_after_pressure";
    case DiagnosticCode::kPersistentPressure: return "persistent_pressure";
    case DiagnosticCode::kPressureWithoutRelief: return "pressure_without_relief";
    case DiagnosticCode::kDivergentSourceObservations: return "divergent_source_observations";
  }
  return "none";
}

enum class DiagnosticSeverity : std::int32_t {
  kInfo = 0,
  kWarning,
  kCritical,
};

inline const char* to_string(DiagnosticSeverity s) noexcept {
  switch (s) {
    case DiagnosticSeverity::kInfo: return "info";
    case DiagnosticSeverity::kWarning: return "warning";
    case DiagnosticSeverity::kCritical: return "critical";
  }
  return "info";
}

// The epistemic grounding of a finding. This is how reports separate measured
// facts from derived statistics, correlations, and heuristics.
enum class DiagnosticKind : std::int32_t {
  kMeasuredFact = 0,
  kDerivedStatistic,
  kCorrelation,
  kHeuristicInterpretation,
  kInferred,
  kSynthetic,
  kReplayed,
};

inline const char* to_string(DiagnosticKind k) noexcept {
  switch (k) {
    case DiagnosticKind::kMeasuredFact: return "measured_fact";
    case DiagnosticKind::kDerivedStatistic: return "derived_statistic";
    case DiagnosticKind::kCorrelation: return "correlation";
    case DiagnosticKind::kHeuristicInterpretation: return "heuristic_interpretation";
    case DiagnosticKind::kInferred: return "inferred";
    case DiagnosticKind::kSynthetic: return "synthetic";
    case DiagnosticKind::kReplayed: return "replayed";
  }
  return "inferred";
}

// A piece of evidence feeding a diagnostic.
struct DiagnosticEvidence {
  std::string description;
  std::optional<ObservationId> observation;
  std::optional<DomainId> domain;
  std::optional<SourceId> source;
  std::optional<MonotonicTimestamp> timestamp;
  MemoryKind kind = MemoryKind::kUnknown;
  std::optional<double> value;
  std::optional<double> delta;
  std::optional<double> rate;
  friend bool operator==(const DiagnosticEvidence&,
                         const DiagnosticEvidence&) = default;
};

// A metric delta used in a diagnostic explanation.
struct MetricDelta {
  MemoryKind kind = MemoryKind::kUnknown;
  std::optional<double> from;
  std::optional<double> to;
  std::optional<double> delta;
  Duration over;
  friend bool operator==(const MetricDelta&, const MetricDelta&) = default;
};

// A fully explainable diagnostic.
struct Diagnostic {
  DiagnosticCode code = DiagnosticCode::kNone;
  DiagnosticSeverity severity = DiagnosticSeverity::kInfo;
  DiagnosticKind kind = DiagnosticKind::kInferred;
  std::string summary;
  std::optional<DomainId> domain;
  std::optional<SourceId> source;
  std::optional<ObjectId> object;
  std::vector<DiagnosticEvidence> evidence;
  std::vector<MonotonicTimestamp> relevant_timestamps;
  std::vector<MetricDelta> metric_deltas;
  Confidence confidence;
  Freshness freshness;
  Provenance provenance;
  std::string rule_used;
  std::vector<std::string> assumptions;
  std::vector<std::string> limitations;
  std::string explanation;

  [[nodiscard]] bool valid() const noexcept { return code != DiagnosticCode::kNone; }
  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_DIAGNOSTIC_HPP
