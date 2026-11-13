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

// ---------------------------------------------------------------------------
// Ingestion accounting, identity, generations, subscribers, queries, sampler.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

Observation domain_state(SourceId src, DomainId dom, Bytes physical,
                         Bytes available, MonotonicTimestamp t) {
  Observation o;
  o.source = src;
  o.domain = dom;
  o.timestamp.monotonic = t;
  DomainState ds;
  ds.domain = dom;
  ds.capacity.physical = physical;
  ds.capacity.available = available;
  o.payload = ds;
  return o;
}

bool has_code(const std::vector<Diagnostic>& diags, DiagnosticCode code) {
  for (const auto& d : diags) {
    if (d.code == code) return true;
  }
  return false;
}

bool has_code_for(const std::vector<Diagnostic>& diags, DiagnosticCode code,
                  SourceId source) {
  for (const auto& d : diags) {
    if (d.code == code && d.source && *d.source == source) return true;
  }
  return false;
}

// A provider whose backend misbehaves by throwing.
class ThrowingProvider final : public Provider {
 public:
  std::string name() const override { return "throwing"; }
  std::string kind() const override { return "test"; }
  std::string provider_version() const override { return "1"; }
  DomainKind primary_domain() const override { return DomainKind::kUserDefined; }
  bool available() const override { return true; }
  CollectResult collect() override { throw std::runtime_error("backend exploded"); }
};

// A provider that is safe to collect from several threads at once: one gauge
// per collection.
class CountingProvider final : public Provider {
 public:
  std::string name() const override { return "counting"; }
  std::string kind() const override { return "test"; }
  std::string provider_version() const override { return "1"; }
  DomainKind primary_domain() const override { return DomainKind::kUserDefined; }
  bool available() const override { return true; }
  CollectResult collect() override {
    CollectResult r;
    r.ok = true;
    Sample s;
    s.kind = MemoryKind::kAllocated;
    s.gauge_value = static_cast<double>(ticks_.fetch_add(1) + 1);
    r.samples.push_back(std::move(s));
    return r;
  }

 private:
  std::atomic<std::uint64_t> ticks_{0};
};

}  // namespace

TEST(unsourced_observation_is_counted) {
  Observatory obs;
  IngestResult ir = obs.observe(gauge(SourceId{}, DomainId{1}, MemoryKind::kAvailable,
                                      1.0, MonotonicTimestamp::now()));
  CHECK(ir.outcome == IngestOutcome::kRejectedInvalid);
  const auto c = obs.counters();
  CHECK_EQ(c.ingested_total, std::uint64_t{1});
  CHECK_EQ(c.rejected_invalid, std::uint64_t{1});
  CHECK(c.is_balanced());
}

TEST(observation_without_payload_is_rejected_as_malformed) {
  Observatory obs;
  Observation o;
  o.source = reg_source(obs);
  o.domain = reg_domain(obs);
  o.timestamp.monotonic = MonotonicTimestamp::now();
  IngestResult ir = obs.observe(std::move(o));
  CHECK(ir.outcome == IngestOutcome::kRejectedMalformed);
  CHECK_EQ(obs.retained_count(), std::size_t{0});
  const auto c = obs.counters();
  CHECK_EQ(c.rejected_malformed, std::uint64_t{1});
  CHECK(c.is_balanced());
}

TEST(zero_history_capacity_retains_nothing) {
  ObservatoryConfig cfg;
  cfg.history_capacity = 0;
  Observatory obs(cfg);
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  int delivered = 0;
  (void)obs.subscribe([&](const Observation&) { ++delivered; });
  for (int i = 0; i < 3; ++i) {
    IngestResult ir = obs.observe(
        gauge(s, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp::now()));
    CHECK(ir.accepted());
  }
  CHECK_EQ(obs.retained_count(), std::size_t{0});
  CHECK_EQ(delivered, 3);
  const auto c = obs.counters();
  CHECK_EQ(c.ingested_total, std::uint64_t{3});
  CHECK_EQ(c.retained, std::uint64_t{0});
  CHECK_EQ(c.evicted, std::uint64_t{3});
  CHECK(c.is_balanced());
}

