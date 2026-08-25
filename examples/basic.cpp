// Example: basic host observation, snapshot, and report.
// Demonstrates the observable, non-mutating usage of Memory Observatory.
#include <cstdio>
#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/sampler.h"

using namespace memory_observatory;

int main() {
  Observatory observatory;
  Sampler sampler(observatory);

  auto host = make_host_memory_provider();
  if (host) {
    (void)sampler.add_provider(std::move(host), "host");
    (void)sampler.collect_once();
    (void)sampler.collect_once();
  } else {
    auto synth = make_synthetic_provider("synthetic", 1, DomainKind::kHostRam);
    (void)sampler.add_provider(std::move(synth), "host");
    (void)sampler.collect_once();
  }

  std::printf("retained=%zu\n", observatory.retained_count());
  auto snap = observatory.snapshot();
  if (snap.ok()) {
    std::printf("snapshot %llu sources=%zu domains=%zu observations=%llu\n",
                (unsigned long long)snap.value().id.value(),
                snap.value().sources.size(), snap.value().domains.size(),
                (unsigned long long)snap.value().observation_count);
  }
  auto rep = observatory.make_report();
  if (rep.ok()) {
    std::printf("report diagnostics=%zu correlations=%zu\n",
                rep.value().diagnostics.size(), rep.value().correlations.size());
  }
  return 0;
}
