// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The observation model. Every observation carries:
//   - a stable ObservationId
//   - a source (SourceId + provenance)
//   - a timestamp (monotonic, optional wall, source-local sequence)
//   - a domain (DomainId + kind)
//   - a generation/epoch
//   - optional object/process/device/worker context
//   - a typed payload (metric sample, event, or state payload)
//   - annotations
//
// Unknown values stay unknown: every numeric payload is optional, and a missing
// metric is never silently treated as zero.

#ifndef MEMORY_OBSERVATORY_OBSERVATION_HPP
#define MEMORY_OBSERVATORY_OBSERVATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "memory_observatory/domain.h"
#include "memory_observatory/provenance.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// A key/value annotation attached to an observation or snapshot.
struct Annotation {
  std::string key;
  std::string value;
  friend bool operator==(const Annotation&, const Annotation&) = default;
};

// Summary of a distribution: a bounded set of statistics, each optional so
// "unknown" is preserved.
struct DistributionSummary {
  std::optional<double> mean;
  std::optional<double> min;
  std::optional<double> max;
  std::optional<std::uint64_t> count;
  std::optional<double> p50;
  std::optional<double> p90;
  std::optional<double> p95;
  std::optional<double> p99;

  [[nodiscard]] bool empty() const noexcept {
    return !mean && !min && !max && !count && !p50 && !p90 && !p95 && !p99;
  }
  friend bool operator==(const DistributionSummary&,
                         const DistributionSummary&) = default;
};

// A gauge sample: an instantaneous value (bytes, ratio, latency, etc.).
struct GaugeSample {
  MemoryKind kind;
  std::optional<double> value;

  [[nodiscard]] bool valid() const noexcept { return value.has_value(); }
  friend bool operator==(const GaugeSample&, const GaugeSample&) = default;
};

// A counter sample: a monotonic cumulative value plus an explicit reset flag
// (a reset counter must never be interpreted as a gigantic negative delta).
struct CounterSample {
  MemoryKind kind;
  std::optional<std::uint64_t> value;
  bool reset = false;

  [[nodiscard]] bool valid() const noexcept { return value.has_value(); }
  friend bool operator==(const CounterSample&, const CounterSample&) = default;
};

// A distribution sample: a set of statistics describing a collection.
struct DistributionSample {
  MemoryKind kind;
  DistributionSummary summary;

  [[nodiscard]] bool valid() const noexcept { return !summary.empty(); }
  friend bool operator==(const DistributionSample&,
                         const DistributionSample&) = default;
};

// A MetricSample is exactly one of the typed samples above.
using MetricSample = std::variant<GaugeSample, CounterSample, DistributionSample>;

// Which "bucket" a metric quantity is measured in.
enum class Scale : std::int32_t {
  kBytes = 0,
  kRatio,
  kCount,
  kMicros,
  kPerSecond,
  kUnknown,
};

inline const char* to_string(Scale s) noexcept {
  switch (s) {
    case Scale::kBytes: return "bytes";
    case Scale::kRatio: return "ratio";
    case Scale::kCount: return "count";
    case Scale::kMicros: return "micros";
    case Scale::kPerSecond: return "per_second";
    case Scale::kUnknown: return "unknown";
  }
  return "unknown";
}

// Event classification.
enum class EventType : std::int32_t {
  kUnknown = 0,
  kSourceRegistered,
  kSourceRemoved,
  kSourceReset,
  kSourceGenerationRollover,
  kQueueOverflow,
  kMalformedRejected,
  kDuplicateSuppressed,
  kBackpressure,
  kProviderUnavailable,
  kProviderRecovered,
  kStaleData,
  kCounterReset,
  kThresholdCrossed,
  kPressure,
  kUser,
  kOther,
};

inline const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::kUnknown: return "unknown";
    case EventType::kSourceRegistered: return "source_registered";
    case EventType::kSourceRemoved: return "source_removed";
    case EventType::kSourceReset: return "source_reset";
    case EventType::kSourceGenerationRollover: return "source_generation_rollover";
    case EventType::kQueueOverflow: return "queue_overflow";
    case EventType::kMalformedRejected: return "malformed_rejected";
    case EventType::kDuplicateSuppressed: return "duplicate_suppressed";
    case EventType::kBackpressure: return "backpressure";
    case EventType::kProviderUnavailable: return "provider_unavailable";
    case EventType::kProviderRecovered: return "provider_recovered";
    case EventType::kStaleData: return "stale_data";
    case EventType::kCounterReset: return "counter_reset";
    case EventType::kThresholdCrossed: return "threshold_crossed";
    case EventType::kPressure: return "pressure";
    case EventType::kUser: return "user";
    case EventType::kOther: return "other";
  }
  return "unknown";
}

