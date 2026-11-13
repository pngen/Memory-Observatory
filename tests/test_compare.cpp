// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <cmath>
#include <vector>

#include "memory_observatory/compare.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

namespace {

// Build a deterministic trace of gauges.
Trace make_trace(std::uint64_t source_val, std::uint64_t domain_val, std::uint64_t gen,
                 MemoryKind kind, double start, double step, std::uint64_t n = 20) {
  Trace t;
  t.id = TraceId{1};
  t.session = SessionId{2};
  const MonotonicTimestamp base{1000000000LL};
  for (std::uint64_t i = 0; i < n; ++i) {
    TraceRecord rec;
    Observation& o = rec.observation;
    o.source = SourceId{source_val};
    o.domain = DomainId{domain_val};
    o.domain_kind = DomainKind::kHostRam;
    o.timestamp.monotonic = base + Duration{1000000LL * static_cast<std::int64_t>(i)};
    o.timestamp.sequence = SequenceNo{static_cast<std::int64_t>(i + 1)};
    o.generation = Generation{gen};
    o.provenance.source = SourceId{source_val};
    o.provenance.source_name = "src";
    o.provenance.mechanism = ObservationMechanism::kLive;
    GaugeSample g;
    g.kind = kind;
    g.value = start + static_cast<double>(i) * step;
    o.payload = g;
    t.records.push_back(std::move(rec));
  }
  return t;
}

}  // namespace

TEST(compare_identical_traces_are_empty) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = a;  // identical copy
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(c.value().identical_inputs);
  CHECK(c.value().findings.empty());
  CHECK(c.value().left_observations == c.value().right_observations);
  CHECK(c.value().left_observations == 20);
}

TEST(compare_deterministic_for_identical_inputs) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = a;
  auto c1 = compare_traces(a, b);
  REQUIRE(c1.ok());
  auto c2 = compare_traces(a, b);
  REQUIRE(c2.ok());
  CHECK(render_compare_json(c1.value()) == render_compare_json(c2.value()));
  CHECK(render_compare_text(c1.value()) == render_compare_text(c2.value()));
}

TEST(compare_materially_different_traces) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);   // 1000..1950
  Trace b = make_trace(100, 5, 7, MemoryKind::kAllocated, 5000, 0);    // constant 5000
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(!c.value().identical_inputs);
  CHECK(!c.value().findings.empty());
  bool found_delta = false;
  for (const auto& md : c.value().metrics) {
    if (md.kind == MemoryKind::kAllocated && md.mean_delta) {
      found_delta = true;
      CHECK(std::fabs(*md.mean_delta) > 1000.0);
    }
  }
  CHECK(found_delta);
}

TEST(compare_missing_metrics) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = make_trace(100, 5, 7, MemoryKind::kAvailable, 1000, 50);  // different kind
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(!c.value().identical_inputs);
  bool only_left = false, only_right = false;
  for (const auto& md : c.value().metrics) {
    if (md.kind == MemoryKind::kAllocated && md.left_count > 0 && md.right_count == 0) only_left = true;
    if (md.kind == MemoryKind::kAvailable && md.right_count > 0 && md.left_count == 0) only_right = true;
  }
  CHECK(only_left);
  CHECK(only_right);
}

TEST(compare_differing_generations_and_provenance) {
  // Same data but a different source generation -> provenance differs.
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = make_trace(100, 5, 8, MemoryKind::kAllocated, 1000, 50);
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(!c.value().provenance_diff.empty());
}

TEST(compare_differing_domain_sets) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = make_trace(100, 9, 7, MemoryKind::kAllocated, 1000, 50);  // domain 9
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(!c.value().domains_only_left.empty() || !c.value().domains_only_right.empty());
}

TEST(compare_empty_traces_are_identical) {
  Trace a, b;
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(c.value().identical_inputs);
  CHECK(c.value().left_observations == 0);
  CHECK(c.value().right_observations == 0);
}

