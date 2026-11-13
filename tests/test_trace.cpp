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

// ---------------------------------------------------------------------------
// Strict deserialization, faithful JSON, replay provenance.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

std::uint32_t test_crc32(const std::vector<std::uint8_t>& bytes, std::size_t n) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= bytes[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}

// Re-seals a hand-edited trace, so the edit and not the checksum is what the
// deserializer has to catch.
void reseal(std::vector<std::uint8_t>& bytes) {
  const std::size_t body = bytes.size() - 4;
  const std::uint32_t crc = test_crc32(bytes, body);
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[body + i] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xffu);
  }
}

std::vector<std::uint8_t> one_gauge_trace_bytes() {
  auto ser = serialize_trace_binary(make_trace(1));
  return ser.ok() ? ser.value() : std::vector<std::uint8_t>{};
}

// Serializes a trace and reports whether reading it back fails with `expected`.
bool binary_rejects(const Trace& t, ErrorCode expected) {
  auto ser = serialize_trace_binary(t);
  if (!ser.ok()) return false;
  auto de = deserialize_trace_binary(ser.value().data(), ser.value().size());
  return !de.ok() && de.error().code() == expected;
}

}  // namespace

TEST(trace_binary_rejects_trailing_bytes) {
  std::vector<std::uint8_t> bytes = one_gauge_trace_bytes();
  REQUIRE(bytes.size() > 8);
  CHECK(deserialize_trace_binary(bytes.data(), bytes.size()).ok());
  bytes.insert(bytes.end() - 4, {0xde, 0xad, 0xbe, 0xef});
  reseal(bytes);
  CHECK(!deserialize_trace_binary(bytes.data(), bytes.size()).ok());
}

TEST(trace_binary_rejects_wrong_byte_order_flag) {
  std::vector<std::uint8_t> bytes = one_gauge_trace_bytes();
  REQUIRE(bytes.size() > 16);
  bytes[8] = 0;  // the endianness word: "not little-endian"
  reseal(bytes);
  CHECK(!deserialize_trace_binary(bytes.data(), bytes.size()).ok());
}

TEST(trace_binary_rejects_non_finite_present_value) {
  std::vector<std::uint8_t> bytes = one_gauge_trace_bytes();
  REQUIRE(bytes.size() > 32);
  // The record ends: value-present flag (1), value (8), annotation count (4);
  // the checksum (4) follows.
  const std::size_t value_at = bytes.size() - 4 - 4 - 8;
  REQUIRE(bytes[value_at - 1] == 1);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::memcpy(&bytes[value_at], &nan, sizeof(nan));
  reseal(bytes);
  CHECK(!deserialize_trace_binary(bytes.data(), bytes.size()).ok());
}

TEST(trace_binary_rejects_invalid_enums_and_timestamps) {
  {
    Trace t = make_trace(2);
    t.records[1].observation.domain_kind = static_cast<DomainKind>(77);
    CHECK(binary_rejects(t, ErrorCode::kInvalidEnum));
  }
  {
    Trace t = make_trace(2);
    t.records[0].observation.provenance.mechanism = static_cast<ObservationMechanism>(9);
    CHECK(binary_rejects(t, ErrorCode::kInvalidEnum));
  }
  {
    Trace t = make_trace(1);
    GaugeSample g;
    g.kind = static_cast<MemoryKind>(99);
    g.value = 1.0;
    t.records[0].observation.payload = g;
    CHECK(binary_rejects(t, ErrorCode::kInvalidEnum));
  }
  {
    Trace t = make_trace(1);
    Event e;
    e.type = static_cast<EventType>(500);
    t.records[0].observation.payload = e;
    CHECK(binary_rejects(t, ErrorCode::kInvalidEnum));
  }
  {
    Trace t = make_trace(1);
    Event e;
    e.severity = static_cast<EventSeverity>(-1);
    t.records[0].observation.payload = e;
    CHECK(binary_rejects(t, ErrorCode::kInvalidEnum));
  }
  {
    Trace t = make_trace(1);
    t.records[0].observation.timestamp.monotonic = MonotonicTimestamp{-5};
    CHECK(binary_rejects(t, ErrorCode::kInvalidTimestamp));
  }
  {
    Trace t = make_trace(1);
    TransferState ts;
    ts.duration = Duration{-1};
    t.records[0].observation.payload = ts;
    CHECK(binary_rejects(t, ErrorCode::kInvalidTimestamp));
  }
}

