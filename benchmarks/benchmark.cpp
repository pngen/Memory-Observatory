// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Self-benchmark of Memory Observatory. Uses a deterministic seeded workload.
// Asynchronous work is synchronized before timing; results are single-machine
// measurements, not claims of universal scalability.

#include <cstdio>
#include <vector>
#include "memory_observatory/observatory.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

int main() {
  const int kN = 200000;
  const int kDomain = 4;
  ObservatoryConfig cfg;
  cfg.history_capacity = 1000000;
  Observatory obs(cfg);
  SourceDescriptor sd; sd.name = "bench"; sd.kind = "synthetic";
  auto src = obs.register_source(sd);
  DomainSpec ds; ds.name = "host"; ds.kind = DomainKind::kHostRam;
  auto dom = obs.register_domain(ds);
  std::vector<DomainId> doms;
  for (int d = 0; d < kDomain; ++d) {
    DomainSpec dsp; dsp.name = "dom" + std::to_string(d); dsp.kind = DomainKind::kHostRam;
    auto r = obs.register_domain(dsp);
    if (r.ok()) doms.push_back(r.value());
  }
  if (!src.ok() || doms.empty()) { std::printf("setup failed\n"); return 1; }

  // Seed deterministic data into a vector.
  std::vector<Observation> data;
  data.reserve(static_cast<std::size_t>(kN));
  const MonotonicTimestamp base = MonotonicTimestamp::now();
  for (int i = 0; i < kN; ++i) {
    Observation o;
    o.source = src.value();
    o.domain = doms[static_cast<std::size_t>(i % kDomain)];
    o.timestamp.monotonic = base + Duration{1000LL * i};
    GaugeSample g;
    g.kind = MemoryKind::kAvailable;
    g.value = 1000000.0 - static_cast<double>(i);
    o.payload = g;
    data.push_back(std::move(o));
  }

  // Ingestion throughput.
  auto ingest_start = MonotonicTimestamp::now();
  std::size_t accepted = 0;
  for (auto& o : data) {
    auto r = obs.observe(std::move(o));
    if (r.accepted()) ++accepted;
  }
  auto ingest_end = MonotonicTimestamp::now();
  const double ingest_secs = (ingest_end - ingest_start).seconds();
  std::printf("ingestion: %zu accepted in %.3f s -> %.0f obs/s\n",
              accepted, ingest_secs, ingest_secs > 0 ? accepted / ingest_secs : 0.0);

  // Snapshot latency.
  auto ss = MonotonicTimestamp::now();
  auto snap = obs.snapshot();
  auto se = MonotonicTimestamp::now();
  std::printf("snapshot: ok=%d in %.3f ms\n", snap.ok() ? 1 : 0, (se - ss).seconds() * 1000.0);

  // Timeline query latency.
  TimelineFilter tf;
  auto qs = MonotonicTimestamp::now();
  auto tl = obs.query_timeline(tf);
  auto qe = MonotonicTimestamp::now();
  std::printf("timeline query: %zu entries in %.3f ms\n",
              tl.ok() ? tl.value().size() : 0, (qe - qs).seconds() * 1000.0);

  // Diagnostics.
  auto diags = obs.evaluate_diagnostics();
  std::printf("diagnostics: %zu active\n", diags.size());

  // Trace serialization / deserialization.
  auto trace = obs.capture_trace();
  if (trace.ok()) {
    auto ss1 = MonotonicTimestamp::now();
    auto ser = serialize_trace_binary(trace.value());
    auto se1 = MonotonicTimestamp::now();
    std::printf("trace serialize: %zu bytes in %.3f ms\n",
                ser.ok() ? ser.value().size() : 0, (se1 - ss1).seconds() * 1000.0);
    if (ser.ok()) {
      auto sd_ = MonotonicTimestamp::now();
      auto de = deserialize_trace_binary(ser.value().data(), ser.value().size());
      auto ed = MonotonicTimestamp::now();
      std::printf("trace deserialize: ok=%d in %.3f ms\n", de.ok() ? 1 : 0,
                  (ed - sd_).seconds() * 1000.0);
    }
  }
  return 0;
}
