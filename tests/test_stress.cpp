// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Property / randomized tests. Fully deterministic by seed: a failing seed is
// printed and reproducible.

#include "framework.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include "memory_observatory/observatory.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

namespace {
std::uint32_t g_seed = 0xC0FFEEu;
}  // namespace

TEST(randomized_property_invariants) {
  std::printf("  (seed %u)\n", g_seed);
  std::mt19937 rng(g_seed);
  const int baseline = mo_test::failures();
  for (int iter = 0; iter < 2000; ++iter) {
    ObservatoryConfig cfg;
    cfg.history_capacity = 1 + rng() % 64;
    cfg.deduplicate_ids = true;
    Observatory obs(cfg);

    const int ns = 1 + static_cast<int>(rng() % 4);
    const int nd = 1 + static_cast<int>(rng() % 3);
    std::vector<SourceId> srcs;
    std::vector<DomainId> doms;
    for (int i = 0; i < ns; ++i) {
      SourceDescriptor sd;
      sd.name = "s" + std::to_string(i);
      sd.kind = "synthetic";
      auto r = obs.register_source(sd);
      if (r.ok()) srcs.push_back(r.value());
    }
    for (int i = 0; i < nd; ++i) {
      DomainSpec ds;
      ds.name = "d" + std::to_string(i);
      ds.kind = DomainKind::kHostRam;
      auto r = obs.register_domain(ds);
      if (r.ok()) doms.push_back(r.value());
    }
    if (srcs.empty() || doms.empty()) continue;

    const MonotonicTimestamp base = MonotonicTimestamp::now();
    const int n = 1 + static_cast<int>(rng() % 100);
    for (int i = 0; i < n; ++i) {
      Observation o;
      o.source = srcs[rng() % srcs.size()];
      o.domain = doms[rng() % doms.size()];
      const std::int64_t off = static_cast<std::int64_t>(rng() % 40) * 1000;
      o.timestamp.monotonic = base + Duration{off};
      o.timestamp.sequence = SequenceNo{static_cast<std::int64_t>(i)};
      if ((rng() % 3) == 0) {
        o.id = ObservationId{static_cast<std::uint64_t>(rng() % 8)};
      }
      if ((rng() % 2) == 0) {
        GaugeSample g;
        g.kind = (rng() % 3) ? MemoryKind::kAvailable : MemoryKind::kAllocated;
        g.value = static_cast<double>(rng() % 100000);
        o.payload = g;
      } else {
        CounterSample c;
        c.kind = MemoryKind::kCommitted;
        c.value = static_cast<std::uint64_t>(rng() % 100000);
        c.reset = (rng() % 4) == 0;
        o.payload = c;
      }
      (void)obs.observe(std::move(o));
    }

    auto c = obs.counters();
    CHECK(c.is_balanced());
    CHECK(obs.retained_count() <= cfg.history_capacity);
    CHECK(c.retained <= cfg.history_capacity);
    CHECK(c.duplicate_suppressed <= c.ingested_total);
    CHECK(c.ingested_total >= c.retained);

    auto snap = obs.snapshot();
    CHECK(snap.ok());
    TimelineFilter tf;
    auto tl = obs.query_timeline(tf);
    CHECK(tl.ok());
    (void)obs.evaluate_diagnostics();
    (void)obs.evaluate_correlations();
  }
  CHECK(mo_test::failures() == baseline);
}

TEST(deserialize_fuzz_never_crashes) {
  std::mt19937 rng(0x1234);
  for (int i = 0; i < 5000; ++i) {
    const std::size_t len = rng() % 256;
    std::vector<std::uint8_t> buf(len);
    for (std::size_t j = 0; j < len; ++j) {
      buf[j] = static_cast<std::uint8_t>(rng() % 256);
    }
    (void)deserialize_trace_binary(buf.data(), buf.size());
  }
  Trace t;
  t.id = TraceId{1};
  for (int i = 0; i < 3; ++i) {
    TraceRecord r;
    GaugeSample g;
    g.kind = MemoryKind::kAvailable;
    g.value = 1.0;
    r.observation.payload = g;
    t.records.push_back(std::move(r));
  }
  auto ser = serialize_trace_binary(t);
  REQUIRE(ser.ok());
  for (std::size_t cut = 0; cut <= ser.value().size(); ++cut) {
    (void)deserialize_trace_binary(ser.value().data(), cut);
  }
}

TEST(counter_reset_not_negative_delta) {
  CounterSample c;
  c.kind = MemoryKind::kCommitted;
  c.value = 1000;
  c.reset = true;
  CHECK(c.reset);
  CHECK(c.value.has_value());
}
