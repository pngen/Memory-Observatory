// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Confidence, freshness, and provenance. Provenance is first-class: every
// observation knows where it came from, how it was produced, whether it is
// measured/replayed/synthetic, and how much to trust it.
//
// "Unknown must remain unknown." An absent confidence is distinct from zero
// confidence: confidence.valid() == false means we genuinely do not know.

#ifndef MEMORY_OBSERVATORY_PROVENANCE_HPP
#define MEMORY_OBSERVATORY_PROVENANCE_HPP

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "memory_observatory/id.h"
#include "memory_observatory/time.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// Confidence is a single value in [0, 1]. An empty confidence is "unknown", not
// "zero". Non-finite or out-of-range inputs are clamped and flagged.
class Confidence {
 public:
  Confidence() noexcept = default;  // unknown

  explicit Confidence(double value) noexcept : value_(normalize(value)) {}

  [[nodiscard]] bool valid() const noexcept { return value_.has_value(); }
  [[nodiscard]] double score() const noexcept {
    return value_.value_or(0.0);
  }

  static constexpr double kMax = 1.0;
  static constexpr double kMin = 0.0;

  friend bool operator==(const Confidence& a, const Confidence& b) noexcept {
    return a.value_ == b.value_;
  }

  friend std::ostream& operator<<(std::ostream& os, const Confidence& c) {
    if (!c.valid()) {
      os << "unknown";
    } else {
      os << c.score();
    }
    return os;
  }

 private:
  static double normalize(double v) noexcept;

  std::optional<double> value_;
};

// A classification of the grounding of a finding, used so reports can clearly
// separate measured facts from derived statistics, correlations, or heuristics.
enum class ConfidenceKind : std::int32_t {
  kUnknown = 0,
  kMeasured,   // direct observation
  kDerived,    // computed statistic from measured inputs
  kCorrelated, // evidence-based correlation, not causation
  kInferred,   // diagnostic interpretation
  kHeuristic,  // heuristic/suspect interpretation (e.g. leak suspect)
  kSynthetic,  // generated, not observed
  kReplayed,   // replayed from a recorded trace
};

inline const char* to_string(ConfidenceKind k) noexcept {
  switch (k) {
    case ConfidenceKind::kUnknown: return "unknown";
    case ConfidenceKind::kMeasured: return "measured";
    case ConfidenceKind::kDerived: return "derived";
    case ConfidenceKind::kCorrelated: return "correlated";
    case ConfidenceKind::kInferred: return "inferred";
    case ConfidenceKind::kHeuristic: return "heuristic";
    case ConfidenceKind::kSynthetic: return "synthetic";
    case ConfidenceKind::kReplayed: return "replayed";
  }
  return "unknown";
}

// How an observation was produced.
enum class ObservationMechanism : std::int32_t {
  kLive = 0,
  kSynthetic,
  kReplayed,
  kInjected,
  kExternal,
};

inline const char* to_string(ObservationMechanism m) noexcept {
  switch (m) {
    case ObservationMechanism::kLive: return "live";
    case ObservationMechanism::kSynthetic: return "synthetic";
    case ObservationMechanism::kReplayed: return "replayed";
    case ObservationMechanism::kInjected: return "injected";
    case ObservationMechanism::kExternal: return "external";
  }
  return "unknown";
}

// Freshness describes how recent an observation is and whether it is considered
// stale under the relevant configuration.
struct Freshness {
  std::optional<Duration> age;
  bool stale = false;

  [[nodiscard]] bool valid() const noexcept { return age.has_value(); }
  friend bool operator==(const Freshness&, const Freshness&) = default;
};

// Provenance is first-class for every observation. All fields are optional where
// the environment may not supply them. A machine id is opt-in (privacy-safe).
struct Provenance {
  SourceId source;
  std::string source_name;
  std::string provider_version;

  std::optional<std::string> machine_id;  // privacy: only if explicitly supplied
  std::string process;
  std::string runtime_version;
  std::string toolchain;
  std::string os;
  std::string architecture;
  std::optional<std::string> accelerator;
  std::string driver_version;

  ObservationMechanism mechanism = ObservationMechanism::kLive;
  bool wall_clock_trusted = false;  // wall clock may be trusted for correlation

  Confidence confidence;
  std::string capture_session;

  Freshness freshness;
  bool synthetic = false;

  // Arbitrary key/value annotations supplied by producers or adapters.
  std::vector<std::pair<std::string, std::string>> annotations;

  // Observation, Diagnostic, Snapshot and Trace default their equality over
  // this member, so it has to be comparable for those to be usable.
  friend bool operator==(const Provenance&, const Provenance&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_PROVENANCE_HPP
