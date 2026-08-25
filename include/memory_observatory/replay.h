// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic offline replay. Replay feeds a fresh Observatory from a trace,
// regenerates diagnostics, and can compare output to recorded expectations.
// Default replay is accelerated (no wall-clock sleeping); original-relative-
// timing replay is available but tests never sleep for recorded durations.

#ifndef MEMORY_OBSERVATORY_REPLAY_HPP
#define MEMORY_OBSERVATORY_REPLAY_HPP

#include <cstdint>
#include <string>

#include "memory_observatory/diagnostic.h"
#include "memory_observatory/trace.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

struct ReplayOptions {
  bool accelerated = true;            // ignore original inter-record timing
  bool preserve_relative_timing = false;  // spacing proportional to original
  bool include_wall_times = false;
  std::size_t record_limit = 0;       // 0 = all records
};

// Result of feeding a trace into a fresh Observatory.
struct ReplayResult {
  std::uint64_t consumed = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  SessionId session;
  SnapshotId snapshot_after;
  std::vector<Diagnostic> diagnostics;
  TraceId trace;
  std::string seed;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_REPLAY_HPP