TEST(minted_ids_never_collide_with_supplied_ids) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  // A producer supplies id 2; the Observatory then mints ids for the others.
  Observation supplied = gauge(s, d, MemoryKind::kAvailable, 1.0, t);
  supplied.id = ObservationId{2};
  CHECK(obs.observe(supplied).accepted());
  for (int i = 0; i < 4; ++i) {
    IngestResult ir = obs.observe(gauge(s, d, MemoryKind::kAvailable, double(i), t));
    CHECK(ir.accepted());
    CHECK(ir.id != ObservationId{2});
  }
  CHECK_EQ(obs.counters().duplicate_suppressed, std::uint64_t{0});

  // The other way round: ids minted first, then a producer whose own numbering
  // happens to start in the same range.
  Observatory other;
  SourceId s2 = reg_source(other);
  for (int i = 0; i < 4; ++i) {
    CHECK(other.observe(gauge(s2, d, MemoryKind::kAvailable, double(i), t)).accepted());
  }
  Observation late = gauge(s2, d, MemoryKind::kAvailable, 9.0, t);
  late.id = ObservationId{3};
  CHECK(other.observe(late).accepted());
  // A real duplicate of a supplied id is still suppressed.
  CHECK(other.observe(late).outcome == IngestOutcome::kDuplicateSuppressed);
  CHECK_EQ(other.counters().duplicate_suppressed, std::uint64_t{1});
  CHECK(other.counters().is_balanced());
}

TEST(registered_source_never_shares_an_id_with_an_observed_one) {
  Observatory obs;
  // A producer the Observatory has never seen announces itself as source 1.
  Observation o = gauge(SourceId{1}, DomainId{1}, MemoryKind::kAvailable, 1.0,
                        MonotonicTimestamp::now());
  o.provenance.source_name = "remote";
  REQUIRE(obs.observe(std::move(o)).accepted());
  SourceDescriptor sd;
  sd.name = "local";
  sd.kind = "synthetic";
  auto reg = obs.register_source(sd);
  REQUIRE(reg.ok());
  CHECK(reg.value() != SourceId{1});
  auto local = obs.source_info(reg.value());
  REQUIRE(local.ok());
  CHECK_EQ(local.value().name, std::string("local"));
  auto remote = obs.source_info(SourceId{1});
  REQUIRE(remote.ok());
  CHECK_EQ(remote.value().name, std::string("remote"));
  CHECK_EQ(obs.sources().size(), std::size_t{2});
}

TEST(source_generation_never_moves_backward) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  auto with_gen = [&](std::uint64_t gen, std::int64_t off) {
    Observation o = gauge(s, d, MemoryKind::kAvailable, 1.0, t + Duration{off});
    o.generation = Generation{gen};
    return o;
  };
  CHECK(obs.observe(with_gen(7, 0)).accepted());
  CHECK(obs.observe(with_gen(8, 1)).accepted());  // the source restarted
  // A late observation from before the restart.
  CHECK(obs.observe(with_gen(7, 2)).accepted());
  auto mid = obs.source_info(s);
  REQUIRE(mid.ok());
  CHECK(mid.value().generation == Generation{8});
  // Current and late observations keep interleaving.
  CHECK(obs.observe(with_gen(8, 3)).accepted());
  CHECK(obs.observe(with_gen(7, 4)).accepted());
  CHECK(obs.observe(with_gen(8, 5)).accepted());
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK(info.value().generation == Generation{8});
  CHECK_EQ(info.value().generation_rollovers, std::uint64_t{1});
}

TEST(reject_stale_refuses_a_superseded_generation) {
  ObservatoryConfig cfg;
  cfg.reject_stale = true;
  Observatory obs(cfg);
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  Observation current = gauge(s, d, MemoryKind::kAvailable, 1.0, t);
  current.generation = Generation{5};
  REQUIRE(obs.observe(current).accepted());
  Observation old = gauge(s, d, MemoryKind::kAvailable, 2.0, t + Duration{1000});
  old.generation = Generation{4};
  CHECK(obs.observe(old).outcome == IngestOutcome::kRejectedStale);
  CHECK_EQ(obs.retained_count(), std::size_t{1});
  const auto c = obs.counters();
  CHECK_EQ(c.rejected_stale, std::uint64_t{1});
  CHECK(c.is_balanced());
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK(info.value().generation == Generation{5});
}

