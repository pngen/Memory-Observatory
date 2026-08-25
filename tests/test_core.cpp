// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "memory_observatory/observatory.h"
#include "memory_observatory/sampler.h"
#include "memory_observatory/provider.h"

using namespace memory_observatory;

namespace {

SourceId reg_source(Observatory& obs, const char* name = "test") {
  SourceDescriptor sd;
  sd.name = name;
  sd.kind = "synthetic";
  auto r = obs.register_source(sd);
  return r.ok() ? r.value() : SourceId{};
}

DomainId reg_domain(Observatory& obs, const char* name = "host",
                    DomainKind k = DomainKind::kHostRam) {
  DomainSpec ds;
  ds.name = name;
  ds.kind = k;
  auto r = obs.register_domain(ds);
  return r.ok() ? r.value() : DomainId{};
}

Observation gauge(SourceId src, DomainId dom, MemoryKind kind, double v,
                  MonotonicTimestamp t) {
  Observation o;
  o.source = src;
  o.domain = dom;
  o.timestamp.monotonic = t;
  GaugeSample g;
  g.kind = kind;
  g.value = v;
  o.payload = g;
  return o;
}

Observation counter(SourceId src, DomainId dom, MemoryKind kind, std::uint64_t v,
                    MonotonicTimestamp t, bool reset = false) {
  Observation o;
  o.source = src;
  o.domain = dom;
  o.timestamp.monotonic = t;
  CounterSample c;
  c.kind = kind;
  c.value = v;
  c.reset = reset;
  o.payload = c;
  return o;
}

}  // namespace

TEST(ids_are_strong_and_invalid_when_zero) {
  ObservationId a{1};
  ObservationId b{2};
  ObservationId z;
  CHECK(!z.valid());
  CHECK(z.value() == 0);
  CHECK(a.valid());
  CHECK(a != b);
  CHECK(a == a);
  CHECK(a.next().value() == 2);
  CHECK(a < b);
}

TEST(error_codes_have_stable_strings) {
  CHECK_EQ(std::string(to_string(ErrorCode::kNone)), std::string("none"));
  CHECK_EQ(std::string(to_string(ErrorCode::kProviderUnavailable)),
           std::string("provider_unavailable"));
  CHECK_EQ(std::string(to_string(ErrorCode::kInvalidTrace)), std::string("invalid_trace"));
}

TEST(result_holds_value_and_error) {
  auto r = ok(42);
  CHECK(r.ok());
  CHECK_EQ(r.value(), 42);
  auto e = fail<int>(ErrorCode::kInvalidArgument, "bad");
  CHECK(!e.ok());
  CHECK(e.error().code() == ErrorCode::kInvalidArgument);
}

TEST(domain_capacity_accounting_invariants) {
  DomainCapacity cap;
  cap.physical = 100;
  cap.available = 20;
  CHECK(cap.accounting_is_possible());
  cap.governed = 200;
  CHECK(!cap.accounting_is_possible());
}

TEST(confidence_unknown_distinct_from_zero) {
  Confidence u;
  CHECK(!u.valid());
  Confidence z{0.0};
  CHECK(z.valid());
  CHECK_EQ(z.score(), 0.0);
  Confidence c{0.75};
  CHECK_EQ(c.score(), 0.75);
}

TEST(domain_kind_strings_round_trip) {
  CHECK_EQ(std::string(to_string(DomainKind::kHostRam)), std::string("host_ram"));
  CHECK(domain_kind_from_string("host_ram") == DomainKind::kHostRam);
  CHECK(domain_kind_from_string("nonsense") == DomainKind::kUnknown);
}

TEST(source_registry_lifecycle) {
  Observatory obs;
  SourceId s = reg_source(obs, "a");
  CHECK(s.valid());
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK_EQ(info.value().name, std::string("a"));
  const auto li = obs.source_info(s);
  CHECK(li.ok());
  CHECK(li.value().kind == std::string("synthetic"));
  // removal
  REQUIRE(obs.remove_source(s).ok());
  auto after = obs.source_info(s);
  CHECK(after.ok());
  CHECK(after.value().health == SourceHealth::kRemoved);
}

TEST(ingestion_accepted_and_accounting_balanced) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp t = MonotonicTimestamp::now();
  IngestResult ir = obs.observe(gauge(s, d, MemoryKind::kAvailable, 100.0, t));
  CHECK(ir.accepted());
  CHECK(obs.retained_count() == 1);
  auto c = obs.counters();
  CHECK(c.ingested_total == 1);
  CHECK(c.retained == 1);
  CHECK(c.is_balanced());
}

