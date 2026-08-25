// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/sampler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace memory_observatory {

namespace {

// Build an Observation from a provider Sample, attaching identity + provenance.
Observation sample_to_observation(const Provider::Sample& sample, SourceId src,
                                  DomainId dom, DomainKind dkink,
                                  const Provider& provider, SequenceNo seq,
                                  MonotonicTimestamp now) {
  Observation o;
  o.source = src;
  o.domain = dom;
  o.domain_kind = dkink;
  o.timestamp.monotonic = now;
  o.timestamp.sequence = seq;
  o.provenance.source = src;
  o.provenance.source_name = provider.name();
  o.provenance.provider_version = provider.provider_version();
  o.provenance.mechanism = (provider.kind() == "synthetic")
                               ? ObservationMechanism::kSynthetic
                               : ObservationMechanism::kLive;
  o.provenance.confidence = Confidence{0.9};
  if (sample.gauge_value) {
    GaugeSample g;
    g.kind = sample.kind;
    g.value = sample.gauge_value;
    o.payload = g;
  } else if (sample.counter_value) {
    CounterSample c;
    c.kind = sample.kind;
    c.value = sample.counter_value;
    c.reset = sample.counter_reset;
    o.payload = c;
  } else if (!sample.distribution.empty()) {
    DistributionSample d;
    d.kind = sample.kind;
    d.summary = sample.distribution;
    o.payload = d;
  } else {
    o.payload = std::monostate{};
  }
  return o;
}

Observation event_observation(SourceId src, DomainId dom, EventType type,
                              EventSeverity sev, std::string message,
                              SequenceNo seq, MonotonicTimestamp now) {
  Observation o;
  o.source = src;
  o.domain = dom;
  o.timestamp.monotonic = now;
  o.timestamp.sequence = seq;
  Event e;
  e.type = type;
  e.severity = sev;
  e.source = EventSource::kProvider;
  e.message = std::move(message);
  o.payload = e;
  return o;
}

}  // namespace

struct Sampler::Impl {
  struct Entry {
    std::shared_ptr<Provider> provider;
    SourceId source;
    DomainId domain;
  };

  explicit Impl(Observatory& o, const Config& c) : observatory(o), config(c) {}

  Sampler* owner = nullptr;
  Observatory& observatory;
  Config config;

  mutable std::mutex mutex_;
  std::vector<Entry> providers_;
  std::unordered_set<SourceId, IdHash<SourceTag>> failed_sources_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::mutex cv_m_;
  std::condition_variable cv_;

  SequenceNo next_seq_;
  std::uint64_t samples_taken_ = 0;
  std::uint64_t failures_ = 0;
  std::uint64_t recoveries_ = 0;

  void thread_loop() {
    while (true) {
      bool stop = false;
      {
        std::unique_lock<std::mutex> lk(cv_m_);
        stop = cv_.wait_for(lk, std::chrono::nanoseconds(config.period.nanoseconds()),
                            [this] { return stopping_.load(); });
      }
      if (stop) {
        break;
      }
      if (owner) {
        (void)owner->collect_once();
      }
    }
    running_.store(false);
  }
};

Sampler::Sampler(Observatory& observatory, const Config& config)
    : impl_(std::make_unique<Impl>(observatory, config)) {
  impl_->owner = this;
  if (config.auto_start) {
    (void)start();
  }
}

Sampler::~Sampler() { (void)stop(); }

Result<std::pair<SourceId, DomainId>> Sampler::add_provider(
    std::shared_ptr<Provider> provider, const std::string& domain_name) {
  if (!provider) {
    return fail<std::pair<SourceId, DomainId>>(ErrorCode::kInvalidArgument,
                                               "provider is null");
  }
  SourceDescriptor sd;
  sd.name = provider->name();
  sd.kind = provider->kind();
  sd.provider_version = provider->provider_version();
  auto src = impl_->observatory.register_source(sd);
  if (!src.ok()) {
    return Result<std::pair<SourceId, DomainId>>{src.error()};
  }
  DomainSpec ds;
  ds.name = domain_name.empty() ? provider->kind() : domain_name;
  ds.kind = provider->primary_domain();
  auto dom = impl_->observatory.register_domain(ds);
  if (!dom.ok()) {
    return Result<std::pair<SourceId, DomainId>>{dom.error()};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  Impl::Entry e;
  e.provider = std::move(provider);
  e.source = src.value();
  e.domain = dom.value();
  impl_->providers_.push_back(std::move(e));
  return ok(std::pair<SourceId, DomainId>{src.value(), dom.value()});
}

Error Sampler::collect_once() {
  std::vector<Impl::Entry> snapshot;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    snapshot = impl_->providers_;
  }
  const MonotonicTimestamp now = MonotonicTimestamp::now();
  for (const auto& entry : snapshot) {
    if (impl_->stopping_.load()) {
      break;
    }
    bool ok = entry.provider->available();
    Provider::CollectResult cr;
    if (ok) {
      cr = entry.provider->collect();
    }
    bool was_failed = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex_);
      was_failed = impl_->failed_sources_.count(entry.source) != 0;
    }
    if (!cr.ok || !cr.error.ok()) {
      if (!was_failed) {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->failed_sources_.insert(entry.source);
        ++impl_->failures_;
      }
      Observation ev = event_observation(
          entry.source, entry.domain, EventType::kProviderUnavailable,
          EventSeverity::kWarning,
          "provider '" + entry.provider->name() + "' unavailable or failed",
          impl_->next_seq_++, now);
      (void)impl_->observatory.observe(std::move(ev));
      continue;
    }
    // Recovery if previously failed.
    if (was_failed) {
      {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->failed_sources_.erase(entry.source);
        ++impl_->recoveries_;
      }
      Observation ev = event_observation(
          entry.source, entry.domain, EventType::kProviderRecovered,
          EventSeverity::kInfo,
          "provider '" + entry.provider->name() + "' recovered",
          impl_->next_seq_++, now);
      (void)impl_->observatory.observe(std::move(ev));
    }
    for (const auto& sample : cr.samples) {
      Observation o = sample_to_observation(sample, entry.source, entry.domain,
                                            entry.provider->primary_domain(),
                                            *entry.provider, impl_->next_seq_++,
                                            now);
      (void)impl_->observatory.observe(std::move(o));
      std::lock_guard<std::mutex> lock(impl_->mutex_);
      ++impl_->samples_taken_;
    }
  }
  return Error{};
}

Error Sampler::start() {
  if (impl_->running_.load()) {
    return make_error(ErrorCode::kInvalidState, "sampler already running");
  }
  impl_->stopping_.store(false);
  impl_->worker_ = std::thread([this] { impl_->thread_loop(); });
  impl_->running_.store(true);
  return Error{};
}

Error Sampler::stop() {
  if (!impl_->running_.load()) {
    return Error{};
  }
  impl_->stopping_.store(true);
  impl_->cv_.notify_all();
  if (impl_->worker_.joinable()) {
    impl_->worker_.join();
  }
  impl_->running_.store(false);
  return Error{};
}

bool Sampler::running() const noexcept { return impl_->running_.load(); }

std::uint64_t Sampler::samples_taken() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->samples_taken_;
}

std::uint64_t Sampler::provider_failures() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->failures_;
}

std::uint64_t Sampler::provider_recoveries() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->recoveries_;
}

std::size_t Sampler::provider_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->providers_.size();
}

}  // namespace memory_observatory