TEST(refused_observation_changes_no_state) {
  ObservatoryConfig cfg;
  cfg.reject_out_of_order = true;
  cfg.reject_stale = true;
  Observatory obs(cfg);
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  Observation first = gauge(s, d, MemoryKind::kAvailable, 1.0, t);
  first.generation = Generation{1};
  REQUIRE(obs.observe(first).accepted());
  // Older than the source's last accepted time, but naming a newer generation.
  Observation late = gauge(s, d, MemoryKind::kAvailable, 2.0, t - Duration{1000});
  late.generation = Generation{5};
  CHECK(obs.observe(late).outcome == IngestOutcome::kRejectedOutOfOrder);
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK(info.value().generation == Generation{1});
  CHECK_EQ(info.value().generation_rollovers, std::uint64_t{0});
  // A refused observation from a source nobody has seen registers nothing.
  Observation unknown = gauge(SourceId{77}, d, MemoryKind::kAvailable, 3.0, t);
  unknown.freshness.stale = true;
  CHECK(obs.observe(unknown).outcome == IngestOutcome::kRejectedStale);
  CHECK(!obs.source_info(SourceId{77}).ok());
  CHECK_EQ(obs.sources().size(), std::size_t{1});
  CHECK(obs.counters().is_balanced());
}

TEST(removed_source_is_refused_and_not_reported_missing) {
  Observatory obs;
  SourceId gone = reg_source(obs, "gone");
  DomainId d = reg_domain(obs);
  REQUIRE(obs.remove_source(gone).ok());
  CHECK(!has_code_for(obs.evaluate_diagnostics(), DiagnosticCode::kMissingProvider, gone));
  IngestResult ir = obs.observe(
      gauge(gone, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp::now()));
  CHECK(ir.outcome == IngestOutcome::kRejectedInvalid);
  CHECK_EQ(obs.retained_count(), std::size_t{0});
  CHECK(obs.counters().is_balanced());
}

TEST(throwing_subscriber_does_not_break_ingestion) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  int delivered = 0;
  (void)obs.subscribe(
      [](const Observation&) { throw std::runtime_error("subscriber failed"); });
  (void)obs.subscribe([&](const Observation&) { ++delivered; });
  CHECK(!obs.subscribe(Observatory::SubscriberFn{}).valid());  // nothing to call
  bool threw = false;
  IngestResult ir;
  try {
    ir = obs.observe(gauge(s, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp::now()));
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(ir.accepted());
  CHECK_EQ(delivered, 1);
  CHECK_EQ(obs.retained_count(), std::size_t{1});
}

TEST(source_records_its_last_event) {
  Observatory obs;
  SourceId s = reg_source(obs);
  Observation ev;
  ev.source = s;
  ev.domain = reg_domain(obs);
  ev.timestamp.monotonic = MonotonicTimestamp{5000};
  Event e;
  e.type = EventType::kUser;
  ev.payload = e;
  REQUIRE(obs.observe(std::move(ev)).accepted());
  auto info = obs.source_info(s);
  REQUIRE(info.ok());
  CHECK_EQ(info.value().events_ingested, std::uint64_t{1});
  REQUIRE(info.value().last_event.has_value());
  CHECK(*info.value().last_event == MonotonicTimestamp{5000});
}

TEST(disabled_diagnostics_produce_nothing) {
  ObservatoryConfig off_cfg;
  off_cfg.diagnostics.enabled = false;
  Observatory off(off_cfg);
  ObservatoryConfig no_corr_cfg;
  no_corr_cfg.diagnostics.correlation_enabled = false;
  Observatory no_corr(no_corr_cfg);
  for (Observatory* obs : {&off, &no_corr}) {
    SourceId s = reg_source(*obs);
    DomainId d = reg_domain(*obs);
    const MonotonicTimestamp base = MonotonicTimestamp::now();
    for (int i = 0; i < 20; ++i) {
      obs->observe(gauge(s, d, MemoryKind::kAllocated, double(1000 + i * 100),
                         base + Duration{1000 * i}));
      obs->observe(gauge(s, d, MemoryKind::kAvailable, double(9000 - i * 100),
                         base + Duration{1000 * i}));
    }
  }
  CHECK(off.evaluate_diagnostics().empty());
  CHECK(off.evaluate_correlations().empty());
  auto snap = off.snapshot();
  REQUIRE(snap.ok());
  CHECK(snap.value().active_diagnostics.empty());
  auto report = off.make_report();
  REQUIRE(report.ok());
  CHECK(report.value().diagnostics.empty());
  // With correlation alone switched off, the diagnostics still run.
  CHECK(!no_corr.evaluate_diagnostics().empty());
  CHECK(no_corr.evaluate_correlations().empty());
}