TEST(trace_json_rejects_deep_nesting) {
  const std::string deep_arrays(200000, '[');
  CHECK(!deserialize_trace_json(deep_arrays).ok());
  std::string deep_objects;
  for (int i = 0; i < 50000; ++i) deep_objects += "{\"a\":";
  CHECK(!deserialize_trace_json(deep_objects).ok());
  // Nesting inside the bound still parses.
  CHECK(deserialize_trace_json("{\"version\":1,\"x\":[[[[[[[[1]]]]]]]]}").ok());
}

TEST(trace_json_reads_back_what_it_writes) {
  Trace in;
  in.id = TraceId{7};
  in.session = SessionId{3};
  in.capture_mechanism = "synthetic";
  in.annotations.push_back({"run", "a \"quoted\" value\nwith a newline"});
  // None of these fit a double exactly.
  const std::int64_t wall_ns = 1794614400123456789LL;
  const std::int64_t mono_ns = 9007199254740993LL;  // 2^53 + 1
  const std::uint64_t big = 9007199254740993ULL + 4096;
  auto add = [&](ObservationPayload payload) {
    const std::int64_t n = static_cast<std::int64_t>(in.records.size());
    TraceRecord rec;
    Observation& o = rec.observation;
    o.id = ObservationId{100 + static_cast<std::uint64_t>(n)};
    o.source = SourceId{5};
    o.domain = DomainId{9};
    o.domain_kind = DomainKind::kAcceleratorDevice;
    o.timestamp.monotonic = MonotonicTimestamp{mono_ns + n};
    o.timestamp.wall = WallTimestamp{wall_ns + n};
    o.timestamp.sequence = SequenceNo{n};
    o.generation = Generation{4};
    o.epoch = Epoch{2};
    o.provenance.mechanism = ObservationMechanism::kSynthetic;
    o.provenance.confidence = Confidence{0.5};
    o.annotations.push_back({"k", "v"});
    o.payload = std::move(payload);
    in.records.push_back(std::move(rec));
  };
  {
    GaugeSample g;
    g.kind = MemoryKind::kLive;
    g.value = 12.5;
    add(MetricSample{g});
  }
  {
    CounterSample c;
    c.kind = MemoryKind::kReserved;
    c.value = big;
    c.reset = true;
    add(MetricSample{c});
  }
  {
    DistributionSample d;
    d.kind = MemoryKind::kCached;
    d.summary.mean = 3.5;
    d.summary.p95 = 9.25;
    d.summary.p99 = 9.75;
    d.summary.count = big;
    add(MetricSample{d});
  }
  {
    Event e;
    e.type = EventType::kThresholdCrossed;
    e.severity = EventSeverity::kError;
    e.source = EventSource::kSampler;
    e.message = "limit \"hit\"\tnow";
    add(e);
  }
  {
    DomainState ds;
    ds.domain = DomainId{9};
    ds.capacity.physical = big + 9;
    ds.capacity.governed = big + 8;
    ds.capacity.committed = big + 7;
    ds.capacity.reserved = big + 6;
    ds.capacity.allocated = big + 5;
    ds.capacity.live = big + 4;
    ds.capacity.available = big + 3;
    ds.capacity.cached = big + 2;
    ds.capacity.pinned = big + 1;
    ds.capacity.mapped = big;
    add(ds);
  }
  {
    PressureState ps;
    ps.level = 0.75;
    ps.available = big;
    add(ps);
  }
  add(ReclaimState{});  // the JSON form carries its type only

  auto ser = serialize_trace_json(in);
  REQUIRE(ser.ok());
  auto de = deserialize_trace_json(ser.value());
  REQUIRE(de.ok());
  const Trace& out = de.value();
  CHECK(out.id == in.id);
  CHECK(out.session == in.session);
  CHECK_EQ(out.capture_mechanism, in.capture_mechanism);
  CHECK(out.annotations == in.annotations);
  REQUIRE(out.records.size() == in.records.size());
  for (std::size_t i = 0; i < in.records.size(); ++i) {
    const Observation& a = in.records[i].observation;
    const Observation& b = out.records[i].observation;
    CHECK(a.id == b.id);
    CHECK(a.source == b.source);
    CHECK(a.domain == b.domain);
    CHECK(a.domain_kind == b.domain_kind);
    CHECK(a.timestamp == b.timestamp);
    CHECK(a.generation == b.generation);
    CHECK(a.epoch == b.epoch);
    CHECK(a.provenance.mechanism == b.provenance.mechanism);
    CHECK(a.provenance.confidence == b.provenance.confidence);
    CHECK(a.annotations == b.annotations);
    CHECK(a.payload == b.payload);
    if (!(a.payload == b.payload)) std::printf("  payload of record %zu differs\n", i);
  }
}

