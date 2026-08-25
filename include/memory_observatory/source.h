// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Source registry. A Source is an identity for a producer of observations.
// Sources carry a generation that advances on reset/restart, so observations
// from before and after a restart can be distinguished. Source health is
// tracked so stale or failing providers are visible to the diagnostics engine.

#ifndef MEMORY_OBSERVATORY_SOURCE_HPP
#define MEMORY_OBSERVATORY_SOURCE_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "memory_observatory/time.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// Health of a source as tracked by the Observatory.
enum class SourceHealth : std::int32_t {
  kUnknown = 0,
  kHealthy,
  kDegraded,
  kStale,
  kFailed,
  kRemoved,
};

inline const char* to_string(SourceHealth h) noexcept {
  switch (h) {
    case SourceHealth::kUnknown: return "unknown";
    case SourceHealth::kHealthy: return "healthy";
    case SourceHealth::kDegraded: return "degraded";
    case SourceHealth::kStale: return "stale";
    case SourceHealth::kFailed: return "failed";
    case SourceHealth::kRemoved: return "removed";
  }
  return "unknown";
}

// Descriptor supplied when registering a source.
struct SourceDescriptor {
  std::string name;
  std::string kind;  // e.g. "host", "process", "cuda", "synthetic", "replay", "push"
  std::optional<std::string> provider_version;
  std::optional<std::string> machine_id;  // privacy: opt-in
};

// Runtime information about a registered source.
struct SourceInfo {
  SourceId id;
  std::string name;
  std::string kind;
  Generation generation;
  SourceHealth health = SourceHealth::kUnknown;
  std::uint64_t observations_ingested = 0;
  std::uint64_t events_ingested = 0;
  MonotonicTimestamp last_observation;
  bool last_observation_known = false;
  std::uint64_t generation_rollovers = 0;
  MonotonicTimestamp last_generation_change;
  std::optional<MonotonicTimestamp> first_observation;
  std::optional<MonotonicTimestamp> last_event;

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }
  friend bool operator==(const SourceInfo&, const SourceInfo&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_SOURCE_HPP
