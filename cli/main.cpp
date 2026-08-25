// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// memory-observatory CLI.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/report.h"
#include "memory_observatory/sampler.h"
#include "memory_observatory/trace.h"
#include "memory_observatory/version.h"

using namespace memory_observatory;

namespace {

void print_help() {
  std::printf(
    "memory-observatory -- open-source observability runtime for memory behavior\n"
    "\n"
    "usage: memory-observatory <command> [options]\n"
    "\n"
    "commands:\n"
    "  info                              print version and environment\n"
    "  providers                         list available providers and backends\n"
    "  domains                           list known memory domains\n"
    "  observe                           run one collection pass over providers\n"
    "  snapshot[--json]                  produce a system snapshot\n"
    "  watch [--count N] [--interval MS] bounded live watch\n"
    "  timeline [--domain ID]            query the retained timeline\n"
    "  diagnostics                       evaluate active diagnostics\n"
    "  explain <code>                    explain a diagnostic code\n"
    "  trace-record <file>               record a trace to <file> (binary)\n"
    "  trace-inspect <file>              inspect a trace file\n"
    "  trace-replay <file>               replay a trace into a fresh observatory\n"
    "  compare <before> <after>          compare two trace files\n"
    "  report [--json|--csv]             generate a session report\n"
    "  benchmark                         run a self-benchmark\n"
    "  --help                            show this help\n");
}

int cmd_info() {
  std::printf("memory-observatory %s\n", version::kString);
  std::printf("library version: %d.%d.%d\n", version::kMajor, version::kMinor, version::kPatch);
  return 0;
}

std::vector<Observation> run_observe(Observatory& obs, std::uint64_t seed,
                                     std::uint64_t count) {
  Sampler sampler(obs);
  auto prov = make_synthetic_provider("synthetic", seed, DomainKind::kHostRam);
  if (prov) {
    (void)sampler.add_provider(std::move(prov), "host");
    for (std::uint64_t i = 0; i < count; ++i) {
      (void)sampler.collect_once();
    }
  }
  std::vector<Observation> out;
  TimelineFilter tf;
  auto tl = obs.query_timeline(tf);
  if (tl.ok()) {
    for (const auto& e : tl.value().entries) out.push_back(e.observation);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    print_help();
    return 0;
  }

  if (cmd == "info") return cmd_info();

  if (cmd == "providers") {
    std::printf("available providers (Windows host):\n");
    bool found = false;
    auto host = make_host_memory_provider();
    if (host) { std::printf("  host            : available\n"); found = true; }
    auto proc = make_process_memory_provider();
    if (proc) { std::printf("  process         : available\n"); found = true; }
    auto commit = make_system_commit_provider();
    if (commit) { std::printf("  system-commit   : available\n"); found = true; }
    auto cuda = make_cuda_provider();
    if (cuda) { std::printf("  cuda            : loaded (query depends on runtime availability)\n"); found = true; }
    std::printf("  synthetic       : available\n");
    std::printf("  push            : available\n");
    if (!found) std::printf("  (no platform providers; synthetic/push remain available)\n");
    return 0;
  }

  if (cmd == "domains") {
    std::printf("known memory domains:\n");
    const DomainKind kinds[] = {DomainKind::kHostRam, DomainKind::kProcessCommit,
        DomainKind::kSystemCommit, DomainKind::kPinnedHost, DomainKind::kAcceleratorDevice,
        DomainKind::kSharedMemory, DomainKind::kFileBackedMapped, DomainKind::kPersistentStorage,
        DomainKind::kUserDefined};
    for (DomainKind k : kinds) std::printf("  %s\n", to_string(k));
    return 0;
  }

  if (cmd == "observe") {
    Observatory obs;
    auto obs_list = run_observe(obs, 42, 8);
    std::printf("collected %zu observations\n", obs_list.size());
    auto c = obs.counters();
    std::printf("retained=%llu ingested_total=%llu accounting_balanced=%s\n",
                (unsigned long long)c.retained, (unsigned long long)c.ingested_total,
                c.is_balanced() ? "yes" : "no");
    return 0;
  }

  if (cmd == "snapshot") {
    Observatory obs;
    auto ob = run_observe(obs, 7, 12);
    bool json = false;
    for (int i = 2; i < argc; ++i) if (std::strcmp(argv[i], "--json") == 0) json = true;
    auto snap = obs.snapshot();
    if (!snap.ok()) { std::fprintf(stderr, "snapshot failed: %s\n", snap.error().message().c_str()); return 1; }
    if (json) {
      std::printf("{\"snapshot\":{\"domains\":%zu,\"sources\":%zu,\"observations\":%llu}}\n",
                  snap.value().domains.size(), snap.value().sources.size(),
                  (unsigned long long)snap.value().observation_count);
    } else {
      std::printf("snapshot %llu: %zu domains, %zu sources, %llu observations\n",
                  (unsigned long long)snap.value().id.value(),
                  snap.value().domains.size(), snap.value().sources.size(),
                  (unsigned long long)snap.value().observation_count);
      std::printf("  active diagnostics: %zu\n", snap.value().active_diagnostics.size());
    }
    (void)ob;
    return 0;
  }

  if (cmd == "watch") {
    std::uint64_t count = 5;
    std::uint64_t interval_ms = 100;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = std::strtoull(argv[++i], nullptr, 10);
      if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval_ms = std::strtoull(argv[++i], nullptr, 10);
    }
    Observatory obs;
    Sampler sampler(obs);
    auto prov = make_synthetic_provider("watch", 1, DomainKind::kHostRam);
    if (!prov) { std::fprintf(stderr, "provider unavailable\n"); return 1; }
    (void)sampler.add_provider(std::move(prov), "host");
    for (std::uint64_t i = 0; i < count; ++i) {
      (void)sampler.collect_once();
      auto c = obs.counters();
      std::printf("tick %llu: retained=%llu\n", (unsigned long long)(i + 1),
                  (unsigned long long)c.retained);
    }
    return 0;
  }