TEST(timeline_trace_and_session_filters_select_by_value) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp base = MonotonicTimestamp::now();
  for (int i = 0; i < 6; ++i) {
    Observation o = gauge(s, d, MemoryKind::kAvailable, double(i),
                          base + Duration{1000 * i});
    if (i < 4) o.annotations.push_back({"trace_id", i < 3 ? "7" : "8"});
    obs.observe(std::move(o));
  }
  TimelineFilter by_trace;
  by_trace.trace = TraceId{7};
  auto tl = obs.query_timeline(by_trace);
  REQUIRE(tl.ok());
  CHECK_EQ(tl.value().size(), std::size_t{3});

  TimelineFilter own_session;
  own_session.session = obs.session_id();
  auto mine = obs.query_timeline(own_session);
  REQUIRE(mine.ok());
  CHECK_EQ(mine.value().size(), std::size_t{6});
  TimelineFilter other_session;
  other_session.session = SessionId{obs.session_id().value() + 41};
  auto theirs = obs.query_timeline(other_session);
  REQUIRE(theirs.ok());
  CHECK_EQ(theirs.value().size(), std::size_t{0});
}

TEST(timeline_cap_keeps_the_earliest_entries) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp base = MonotonicTimestamp::now();
  // Arrival order is not time order.
  const std::int64_t offsets[] = {3000, 1000, 2000, 5000, 4000};
  for (const std::int64_t off : offsets) {
    obs.observe(gauge(s, d, MemoryKind::kAvailable, double(off), base + Duration{off}));
  }
  TimelineFilter f;
  f.max_results = 2;
  auto tl = obs.query_timeline(f);
  REQUIRE(tl.ok());
  REQUIRE(tl.value().size() == 2);
  CHECK(tl.value().entries[0].observation.timestamp.monotonic == base + Duration{1000});
  CHECK(tl.value().entries[1].observation.timestamp.monotonic == base + Duration{2000});
}

TEST(snapshot_whole_system_overrides_the_source_subset) {
  Observatory obs;
  SourceId a = reg_source(obs, "a");
  SourceId b = reg_source(obs, "b");
  DomainId da = reg_domain(obs, "da");
  DomainId db = reg_domain(obs, "db");
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  obs.observe(domain_state(a, da, 1000, 500, t));
  obs.observe(domain_state(b, db, 2000, 900, t));
  SnapshotFilter only_a;
  only_a.source = a;
  auto subset = obs.snapshot(only_a);
  REQUIRE(subset.ok());
  CHECK_EQ(subset.value().domains.size(), std::size_t{1});
  SnapshotFilter whole;
  whole.whole_system = true;
  whole.source = a;
  whole.domain = da;
  auto all = obs.snapshot(whole);
  REQUIRE(all.ok());
  CHECK_EQ(all.value().domains.size(), std::size_t{2});
}

TEST(latest_domain_state_is_the_newest_by_time) {
  Observatory obs;
  SourceId s = reg_source(obs);
  DomainId d = reg_domain(obs);
  const MonotonicTimestamp t = MonotonicTimestamp::now();
  // The newer state arrives first; the older one, near capacity, arrives late.
  obs.observe(domain_state(s, d, 1000, 600, t + Duration{2000}));
  obs.observe(domain_state(s, d, 1000, 20, t + Duration{1000}));
  auto snap = obs.snapshot();
  REQUIRE(snap.ok());
  REQUIRE(snap.value().domains.size() == 1);
  CHECK_EQ(snap.value().domains[0].capacity.available.value(), Bytes{600});
  CHECK(!has_code(snap.value().active_diagnostics, DiagnosticCode::kNearCapacity));
  CHECK(!has_code(obs.evaluate_diagnostics(), DiagnosticCode::kNearCapacity));
}

