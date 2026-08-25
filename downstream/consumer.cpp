// Independent downstream consumer of the installed MemoryObservatory package.
// Builds with find_package(memory_observatory CONFIG REQUIRED) and links
// MemoryObservatory::memory_observatory. Ingests an observation and creates a
// snapshot, then exits successfully.
#include <cstdio>
#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/sampler.h"

using namespace memory_observatory;

int main() {
  Observatory observatory;
  Sampler sampler(observatory);
  auto synth = make_synthetic_provider("downstream", 1, DomainKind::kHostRam);
  if (synth) {
    (void)sampler.add_provider(std::move(synth), "host");
  }
  (void)sampler.collect_once();

  // Ingest a direct observation as well.
  SourceDescriptor sd; sd.name = "direct"; sd.kind = "synthetic";
  auto src = observatory.register_source(sd);
  DomainSpec ds; ds.name = "direct"; ds.kind = DomainKind::kUserDefined;
  auto dom = observatory.register_domain(ds);
  if (src.ok() && dom.ok()) {
    Observation o;
    o.source = src.value();
    o.domain = dom.value();
    o.timestamp.monotonic = MonotonicTimestamp::now();
    GaugeSample g; g.kind = MemoryKind::kAllocated; g.value = 1024.0;
    o.payload = g;
    auto ir = observatory.observe(std::move(o));
    if (!ir.accepted()) { std::printf("ingest rejected\n"); return 1; }
  }

  auto snap = observatory.snapshot();
  if (!snap.ok()) { std::printf("snapshot failed\n"); return 1; }
  std::printf("downstream ok: retained=%zu observations=%llu\n",
              observatory.retained_count(),
              (unsigned long long)snap.value().observation_count);
  return 0;
}