TEST(trace_json_rejects_values_outside_the_schema) {
  const char* bad[] = {
      "{}",
      "{\"version\":\"1\"}",
      "{\"version\":1.5}",
      "{\"version\":4294967297}",
      "{\"version\":1,\"records\":{}}",
      "{\"version\":1,\"records\":[5]}",
      "{\"version\":1,\"records\":[{\"id\":-1}]}",
      "{\"version\":1,\"records\":[{\"id\":1.5}]}",
      "{\"version\":1,\"records\":[{\"source\":1e30}]}",
      "{\"version\":1,\"records\":[{\"mono\":-7}]}",
      "{\"version\":1,\"records\":[{\"mono\":\"soon\"}]}",
      "{\"version\":1,\"records\":[{\"domain_kind\":\"moon_base\"}]}",
      "{\"version\":1,\"records\":[{\"mech\":\"imagined\"}]}",
      "{\"version\":1,\"records\":[{\"confidence\":7}]}",
      "{\"version\":1,\"records\":[{\"payload\":7}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"wormhole\"}}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"metric\",\"mtype\":\"gauge\",\"kind\":\"bogus\"}}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"metric\",\"mtype\":\"sundial\"}}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"metric\",\"mtype\":\"counter\",\"value\":-3}}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"event\",\"severity\":\"apocalyptic\"}}]}",
      "{\"version\":1,\"records\":[{\"payload\":{\"type\":\"domain_state\",\"physical\":\"lots\"}}]}",
      "{\"version\":1,\"annotations\":7}",
      "{\"version\":1,\"magic\":7}",
  };
  for (const char* text : bad) {
    const bool rejected = !deserialize_trace_json(text).ok();
    if (!rejected) std::printf("  accepted: %s\n", text);
    CHECK(rejected);
  }
  // The same shapes with values inside the schema are accepted, exactly.
  auto good = deserialize_trace_json(
      "{\"version\":1,\"records\":[{\"id\":18446744073709551615,\"mono\":0,"
      "\"payload\":{\"type\":\"metric\",\"mtype\":\"counter\",\"kind\":\"live\","
      "\"value\":18446744073709551615}}]}");
  REQUIRE(good.ok());
  REQUIRE(good.value().records.size() == 1);
  const Observation& o = good.value().records[0].observation;
  CHECK_EQ(o.id.value(), std::numeric_limits<std::uint64_t>::max());
  const auto* ms = std::get_if<MetricSample>(&o.payload);
  REQUIRE(ms != nullptr);
  const auto* c = std::get_if<CounterSample>(ms);
  REQUIRE(c != nullptr);
  CHECK(c->kind == MemoryKind::kLive);
  REQUIRE(c->value.has_value());
  CHECK_EQ(*c->value, std::numeric_limits<std::uint64_t>::max());
}