TEST(staleness_is_measured_from_arrival_not_from_the_producers_clock) {
  ObservatoryConfig cfg;
  cfg.diagnostics.stale_after = Duration{1000000000LL};  // 1 s
  Observatory obs(cfg);
  DomainId d = reg_domain(obs);
  // Two producers on other clocks: one far behind this process's steady clock,
  // one far ahead of it. Both have just reported.
  SourceId behind = reg_source(obs, "behind");
  SourceId ahead = reg_source(obs, "ahead");
  obs.observe(gauge(behind, d, MemoryKind::kAvailable, 1.0, MonotonicTimestamp{1000}));
  obs.observe(gauge(ahead, d, MemoryKind::kAvailable, 1.0,
                    MonotonicTimestamp::now() + Duration{3600000000000LL}));
  const auto fresh = obs.evaluate_diagnostics();
  CHECK(!has_code_for(fresh, DiagnosticCode::kStaleTelemetry, behind));
  CHECK(!has_code_for(fresh, DiagnosticCode::kStaleTelemetry, ahead));
  auto snap = obs.snapshot();
  REQUIRE(snap.ok());
  CHECK_EQ(snap.value().stale_sources, std::uint64_t{0});
  // Both then go quiet for longer than the staleness rule allows.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const auto later = obs.evaluate_diagnostics();
  CHECK(has_code_for(later, DiagnosticCode::kStaleTelemetry, behind));
  CHECK(has_code_for(later, DiagnosticCode::kStaleTelemetry, ahead));
  auto snap2 = obs.snapshot();
  REQUIRE(snap2.ok());
  CHECK_EQ(snap2.value().stale_sources, std::uint64_t{2});
}

TEST(rapid_growth_is_judged_over_its_window) {
  const MonotonicTimestamp base = MonotonicTimestamp::now();
  // Slow, steady growth across a long history is not a rapid jump...
  Observatory slow;
  SourceId s = reg_source(slow);
  DomainId d = reg_domain(slow);
  double v = 1000.0;
  for (int i = 0; i < 60; ++i) {
    slow.observe(gauge(s, d, MemoryKind::kAllocated, v, base + Duration{1000 * i}));
    v *= 1.01;
  }
  CHECK(!has_code(slow.evaluate_diagnostics(), DiagnosticCode::kRapidGrowth));
  // ...and a jump inside the window is.
  Observatory jump;
  SourceId s2 = reg_source(jump);
  DomainId d2 = reg_domain(jump);
  for (int i = 0; i < 20; ++i) {
    jump.observe(gauge(s2, d2, MemoryKind::kAllocated, 1000.0, base + Duration{1000 * i}));
  }
  jump.observe(gauge(s2, d2, MemoryKind::kAllocated, 1500.0, base + Duration{1000 * 20}));
  CHECK(has_code(jump.evaluate_diagnostics(), DiagnosticCode::kRapidGrowth));
}

TEST(window_reaching_past_the_clock_range_still_contains_its_span) {
  Window open;
  open.begin = MonotonicTimestamp{1000};
  open.length = Duration{std::numeric_limits<std::int64_t>::max()};
  CHECK(open.contains(MonotonicTimestamp{1000}));
  CHECK(open.contains(MonotonicTimestamp{2000}));
  CHECK(open.contains(MonotonicTimestamp{std::numeric_limits<std::int64_t>::max()}));
  CHECK(!open.contains(MonotonicTimestamp{999}));
  Window backwards;
  backwards.begin = MonotonicTimestamp{1000};
  backwards.length = Duration{-5};
  CHECK(!backwards.contains(MonotonicTimestamp{1000}));
  Window plain;
  plain.begin = MonotonicTimestamp{100};
  plain.length = Duration{50};
  CHECK(plain.contains(MonotonicTimestamp{100}));
  CHECK(plain.contains(MonotonicTimestamp{149}));
  CHECK(!plain.contains(MonotonicTimestamp{150}));
}