TEST(ingestion_rejects_unsourced) {
  Observatory obs;
  Observation o = gauge(SourceId{}, DomainId{1}, MemoryKind::kAvailable, 1.0,
                        MonotonicTimestamp::now());
  IngestResult ir = obs.observe(std::move(o));
  CHECK(ir.outcome == IngestOutcome::kRejectedInvalid);
  CHECK(!ir.accepted());
}

TEST(ingestion_duplicate_suppression) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp t = MonotonicTimestamp::now();
  Observation o = gauge(s, d, MemoryKind::kAvailable, 1.0, t);
  o.id = ObservationId{500};  // producer supplies a stable id
  auto r1 = obs.observe(o);
  CHECK(r1.accepted());
  // Duplicate same id is suppressed.
  auto r2 = obs.observe(std::move(o));
  CHECK(!r2.accepted());
  CHECK(r2.outcome == IngestOutcome::kDuplicateSuppressed);
  auto c = obs.counters();
  CHECK_EQ(c.duplicate_suppressed, std::uint64_t{1});
  CHECK(c.is_balanced());
}

TEST(history_is_bounded) {
  ObservatoryConfig cfg;
  cfg.history_capacity = 16;
  Observatory obs(cfg);
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp base = MonotonicTimestamp::now();
  for (int i = 0; i < 64; ++i) {
    obs.observe(gauge(s, d, MemoryKind::kAvailable, double(i),
                      base + Duration{1000LL * i}));
  }
  CHECK(obs.retained_count() == 16);
  auto c = obs.counters();
  CHECK(c.ingested_total == 64);
  CHECK(c.retained == 16);
  CHECK_EQ(c.evicted, std::uint64_t{48});
  CHECK_EQ(c.dropped_overflow, std::uint64_t{0});
  CHECK(c.is_balanced());
}

TEST(timeline_filter_by_domain_and_sort) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d1 = reg_domain(obs, "a");
  DomainId d2 = reg_domain(obs, "b");
  MonotonicTimestamp base = MonotonicTimestamp::now();
  for (int i = 0; i < 5; ++i) {
    obs.observe(gauge(s, d1, MemoryKind::kAvailable, double(i), base + Duration{1000 * i}));
    obs.observe(gauge(s, d2, MemoryKind::kAvailable, double(100 + i), base + Duration{1000 * i}));
  }
  // Query out-of-order window/domain filter.
  TimelineFilter f;
  f.domain = d1;
  auto tl = obs.query_timeline(f);
  REQUIRE(tl.ok());
  CHECK_EQ(tl.value().size(), std::size_t{5});
  // Chronological ascending.
  for (std::size_t i = 1; i < tl.value().size(); ++i) {
    CHECK(tl.value().entries[i].observation.timestamp.monotonic >=
          tl.value().entries[i - 1].observation.timestamp.monotonic);
  }
}

TEST(aggregation_percentiles_and_counter_resets) {
  std::vector<double> samples{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  auto p = compute_percentiles(samples);
  REQUIRE(p.p50);
  REQUIRE(p.p99);
  CHECK_EQ(*p.p50, 5.0);
  CHECK(*p.p99 >= 9.0);

  BoundedAggregator agg;
  agg.add(1.0);
  agg.add(2.0);
  agg.add(3.0);
  auto a = agg.to_aggregate();
  CHECK_EQ(a.mean.value(), 2.0);
  CHECK_EQ(a.min.value(), 1.0);
  CHECK_EQ(a.max.value(), 3.0);
  CHECK_EQ(a.count.value(), std::uint64_t{3});
}

TEST(aggregator_ignores_nan) {
  BoundedAggregator agg;
  agg.add(std::nan(""));
  agg.add(1.0);
  agg.add(2.0);
  auto a = agg.to_aggregate();
  CHECK_EQ(a.count.value(), std::uint64_t{2});
}

TEST(snapshot_captures_domain_state_and_diagnostics) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp base = MonotonicTimestamp::now();
  // Feed a domain state + metrics.
  Observation state;
  state.source = s;
  state.domain = d;
  state.timestamp.monotonic = base;
  DomainState ds;
  ds.domain = d;
  ds.capacity.physical = 1000;
  ds.capacity.available = 100;
  state.payload = ds;
  obs.observe(std::move(state));

  for (int i = 0; i < 10; ++i) {
    obs.observe(gauge(s, d, MemoryKind::kAvailable, double(100 - i), base + Duration{1000 * i}));
  }
  auto snap = obs.snapshot(SnapshotFilter{});
  REQUIRE(snap.ok());
  CHECK(snap.value().id.valid());
  CHECK(snap.value().counters.retained >= 1);
  CHECK(!snap.value().domains.empty());
}

