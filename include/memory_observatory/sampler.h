// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Sampling engine. A Sampler drives registered Providers on a configurable
// cadence, converts their raw Samples into Observations (attaching source and
// domain identity, provenance, and a monotonic timestamp), and ingests them
// into an Observatory. It is bounded, has clean startup/shutdown, and never
// busy-waits.

#ifndef MEMORY_OBSERVATORY_SAMPLER_HPP
#define MEMORY_OBSERVATORY_SAMPLER_HPP

#include <memory>
#include <vector>

#include "memory_observatory/error.h"
#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/time.h"

namespace memory_observatory {

class Sampler {
 public:
  struct Config {
    Duration period = Duration{1000000000LL};  // 1s
    bool auto_start = false;
  };

  explicit Sampler(Observatory& observatory, const Config& config = {});
  ~Sampler();
  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  // Registers a provider: creates a source + domain and returns their ids.
  Result<std::pair<SourceId, DomainId>> add_provider(
      std::shared_ptr<Provider> provider, const std::string& domain_name = "");

  // Performs a single collection pass synchronously.
  Error collect_once();
  Error start();
  Error stop();
  [[nodiscard]] bool running() const noexcept;

  [[nodiscard]] std::uint64_t samples_taken() const noexcept;
  [[nodiscard]] std::uint64_t provider_failures() const noexcept;
  [[nodiscard]] std::uint64_t provider_recoveries() const noexcept;
  [[nodiscard]] std::size_t provider_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_SAMPLER_HPP