enum class EventSeverity : std::int32_t {
  kDebug = 0,
  kInfo,
  kWarning,
  kError,
  kCritical,
};

inline const char* to_string(EventSeverity s) noexcept {
  switch (s) {
    case EventSeverity::kDebug: return "debug";
    case EventSeverity::kInfo: return "info";
    case EventSeverity::kWarning: return "warning";
    case EventSeverity::kError: return "error";
    case EventSeverity::kCritical: return "critical";
  }
  return "unknown";
}

enum class EventSource : std::int32_t {
  kProvider = 0,
  kIngestion,
  kSampler,
  kDiagnostics,
  kCorrelation,
  kUser,
  kSystem,
};

inline const char* to_string(EventSource s) noexcept {
  switch (s) {
    case EventSource::kProvider: return "provider";
    case EventSource::kIngestion: return "ingestion";
    case EventSource::kSampler: return "sampler";
    case EventSource::kDiagnostics: return "diagnostics";
    case EventSource::kCorrelation: return "correlation";
    case EventSource::kUser: return "user";
    case EventSource::kSystem: return "system";
  }
  return "unknown";
}

struct Event {
  EventType type = EventType::kUnknown;
  EventSeverity severity = EventSeverity::kInfo;
  EventSource source = EventSource::kSystem;
  std::optional<std::string> message;
  std::vector<Annotation> annotations;

  friend bool operator==(const Event&, const Event&) = default;
};

// ----- typed state payloads ----------------------------------------------

// A coherent view of a domain's capacity at an instant (used both as a payload
// and as the body of a snapshot).
struct DomainState {
  DomainId domain;
  DomainCapacity capacity;

  friend bool operator==(const DomainState&, const DomainState&) = default;
};

struct ObjectState {
  ObjectId object;
  MemoryKind kind;
  std::optional<Bytes> size;
  std::optional<Bytes> resident;

  friend bool operator==(const ObjectState&, const ObjectState&) = default;
};

struct ResidencyState {
  std::optional<Bytes> resident;
  std::optional<Bytes> total;

  friend bool operator==(const ResidencyState&, const ResidencyState&) = default;
};

struct PressureState {
  std::optional<double> level;      // a domain-relative pressure score in [0,1]
  std::optional<Bytes> available;   // free/available bytes observed

  friend bool operator==(const PressureState&, const PressureState&) = default;
};

struct TransferState {
  std::optional<Bytes> moved;
  std::optional<Duration> duration;

  friend bool operator==(const TransferState&, const TransferState&) = default;
};

struct AllocationState {
  std::optional<Bytes> allocated;
  std::optional<Bytes> freed;
  std::optional<std::uint64_t> alloc_count;
  std::optional<std::uint64_t> free_count;

  friend bool operator==(const AllocationState&, const AllocationState&) =
      default;
};

struct ReclaimState {
  std::optional<Bytes> reclaimed;
  std::optional<std::uint64_t> event_count;

  friend bool operator==(const ReclaimState&, const ReclaimState&) = default;
};

struct CacheState {
  std::optional<Bytes> resident;
  std::optional<Bytes> evicted;
  std::optional<std::uint64_t> evict_count;

  friend bool operator==(const CacheState&, const CacheState&) = default;
};

struct SystemState {
  std::optional<Bytes> total_physical;
  std::optional<Bytes> total_available;
  std::optional<Bytes> total_committed;
  std::optional<Bytes> total_paged_pool;

  friend bool operator==(const SystemState&, const SystemState&) = default;
};

// The typed payload of an observation. Exactly one alternative is populated.
using ObservationPayload =
    std::variant<std::monostate, MetricSample, Event, DomainState, ObjectState,
                 ResidencyState, PressureState, TransferState, AllocationState,
                 ReclaimState, CacheState, SystemState>;

// A single observation. This is the currency of the ingestion pipeline.
struct Observation {
  ObservationId id;
  SourceId source;
  ObservationTimestamp timestamp;
  DomainId domain;
  DomainKind domain_kind = DomainKind::kUnknown;
  Generation generation;
  Epoch epoch;
  Provenance provenance;
  Confidence confidence;
  Freshness freshness;

  std::optional<ObjectId> object;
  std::optional<ProcessId> process;
  std::optional<DeviceId> device;
  std::optional<WorkerId> worker;

  std::vector<Annotation> annotations;
  ObservationPayload payload;

  [[nodiscard]] bool valid() const noexcept {
    return id.valid() && source.valid() && payload.index() != 0;
  }
  friend bool operator==(const Observation&, const Observation&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_OBSERVATION_HPP
