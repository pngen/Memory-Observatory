// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Provider interface. A Provider collects memory-domain observations without
// mutating the system. A provider returns raw Sample records; the Sampler turns
// them into Observations, attaching source identity, domain mapping, provenance,
// and a monotonic timestamp, then feeds them to the Observatory.
//
// Providers never mutate external runtime state. If a backend is unavailable they
// return a structured unavailable/failed result (never a fabricated value).

#ifndef MEMORY_OBSERVATORY_PROVIDER_HPP
#define MEMORY_OBSERVATORY_PROVIDER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "memory_observatory/domain.h"
#include "memory_observatory/error.h"
#include "memory_observatory/observation.h"
#include "memory_observatory/trace.h"

namespace memory_observatory {

class Provider {
 public:
  struct Sample {
    DomainKind domain_kind = DomainKind::kUnknown;
    MemoryKind kind = MemoryKind::kUnknown;
    Scale scale = Scale::kBytes;
    std::optional<double> gauge_value;
    std::optional<std::uint64_t> counter_value;
    bool counter_reset = false;
    DistributionSummary distribution;
    std::vector<Annotation> annotations;
  };

  struct CollectResult {
    bool ok = false;
    Error error;
    std::vector<Sample> samples;
  };

  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  virtual std::string kind() const = 0;
  virtual std::string provider_version() const = 0;
  virtual DomainKind primary_domain() const = 0;
  virtual bool available() const = 0;
  virtual CollectResult collect() = 0;
};

// Convenience factory functions. Each returns a provider, or nullptr if the
// backend is unavailable (callers must handle unavailable and not fabricate).
std::unique_ptr<Provider> make_synthetic_provider(const std::string& name,
                                                  std::uint64_t seed,
                                                  DomainKind domain_kind);
std::unique_ptr<Provider> make_host_memory_provider();
std::unique_ptr<Provider> make_process_memory_provider();
std::unique_ptr<Provider> make_system_commit_provider();
std::unique_ptr<Provider> make_cuda_provider();
std::unique_ptr<Provider> make_push_provider(const std::string& name);

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_PROVIDER_HPP
