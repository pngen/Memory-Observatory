// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Timeline model: a bounded, chronological projection of the history ordered by
// monotonic time. All timelines are bounded by the history capacity; there is
// no unbounded accumulation. Queries are filterable and support downsampling.

#ifndef MEMORY_OBSERVATORY_TIMELINE_HPP
#define MEMORY_OBSERVATORY_TIMELINE_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "memory_observatory/aggregate.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// A single entry in a materialized timeline. It holds a copy of the matched
// observation. Because history is bounded, materialized timelines are also
// bounded by construction.
struct TimelineEntry {
  Observation observation;
  friend bool operator==(const TimelineEntry&, const TimelineEntry&) = default;
};

// Filtering of a timeline query. Each optional narrows the result; a nullopt
// means "any".
struct TimelineFilter {
  std::optional<DomainId> domain;
  std::optional<ObjectId> object;
  std::optional<ProcessId> process;
  std::optional<DeviceId> device;
  std::optional<EventType> event_type;
  std::optional<EventSeverity> severity;
  std::optional<SourceId> source;
  std::optional<Window> window;
  std::optional<TraceId> trace;
  std::optional<SessionId> session;
  std::optional<MemoryKind> memory_kind;
  std::size_t max_results = 0;  // 0 = no additional limit (bounded by history)
};

// A materialized, chronological timeline.
struct Timeline {
  std::vector<TimelineEntry> entries;  // chronological (ascending monotonic time)
  std::uint64_t matched_count = 0;     // entries matching but capped at max_results

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

// Aggregate over a set of numeric samples drawn from a timeline. Windowed and
// bounded; the caller supplies which payload field to pull via an accessor.
using SampleExtractor = std::function<std::optional<double>(const Observation&)>;

// A windowed aggregate produced by a timeline query.
struct WindowedResult {
  Aggregate aggregate;
  Percentiles percentiles;
  std::optional<Rate> rate;
  std::optional<Delta> delta;
  std::uint64_t sample_count = 0;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_TIMELINE_HPP
