// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Trace capture and replay. A trace is a versioned, deterministic record of
// observations. Two encodings are provided: a strict binary format (canonical,
// bounded, checksummed) and a human-readable JSON format. All deserializers
// treat input as untrusted: they bound counts, lengths, and nesting, and reject
// malformed, truncated, corrupt, or absurd inputs before allocating.

#ifndef MEMORY_OBSERVATORY_TRACE_HPP
#define MEMORY_OBSERVATORY_TRACE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/error.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// Magic bytes for the binary trace format: "MOBS"
constexpr std::uint32_t kTraceMagic = 0x53424f4dU;  // little-endian "MOBS"
constexpr std::uint32_t kTraceFormatVersion = 1;

// One recorded observation in a trace.
struct TraceRecord {
  Observation observation;
  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;
};

// A versioned, deterministic trace.
struct Trace {
  TraceId id;
  std::uint32_t version = kTraceFormatVersion;
  std::string format = "memory-observatory-trace";
  bool little_endian = true;
  SessionId session;
  std::vector<TraceRecord> records;
  std::vector<Annotation> annotations;
  // Capture-time provenance summary.
  std::string capture_machine;
  std::string capture_toolchain;
  std::string capture_os;
  std::string capture_arch;
  std::string capture_mechanism;  // live / synthetic / injected / external

  [[nodiscard]] bool empty() const noexcept { return records.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return records.size(); }
  friend bool operator==(const Trace&, const Trace&) = default;
};

enum class TraceEncoding : std::int32_t {
  kBinary = 0,
  kJson,
};

inline const char* to_string(TraceEncoding e) noexcept {
  switch (e) {
    case TraceEncoding::kBinary: return "binary";
    case TraceEncoding::kJson: return "json";
  }
  return "binary";
}

// Bounds applied to untrusted deserialization. Callers may tighten but never
// loosen these (a parsed trace is used to populate an Observatory, so an absurd
// request could otherwise exhaust memory before a bound is enforced).
struct TraceLimits {
  std::size_t max_records = 10000000;   // 10M
  std::size_t max_annotation_count = 256;
  std::size_t max_annotation_bytes = 4096;
  std::size_t max_string_bytes = 1048576;  // 1 MiB
  std::size_t max_payload_bytes = 16777216;  // 16 MiB
};

// Serialize a trace to the strict binary format. Deterministic: repeated calls
// on an equal trace produce identical bytes.
Result<std::vector<std::uint8_t>> serialize_trace_binary(const Trace& trace);

// Serialize a trace to a human-readable JSON string.
Result<std::string> serialize_trace_json(const Trace& trace);

// Deserialize a strict binary trace. Rejects malformed/truncated/oversized input.
Result<Trace> deserialize_trace_binary(const std::uint8_t* data, std::size_t size,
                                       const TraceLimits& limits = {});

// Deserialize a JSON trace. Validates structure against the schema.
Result<Trace> deserialize_trace_json(std::string_view text,
                                     const TraceLimits& limits = {});

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_TRACE_HPP