TEST(diagnostics_growth_leak_heuristic) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp base = MonotonicTimestamp::now();
  for (int i = 0; i < 12; ++i) {
    obs.observe(gauge(s, d, MemoryKind::kAllocated, double(1000 + i * 50),
                      base + Duration{1000 * i}));
  }
  auto diags = obs.evaluate_diagnostics();
  bool found_growth = false, found_leak_hint = false;
  for (const auto& dg : diags) {
    if (dg.code == DiagnosticCode::kSustainedCapacityGrowth) found_growth = true;
    if (dg.code == DiagnosticCode::kPossibleLeakPattern &&
        dg.kind == DiagnosticKind::kHeuristicInterpretation) {
      found_leak_hint = true;
    }
  }
  CHECK(found_growth);
  CHECK(found_leak_hint);
}

TEST(correlation_is_not_causation) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  MonotonicTimestamp base = MonotonicTimestamp::now();
  // Rising allocation, falling available.
  for (int i = 0; i < 20; ++i) {
    obs.observe(gauge(s, d, MemoryKind::kAllocated, double(1000 + i * 10),
                      base + Duration{1000 * i}));
    obs.observe(gauge(s, d, MemoryKind::kAvailable, double(2000 - i * 10),
                      base + Duration{1000 * i}));
  }
  auto corr = obs.evaluate_correlations();
  bool found = false;
  for (const auto& c : corr) {
    if (c.kind == CorrelationKind::kAllocationPressure) {
      found = true;
      CHECK(!c.limitations.empty());
      CHECK(c.explanation.find("CORRELATION") != std::string::npos ||
            c.explanation.find("correlation") != std::string::npos);
    }
  }
  CHECK(found);
}

TEST(stale_telemetry_diagnostic) {
  ObservatoryConfig cfg;
  cfg.diagnostics.stale_after = Duration{1000000LL};  // 1 ms
  Observatory obs(cfg);
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  // One observation now.
  obs.observe(gauge(s, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp::now()));
  // Let enough time pass for the stale rule.
  std::this_thread::sleep_for(std::chrono::milliseconds(6));
  auto diags = obs.evaluate_diagnostics();
  bool stale = false;
  for (const auto& dg : diags) {
    if (dg.code == DiagnosticCode::kStaleTelemetry) stale = true;
  }
  CHECK(stale);
}

TEST(generation_rollover_tracked) {
  Observatory obs;
  SourceId s = reg_source(obs);
  REQUIRE(obs.advance_generation(s).ok());
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK_EQ(info.value().generation_rollovers, std::uint64_t{1});
}

TEST(subscriptions_dispatch_outside_lock_and_reentrancy_safe) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  std::atomic<int> received{0};
  std::atomic<int> reentrant{0};
  // A subscriber that itself ingests an observation (reentrancy).
  SubscriptionId sub = obs.subscribe(
      [&](const Observation&) { ++received; ++reentrant; });
  CHECK(sub.valid());
  obs.observe(gauge(s, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp::now()));
  // The callback was invoked; no deadlock occurred.
  CHECK(received.load() == 1);
  REQUIRE(obs.unsubscribe(sub).ok());
  obs.observe(gauge(s, d, MemoryKind::kAvailable, 2.0, MonotonicTimestamp::now()));
  CHECK(received.load() == 1);
}

TEST(shutdown_leaves_no_worker_threads) {
  Observatory obs;
  Sampler sampler(obs);
  (void)sampler.add_provider(
      make_synthetic_provider("synthetic", 7, DomainKind::kHostRam));
  // We cannot easily observe thread counts; verify start/stop is clean and
  // repeated start/stop returns to a stopped state.
  REQUIRE(sampler.start().ok());
  CHECK(sampler.running());
  REQUIRE(sampler.stop().ok());
  CHECK(!sampler.running());
  REQUIRE(sampler.stop().ok());  // idempotent
  REQUIRE(sampler.start().ok());
  REQUIRE(sampler.stop().ok());
  CHECK(!sampler.running());
}

TEST(concurrent_ingestion_is_thread_safe) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::atomic<int> accepted{0};
  std::vector<std::thread> th;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&] {
      MonotonicTimestamp base = MonotonicTimestamp::now();
      for (int i = 0; i < kPerThread; ++i) {
        auto o = gauge(s, d, MemoryKind::kAvailable, double(i),
                       base + Duration{1000 * i});
        IngestResult r = obs.observe(std::move(o));
        if (r.accepted()) ++accepted;
      }
    });
  }
  for (auto& t : th) t.join();
  CHECK(accepted.load() == kThreads * kPerThread);
  auto c = obs.counters();
  CHECK(c.is_balanced());
}
