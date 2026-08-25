// Example: a custom external push provider.
// A producer subclasses Provider and forwards observations into the Observatory
// through the Sampler, with provenance and a generation.
#include <cstdio>
#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/sampler.h"

using namespace memory_observatory;

class MyProvider final : public Provider {
 public:
  std::string name() const override { return "custom"; }
  std::string kind() const override { return "custom"; }
  std::string provider_version() const override { return "0.1"; }
  DomainKind primary_domain() const override { return DomainKind::kUserDefined; }
  bool available() const override { return true; }
  CollectResult collect() override {
    CollectResult r;
    r.ok = true;
    ++tick_;
    Sample s;
    s.domain_kind = DomainKind::kUserDefined;
    s.kind = MemoryKind::kAllocated;
    s.scale = Scale::kBytes;
    s.gauge_value = 1024.0 * static_cast<double>(tick_);
    r.samples.push_back(std::move(s));
    return r;
  }
 private:
  std::uint64_t tick_ = 0;
};

int main() {
  Observatory observatory;
  Sampler sampler(observatory);
  auto ids = sampler.add_provider(std::make_shared<MyProvider>(), "custom");
  if (ids.ok()) {
    (void)sampler.collect_once();
    std::printf("custom provider registered source=%llu domain=%llu\n",
                (unsigned long long)ids.value().first.value(),
                (unsigned long long)ids.value().second.value());
    std::printf("retained=%zu\n", observatory.retained_count());
  }
  return 0;
}
