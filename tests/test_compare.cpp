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
