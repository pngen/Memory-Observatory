// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Central public type aliases: strong identifiers for every named entity and the
// combined observation timestamp. Kept together so consumers include one header
// and get the whole stable surface.

#ifndef MEMORY_OBSERVATORY_TYPES_HPP
#define MEMORY_OBSERVATORY_TYPES_HPP

#include <optional>

#include "memory_observatory/id.h"
#include "memory_observatory/time.h"

namespace memory_observatory {

// Tags that uniquely type each strong identifier.
#define MEMORY_OBSERVATORY_ID_TAG(Name) struct Name##Tag {}
MEMORY_OBSERVATORY_ID_TAG(Observation);
MEMORY_OBSERVATORY_ID_TAG(Source);
MEMORY_OBSERVATORY_ID_TAG(Domain);
MEMORY_OBSERVATORY_ID_TAG(Object);
MEMORY_OBSERVATORY_ID_TAG(Process);
MEMORY_OBSERVATORY_ID_TAG(Device);
MEMORY_OBSERVATORY_ID_TAG(Worker);
MEMORY_OBSERVATORY_ID_TAG(Snapshot);
MEMORY_OBSERVATORY_ID_TAG(Trace);
MEMORY_OBSERVATORY_ID_TAG(Session);
MEMORY_OBSERVATORY_ID_TAG(Subscription);
MEMORY_OBSERVATORY_ID_TAG(Epoch);
#undef MEMORY_OBSERVATORY_ID_TAG

using ObservationId = Id<ObservationTag>;
using SourceId = Id<SourceTag>;
using DomainId = Id<DomainTag>;
using ObjectId = Id<ObjectTag>;
using ProcessId = Id<ProcessTag>;
using DeviceId = Id<DeviceTag>;
using WorkerId = Id<WorkerTag>;
using SnapshotId = Id<SnapshotTag>;
using TraceId = Id<TraceTag>;
using SessionId = Id<SessionTag>;
using SubscriptionId = Id<SubscriptionTag>;

// Combined timestamp carried by every observation: a monotonic instant (used for
// all ordering and duration math), an optional wall time for human correlation,
// and a source-local sequence number.
struct ObservationTimestamp {
  MonotonicTimestamp monotonic;
  std::optional<WallTimestamp> wall;
  SequenceNo sequence;

  [[nodiscard]] bool valid() const noexcept { return monotonic.valid(); }
  friend bool operator==(const ObservationTimestamp&,
                         const ObservationTimestamp&) = default;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_TYPES_HPP