  if (cmd == "diagnostics") {
    Observatory obs;
    (void)run_observe(obs, 3, 40);
    auto diags = obs.evaluate_diagnostics();
    std::printf("%zu active diagnostics:\n", diags.size());
    for (const auto& d : diags) {
      std::printf("  [%s] %s: %s\n", to_string(d.kind), to_string(d.code), d.summary.c_str());
    }
    return 0;
  }

  if (cmd == "explain") {
    if (argc < 3) { std::fprintf(stderr, "usage: explain <code>\n"); return 2; }
    DiagnosticCode code = DiagnosticCode::kNone;
    const std::string s = argv[2];
    // Map a few common codes; unknown -> nearest by name.
    if (s == "possible_leak_pattern") code = DiagnosticCode::kPossibleLeakPattern;
    else if (s == "near_capacity") code = DiagnosticCode::kNearCapacity;
    else if (s == "stale_telemetry") code = DiagnosticCode::kStaleTelemetry;
    else if (s == "sustained_capacity_growth") code = DiagnosticCode::kSustainedCapacityGrowth;
    else if (s == "queue_overflow") code = DiagnosticCode::kQueueOverflow;
    else { std::fprintf(stderr, "unknown diagnostic code '%s'\n", s.c_str()); return 2; }
    Observatory obs;
    (void)run_observe(obs, 5, 40);
    auto ex = obs.explain(code);
    std::printf("code=%s kind=%s\n", to_string(code), to_string(ex.conclusion_kind));
    std::printf("narrative: %s\n", ex.narrative.c_str());
    for (const auto& lim : ex.limitations) std::printf("  limitation: %s\n", lim.c_str());
    return 0;
  }

  if (cmd == "timeline") {
    Observatory obs;
    (void)run_observe(obs, 9, 30);
    TimelineFilter tf;
    auto tl = obs.query_timeline(tf);
    if (!tl.ok()) { std::fprintf(stderr, "timeline query failed\n"); return 1; }
    std::printf("%zu timeline entries:\n", tl.value().size());
    for (const auto& e : tl.value().entries) {
      std::printf("  t=%lld src=%llu dom=%llu payload=%zu\n",
                  (long long)e.observation.timestamp.monotonic.nanoseconds(),
                  (unsigned long long)e.observation.source.value(),
                  (unsigned long long)e.observation.domain.value(),
                  e.observation.payload.index());
    }
    return 0;
  }