TEST(trace_json_strings_are_strict) {
  // A raw control character inside a string is not JSON.
  CHECK(!deserialize_trace_json(std::string("{\"version\":1,\"magic\":\"a\nb\"}")).ok());
  // Half of a surrogate pair is not a character.
  CHECK(!deserialize_trace_json("{\"version\":1,\"magic\":\"\\uD83D\"}").ok());
  CHECK(!deserialize_trace_json("{\"version\":1,\"magic\":\"\\uDE00\"}").ok());
  CHECK(!deserialize_trace_json("{\"version\":1,\"magic\":\"\\uD83Dx\"}").ok());
  // A whole pair is one character: four UTF-8 bytes.
  auto pair = deserialize_trace_json("{\"version\":1,\"magic\":\"\\uD83D\\uDE00\"}");
  REQUIRE(pair.ok());
  CHECK_EQ(pair.value().format, std::string("\xF0\x9F\x98\x80"));
  // Control characters are escaped on the way out and come back intact.
  Trace t;
  t.format = std::string("tab\there\x01");
  auto ser = serialize_trace_json(t);
  REQUIRE(ser.ok());
  auto back = deserialize_trace_json(ser.value());
  REQUIRE(back.ok());
  CHECK_EQ(back.value().format, t.format);
}

TEST(trace_json_enforces_annotation_bounds) {
  auto with_annotations = [](std::size_t n, std::size_t key_bytes) {
    std::string s = "{\"version\":1,\"records\":[{\"id\":1,\"annotations\":[";
    for (std::size_t i = 0; i < n; ++i) {
      if (i) s += ",";
      s += "{\"k\":\"" + std::string(key_bytes, 'k') + "\",\"v\":\"v\"}";
    }
    s += "]}]}";
    return s;
  };
  CHECK(deserialize_trace_json(with_annotations(4, 8)).ok());
  CHECK(!deserialize_trace_json(with_annotations(300, 8)).ok());   // over max_annotation_count
  CHECK(!deserialize_trace_json(with_annotations(1, 5000)).ok());  // over max_annotation_bytes
  // The trace's own annotations are bounded the same way, not dropped quietly.
  std::string top = "{\"version\":1,\"annotations\":[";
  for (int i = 0; i < 300; ++i) {
    if (i) top += ",";
    top += "{\"k\":\"a\",\"v\":\"b\"}";
  }
  top += "]}";
  CHECK(!deserialize_trace_json(top).ok());
  TraceLimits tight;
  tight.max_records = 1;
  CHECK(!deserialize_trace_json("{\"version\":1,\"records\":[{\"id\":1},{\"id\":2}]}", tight).ok());
}

TEST(replayed_observations_are_marked_replayed) {
  Trace in = make_trace(3);
  Observatory obs;
  ReplayResult rr = replay_trace(obs, in, {});
  CHECK_EQ(rr.accepted, std::uint64_t{3});
  auto tl = obs.query_timeline(TimelineFilter{});
  REQUIRE(tl.ok());
  REQUIRE(tl.value().size() == 3);
  for (const auto& e : tl.value().entries) {
    CHECK(e.observation.provenance.mechanism == ObservationMechanism::kReplayed);
  }
}

TEST(replay_does_not_report_a_just_replayed_source_as_stale) {
  // The recorded timestamps are one second after the steady clock's origin: a
  // different clock domain, far in this process's past.
  Trace in = make_trace(6);
  Observatory obs;
  ReplayResult rr = replay_trace(obs, in, {});
  bool stale = false;
  for (const auto& d : rr.diagnostics) {
    if (d.code == DiagnosticCode::kStaleTelemetry) stale = true;
  }
  CHECK(!stale);
}