TEST(compare_large_bounded_traces) {
  Trace a = make_trace(1, 5, 7, MemoryKind::kCommitted, 0, 100, 20000);
  Trace b = make_trace(1, 5, 7, MemoryKind::kCommitted, 0, 101, 20000);
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(c.value().left_observations == 20000);
  CHECK(c.value().right_observations == 20000);
  // A small but real mean delta exists; comparison completes bounded.
  bool ok = false;
  for (const auto& md : c.value().metrics) {
    if (md.kind == MemoryKind::kCommitted && md.mean_delta && *md.mean_delta > 0) ok = true;
  }
  CHECK(ok);
}

TEST(compare_renders_text_json_csv) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = make_trace(100, 5, 8, MemoryKind::kAvailable, 2000, 10);
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  const auto text = render_compare_text(c.value());
  const auto json = render_compare_json(c.value());
  const auto csv = render_compare_csv(c.value());
  CHECK(!text.empty());
  CHECK(!json.empty());
  CHECK(!csv.empty());
  CHECK(json.find("\"findings\"") != std::string::npos);
  CHECK(csv.find("category,kind") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Whole-trace coverage, identical-input claims, JSON rendering.
// ---------------------------------------------------------------------------

#include <string>

TEST(compare_covers_traces_longer_than_the_default_history) {
  // 100,000 is the Observatory's default history bound.
  const std::uint64_t n = 100050;
  Trace a = make_trace(1, 5, 7, MemoryKind::kCommitted, 1000, 0, n);
  Trace b = a;
  // The traces differ only at the very beginning.
  for (std::size_t i = 0; i < 40; ++i) {
    GaugeSample g;
    g.kind = MemoryKind::kCommitted;
    g.value = 900000.0;
    b.records[i].observation.payload = g;
  }
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK_EQ(c.value().left_observations, n);
  CHECK_EQ(c.value().right_observations, n);
  CHECK(!c.value().identical_inputs);
  bool mean_differs = false;
  for (const auto& md : c.value().metrics) {
    if (md.kind == MemoryKind::kCommitted && md.mean_delta && *md.mean_delta > 1.0) {
      mean_differs = true;
    }
  }
  CHECK(mean_differs);
}

TEST(compare_does_not_call_different_records_identical) {
  auto set_gauge = [](Trace& t, std::size_t i, double v) {
    GaugeSample g;
    g.kind = MemoryKind::kAllocated;
    g.value = v;
    t.records[i].observation.payload = g;
  };
  // The same counts and the same mean, from different observations.
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 0, 2);
  Trace b = a;
  set_gauge(a, 0, 3000);
  set_gauge(a, 1, 1000);
  set_gauge(b, 0, 2000);
  set_gauge(b, 1, 2000);
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  CHECK(!c.value().identical_inputs);
  CHECK(!c.value().findings.empty());

  // State payloads are not metrics, so no metric summary sees them differ.
  Trace s1 = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 0, 1);
  Trace s2 = s1;
  DomainState ds;
  ds.domain = DomainId{5};
  ds.capacity.physical = 1000;
  ds.capacity.available = 900;
  s1.records[0].observation.payload = ds;
  ds.capacity.available = 800;
  s2.records[0].observation.payload = ds;
  auto c2 = compare_traces(s1, s2);
  REQUIRE(c2.ok());
  CHECK(!c2.value().identical_inputs);
}

TEST(compare_json_is_well_formed) {
  Trace a = make_trace(100, 5, 7, MemoryKind::kAllocated, 1000, 50);
  Trace b = make_trace(100, 5, 8, MemoryKind::kAvailable, 2000, 10);
  auto c = compare_traces(a, b);
  REQUIRE(c.ok());
  const std::string json = render_compare_json(c.value());
  CHECK(json.find("\"observations\":[20,20]") != std::string::npos);
  // Every object opens on a key or is empty: none holds bare values.
  bool bare = false;
  for (std::size_t i = 0; i + 1 < json.size(); ++i) {
    if (json[i] == '{' && json[i + 1] != '"' && json[i + 1] != '}') bare = true;
  }
  CHECK(!bare);
}