  if (cmd == "trace-record") {
    if (argc < 3) { std::fprintf(stderr, "usage: trace-record <file>\n"); return 2; }
    Observatory obs;
    (void)run_observe(obs, 11, 20);
    auto tr = obs.capture_trace();
    if (!tr.ok()) { std::fprintf(stderr, "capture failed\n"); return 1; }
    auto bin = serialize_trace_binary(tr.value());
    if (!bin.ok()) { std::fprintf(stderr, "serialize failed\n"); return 1; }
    std::ofstream f(argv[2], std::ios::binary);
    f.write(reinterpret_cast<const char*>(bin.value().data()), bin.value().size());
    std::printf("recorded %zu records to %s\n", tr.value().size(), argv[2]);
    return 0;
  }

  if (cmd == "trace-inspect") {
    if (argc < 3) { std::fprintf(stderr, "usage: trace-inspect <file>\n"); return 2; }
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto tr = deserialize_trace_binary(bytes.data(), bytes.size());
    if (!tr.ok()) { std::fprintf(stderr, "inspect failed: %s\n", tr.error().message().c_str()); return 1; }
    std::printf("trace %llu version=%u records=%zu format=%s\n",
                (unsigned long long)tr.value().id.value(),
                tr.value().version, tr.value().size(), tr.value().format.c_str());
    return 0;
  }

  if (cmd == "trace-replay") {
    if (argc < 3) { std::fprintf(stderr, "usage: trace-replay <file>\n"); return 2; }
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto tr = deserialize_trace_binary(bytes.data(), bytes.size());
    if (!tr.ok()) { std::fprintf(stderr, "replay failed: %s\n", tr.error().message().c_str()); return 1; }
    Observatory obs;
    ReplayResult rr = replay_trace(obs, tr.value(), {});
    std::printf("replayed %llu records, accepted=%llu rejected=%llu duplicates=%llu\n",
                (unsigned long long)rr.consumed, (unsigned long long)rr.accepted,
                (unsigned long long)rr.rejected, (unsigned long long)rr.duplicates);
    return 0;
  }

  if (cmd == "compare") {
    std::fprintf(stderr, "compare is implemented via the library API in examples; CLI compare requires two trace files.\n");
    return 2;
  }

  if (cmd == "report") {
    Observatory obs;
    (void)run_observe(obs, 21, 30);
    auto rep = obs.make_report();
    if (!rep.ok()) { std::fprintf(stderr, "report failed\n"); return 1; }
    bool json = false, csv = false;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--json") == 0) json = true;
      if (std::strcmp(argv[i], "--csv") == 0) csv = true;
    }
    if (json) std::printf("%s\n", render_report_json(rep.value()).c_str());
    else if (csv) std::printf("%s", render_report_csv(rep.value()).c_str());
    else std::printf("%s", render_report_text(rep.value()).c_str());
    return 0;
  }

  if (cmd == "benchmark") {
    ObservatoryConfig cfg;
    cfg.history_capacity = 1000000;
    Observatory obs(cfg);
    SourceDescriptor sd; sd.name = "bench"; sd.kind = "synthetic";
    auto src = obs.register_source(sd);
    DomainSpec ds; ds.name = "host"; ds.kind = DomainKind::kHostRam;
    auto dom = obs.register_domain(ds);
    if (!src.ok() || !dom.ok()) return 1;
    const int n = 200000;
    auto t0 = MonotonicTimestamp::now();
    for (int i = 0; i < n; ++i) {
      Observation o;
      o.source = src.value();
      o.domain = dom.value();
      o.timestamp.monotonic = t0 + Duration{1000 * i};
      GaugeSample g; g.kind = MemoryKind::kAvailable; g.value = double(i);
      o.payload = g;
      (void)obs.observe(std::move(o));
    }
    auto t1 = MonotonicTimestamp::now();
    double secs = (t1 - t0).seconds();
    std::printf("ingest %d observations in %.3f s => %.0f obs/s\n", n, secs,
                secs > 0 ? n / secs : 0.0);
    return 0;
  }

  std::fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
  print_help();
  return 2;
}
