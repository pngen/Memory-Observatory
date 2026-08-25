// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Evidence-based correlation. Correlations are explicitly NOT causation: every
// correlation exposes the observations/evidence it used and carries a clear
// limitation noting it is a correlation, not a causal conclusion.

#ifndef MEMORY_OBSERVATORY_CORRELATION_HPP
#define MEMORY_OBSERVATORY_CORRELATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/diagnostic.h"

namespace memory_observatory {

// Kinds of correlations the engine can express.
enum class CorrelationKind : std::int32_t {
  kNone = 0,
  kAllocationPressure,
  kPressureTransitionFollowingCommitGrowth,
  kReclaimFollowedByAvailableRecovery,
  kAllocationFreeChurn,
  kResidencyTransitionChurn,
  kTransferBurstPrecedingUtilizationChange,
  kFragmentationMovement,
  kLatencyAndContention,
  kPeriodicOscillation,
  kStaleProviderConfidenceDegradation,
  kDiscontinuityAfterSourceRestart,
};

inline const char* to_string(CorrelationKind k) noexcept {
  switch (k) {
    case CorrelationKind::kNone: return "none";
    case CorrelationKind::kAllocationPressure: return "allocation_pressure";
    case CorrelationKind::kPressureTransitionFollowingCommitGrowth: return "pressure_transition_following_commit_growth";
    case CorrelationKind::kReclaimFollowedByAvailableRecovery: return "reclaim_followed_by_available_recovery";
    case CorrelationKind::kAllocationFreeChurn: return "allocation_free_churn";
    case CorrelationKind::kResidencyTransitionChurn: return "residency_transition_churn";
    case CorrelationKind::kTransferBurstPrecedingUtilizationChange: return "transfer_burst_preceding_utilization_change";
    case CorrelationKind::kFragmentationMovement: return "fragmentation_movement";
    case CorrelationKind::kLatencyAndContention: return "latency_and_contention";
    case CorrelationKind::kPeriodicOscillation: return "periodic_oscillation";
    case CorrelationKind::kStaleProviderConfidenceDegradation: return "stale_provider_confidence_degradation";
    case CorrelationKind::kDiscontinuityAfterSourceRestart: return "discontinuity_after_source_restart";
  }
  return "none";
}

// A correlation over a set of observations. It never asserts causation.
struct Correlation {
  CorrelationKind kind = CorrelationKind::kNone;
  std::string summary;
  std::vector<ObservationId> observations;
  std::vector<Diagnostic> involved_diagnostics;
  Confidence confidence;
  std::vector<std::string> limitations;
  std::string explanation;

  [[nodiscard]] bool valid() const noexcept { return kind != CorrelationKind::kNone; }
  friend bool operator==(const Correlation&, const Correlation&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_CORRELATION_HPP
