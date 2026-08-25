// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Snapshots. A snapshot is a logically immutable, coherent observational view at
// a defined point. It is THREAD-SAFE to create. Consistency semantics are
// documented on the snapshot: 1.0 snapshots are ingestion-consistent/best-effort
// (they represent a bounded, coherent view of the retained history at capture
// time), never a claim of exact global atomicity.

#ifndef MEMORY_OBSERVATORY_SNAPSHOT_HPP
#define MEMORY_OBSERVATORY_SNAPSHOT_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "memory_observatory/counters.h"
#include "memory_observatory/diagnostic.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/provenance.h"
#include "memory_observatory/source.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// What subset a snapshot should cover. whole_system takes precedence.
struct SnapshotFilter {
  bool whole_system = false;
  std::optional<DomainId> domain;
  std::optional<SourceId> source;
};

// Consistency semantics for a snapshot.
enum class SnapshotSemantics : std::int32_t {
  kIngestionConsistent = 0,  // best-effort view of retained history
  kBestEffort,               // may observe a partially ordered history
};

inline const char* to_string(SnapshotSemantics s) noexcept {
  switch (s) {
    case SnapshotSemantics::kIngestionConsistent: return "ingestion_consistent";
    case SnapshotSemantics::kBestEffort: return "best_effort";
  }
  return "best_effort";
}

// A logically immutable snapshot.
struct Snapshot {
  SnapshotId id;
  MonotonicTimestamp captured_at;
  std::optional<WallTimestamp> wall;
  Epoch epoch;
  SessionId session;
  SnapshotSemantics semantics = SnapshotSemantics::kIngestionConsistent;

  std::vector<DomainState> domains;
  std::vector<SourceInfo> sources;
  std::uint64_t observation_count = 0;
  AccountingCounters counters;
  std::vector<Diagnostic> active_diagnostics;

  // Provenance / confidence / freshness summaries.
  std::vector<Provenance> provenance_summary;
  Confidence confidence_summary;
  Freshness freshness_summary;
  std::uint64_t stale_sources = 0;

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }
  friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_SNAPSHOT_HPP
