// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <vector>

#include "memory_observatory/observatory.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

namespace {

Trace make_trace(std::size_t n) {
  Trace trace;
  trace.id = TraceId{7};
  trace.session = SessionId{3};
  trace.format = "memory-observatory-trace";
  trace.capture_mechanism = "synthetic";
  for (std::size_t i = 0; i < n; ++i) {
    TraceRecord rec;
    rec.observation.id = ObservationId{100 + i};
    rec.observation.source = SourceId{1};
    rec.observation.domain = DomainId{9};
    rec.observation.domain_kind = DomainKind::kHostRam;
    rec.observation.timestamp.monotonic = MonotonicTimestamp{1000000000LL + 1000LL * static_cast<int64_t>(i)};
    rec.observation.timestamp.sequence = SequenceNo{static_cast<int64_t>(i)};
    rec.observation.generation = Generation{1};
    GaugeSample g;
    g.kind = MemoryKind::kAvailable;
    g.value = 100.0 - static_cast<double>(i);
    rec.observation.payload = g;
    trace.records.push_back(std::move(rec));
  }
  return trace;
}

}  // namespace

TEST(trace_binary_round_trip) {
  Trace in = make_trace(5);
  auto ser = serialize_trace_binary(in);
  REQUIRE(ser.ok());
  auto de = deserialize_trace_binary(ser.value().data(), ser.value().size());
  REQUIRE(de.ok());
  CHECK_EQ(de.value().records.size(), std::size_t{5});
  CHECK(de.value().id == in.id);
  CHECK(de.value().session == in.session);
  for (std::size_t i = 0; i < 5; ++i) {
    const Observation& a = in.records[i].observation;
    const Observation& b = de.value().records[i].observation;
    CHECK(a.id == b.id);
    CHECK(a.source == b.source);
    CHECK(a.domain == b.domain);
    CHECK(a.timestamp.monotonic == b.timestamp.monotonic);
    const auto* ma = std::get_if<MetricSample>(&a.payload);
    const auto* mb = std::get_if<MetricSample>(&b.payload);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);
    const auto* ga = std::get_if<GaugeSample>(ma);
    const auto* gb = std::get_if<GaugeSample>(mb);
    REQUIRE(ga != nullptr);
    REQUIRE(gb != nullptr);
    CHECK(ga->kind == gb->kind);
    CHECK(*ga->value == *gb->value);
  }
}

TEST(trace_json_round_trip) {
  Trace in = make_trace(4);
  auto ser = serialize_trace_json(in);
  REQUIRE(ser.ok());
  auto de = deserialize_trace_json(ser.value());
  REQUIRE(de.ok());
  CHECK_EQ(de.value().records.size(), std::size_t{4});
  CHECK(de.value().id == in.id);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(de.value().records[i].observation.id == in.records[i].observation.id);
    CHECK(de.value().records[i].observation.source == in.records[i].observation.source);
    CHECK(de.value().records[i].observation.domain == in.records[i].observation.domain);
  }
}

TEST(trace_json_rejects_malformed) {
  auto de = deserialize_trace_json("{ not json ");
  CHECK(!de.ok());
  auto de2 = deserialize_trace_json("{\"version\":999}");
  CHECK(!de2.ok());
}

TEST(trace_serialization_is_deterministic) {
  Trace in = make_trace(3);
  auto s1 = serialize_trace_binary(in);
  auto s2 = serialize_trace_binary(in);
  REQUIRE(s1.ok());
  REQUIRE(s2.ok());
  CHECK(s1.value() == s2.value());
}

TEST(trace_rejects_corruption) {
  Trace in = make_trace(4);
  auto ser = serialize_trace_binary(in);
  REQUIRE(ser.ok());
  std::vector<std::uint8_t> bytes = ser.value();
  // Flip a byte in the middle; deserialization must reject it.
  const std::size_t mid = bytes.size() / 2;
  bytes[mid] ^= 0xff;
  auto de = deserialize_trace_binary(bytes.data(), bytes.size());
  CHECK(!de.ok());
}

TEST(trace_rejects_truncation) {
  Trace in = make_trace(4);
  auto ser = serialize_trace_binary(in);
  REQUIRE(ser.ok());
  std::vector<std::uint8_t> bytes = ser.value();
  bytes.resize(bytes.size() - 5);  // drop trailing bytes
  auto de = deserialize_trace_binary(bytes.data(), bytes.size());
  CHECK(!de.ok());
}

TEST(trace_rejects_bad_magic_and_bad_version) {
  std::vector<std::uint8_t> bytes(64, 0);
  auto de = deserialize_trace_binary(bytes.data(), bytes.size());
  CHECK(!de.ok());
  CHECK(de.error().code() == ErrorCode::kInvalidTrace ||
        de.error().code() == ErrorCode::kMalformedInput);
}

TEST(deterministic_replay_consumes_records) {
  Trace in = make_trace(10);
  Observatory obs;
  ReplayOptions opts;
  opts.accelerated = true;
  ReplayResult rr = replay_trace(obs, in, opts);
  CHECK_EQ(rr.accepted, std::uint64_t{10});
  CHECK_EQ(rr.consumed, std::uint64_t{10});
  CHECK_EQ(rr.rejected, std::uint64_t{0});
  CHECK(rr.snapshot_after.valid());
}

TEST(replay_regenerates_snapshot_and_diagnostics) {
  Trace in = make_trace(12);
  // Set a growth-like series in the trace so diagnostics can fire.
  for (std::size_t i = 0; i < in.records.size(); ++i) {
    GaugeSample g;
    g.kind = MemoryKind::kAllocated;
    g.value = 1000.0 + static_cast<double>(i) * 50.0;
    in.records[i].observation.payload = g;
  }
  Observatory obs;
  ReplayResult rr = replay_trace(obs, in, {});
  CHECK_EQ(rr.accepted, std::uint64_t{12});
  CHECK(rr.snapshot_after.valid());
  // The regenerated snapshot should carry at least one diagnostic.
  CHECK(rr.diagnostics.size() >= 1);
}