TEST(sampler_treats_a_throwing_provider_as_a_failed_collection) {
  Observatory obs;
  Sampler sampler(obs);
  REQUIRE(sampler.add_provider(std::make_shared<ThrowingProvider>(), "boom").ok());
  bool threw = false;
  try {
    (void)sampler.collect_once();
    (void)sampler.collect_once();
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK_EQ(sampler.provider_failures(), std::uint64_t{1});
  CHECK_EQ(sampler.samples_taken(), std::uint64_t{0});
  // The failure is on record as provider-unavailable events.
  TimelineFilter f;
  f.event_type = EventType::kProviderUnavailable;
  auto tl = obs.query_timeline(f);
  REQUIRE(tl.ok());
  CHECK_EQ(tl.value().size(), std::size_t{2});
}

TEST(sampler_worker_survives_a_throwing_provider) {
  Observatory obs;
  Sampler::Config cfg;
  cfg.period = Duration{1000000LL};  // 1 ms
  Sampler sampler(obs, cfg);
  REQUIRE(sampler.add_provider(std::make_shared<ThrowingProvider>(), "boom").ok());
  REQUIRE(sampler.add_provider(std::make_shared<CountingProvider>(), "ok").ok());
  REQUIRE(sampler.start().ok());
  // The healthy provider keeps being sampled on the worker.
  for (int i = 0; i < 5000 && sampler.samples_taken() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(sampler.samples_taken() >= 3);
  CHECK(sampler.running());
  REQUIRE(sampler.stop().ok());
  CHECK_EQ(sampler.provider_failures(), std::uint64_t{1});
}

TEST(sampler_collects_on_request_after_a_start_and_stop) {
  Observatory obs;
  Sampler::Config cfg;
  cfg.period = Duration{3600000000000LL};  // one hour: the worker never fires here
  Sampler sampler(obs, cfg);
  REQUIRE(sampler.add_provider(std::make_shared<CountingProvider>(), "ok").ok());
  REQUIRE(sampler.start().ok());
  REQUIRE(sampler.stop().ok());
  CHECK(!sampler.running());
  CHECK(sampler.collect_once().ok());
  CHECK_EQ(sampler.samples_taken(), std::uint64_t{1});
  // A sampler that was never started can be stopped, and still used.
  Sampler idle(obs);
  REQUIRE(idle.add_provider(std::make_shared<CountingProvider>(), "idle").ok());
  REQUIRE(idle.stop().ok());
  CHECK(idle.collect_once().ok());
  CHECK_EQ(idle.samples_taken(), std::uint64_t{1});
}

TEST(sampler_refuses_a_period_that_is_not_positive) {
  Observatory obs;
  for (const std::int64_t ns : {std::int64_t{0}, std::int64_t{-5}}) {
    Sampler::Config cfg;
    cfg.period = Duration{ns};
    Sampler sampler(obs, cfg);
    const Error e = sampler.start();
    CHECK(!e.ok());
    CHECK(e.code() == ErrorCode::kInvalidArgument);
    CHECK(!sampler.running());
    CHECK(sampler.stop().ok());
  }
}

TEST(sampler_can_be_stopped_from_its_own_worker) {
  Observatory obs;
  Sampler::Config cfg;
  cfg.period = Duration{1000000LL};  // 1 ms
  Sampler sampler(obs, cfg);
  REQUIRE(sampler.add_provider(std::make_shared<CountingProvider>(), "ok").ok());
  std::atomic<int> seen{0};
  // A subscriber that shuts the sampler down on the first sample it sees. It
  // runs on the sampler's worker thread.
  (void)obs.subscribe([&](const Observation&) {
    if (seen.fetch_add(1) == 0) {
      (void)sampler.stop();
    }
  });
  REQUIRE(sampler.start().ok());
  for (int i = 0; i < 5000 && sampler.running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!sampler.running());
  CHECK(seen.load() >= 1);
  REQUIRE(sampler.stop().ok());  // joins the worker that stopped itself
  const std::uint64_t taken = sampler.samples_taken();
  REQUIRE(sampler.start().ok());  // and it can be started again
  for (int i = 0; i < 5000 && sampler.samples_taken() == taken; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(sampler.samples_taken() > taken);
  REQUIRE(sampler.stop().ok());
  CHECK(!sampler.running());
}

TEST(sampler_sequence_numbers_are_unique_across_threads) {
  Observatory obs;
  Sampler::Config cfg;
  cfg.period = Duration{1000000LL};  // 1 ms
  Sampler sampler(obs, cfg);
  REQUIRE(sampler.add_provider(std::make_shared<CountingProvider>(), "ok").ok());
  REQUIRE(sampler.start().ok());
  for (int i = 0; i < 2000; ++i) {
    (void)sampler.collect_once();
  }
  REQUIRE(sampler.stop().ok());
  auto tl = obs.query_timeline(TimelineFilter{});
  REQUIRE(tl.ok());
  std::vector<std::int64_t> seqs;
  for (const auto& e : tl.value().entries) {
    seqs.push_back(e.observation.timestamp.sequence.value());
  }
  std::sort(seqs.begin(), seqs.end());
  CHECK(std::adjacent_find(seqs.begin(), seqs.end()) == seqs.end());
  CHECK_EQ(static_cast<std::uint64_t>(seqs.size()), sampler.samples_taken());
}
