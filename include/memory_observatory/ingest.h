// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Ingestion result model. Every ingestion is accounted for under an explicit
// disposition, and each non-accepted observation also increments a precise
// reason counter so nothing is lost silently.

#ifndef MEMORY_OBSERVATORY_INGEST_HPP
#define MEMORY_OBSERVATORY_INGEST_HPP

#include <cstdint>
#include <string>

#include "memory_observatory/types.h"

namespace memory_observatory {

// What happened to an ingested observation.
enum class IngestOutcome : std::int32_t {
  kAccepted = 0,
  kRejectedMalformed,
  kRejectedInvalid,
  kRejectedStale,
  kRejectedOutOfOrder,
  kDroppedOverflow,
  kDuplicateSuppressed,
  kBackpressured,
};

inline const char* to_string(IngestOutcome o) noexcept {
  switch (o) {
    case IngestOutcome::kAccepted: return "accepted";
    case IngestOutcome::kRejectedMalformed: return "rejected_malformed";
    case IngestOutcome::kRejectedInvalid: return "rejected_invalid";
    case IngestOutcome::kRejectedStale: return "rejected_stale";
    case IngestOutcome::kRejectedOutOfOrder: return "rejected_out_of_order";
    case IngestOutcome::kDroppedOverflow: return "dropped_overflow";
    case IngestOutcome::kDuplicateSuppressed: return "duplicate_suppressed";
    case IngestOutcome::kBackpressured: return "backpressured";
  }
  return "unknown";
}

// Per-observation ingestion outcome.
struct IngestResult {
  IngestOutcome outcome = IngestOutcome::kRejectedInvalid;
  std::string reason;
  ObservationId id;

  [[nodiscard]] bool accepted() const noexcept {
    return outcome == IngestOutcome::kAccepted;
  }
  [[nodiscard]] bool dropped() const noexcept {
    return outcome == IngestOutcome::kDroppedOverflow ||
           outcome == IngestOutcome::kBackpressured;
  }
  friend bool operator==(const IngestResult&, const IngestResult&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_INGEST_HPP
