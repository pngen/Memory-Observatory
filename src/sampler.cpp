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
  // Carry provider provenance (e.g. CUDA device identity) onto the observation.
  o.annotations = sample.annotations;
  for (const auto& a : sample.annotations) {
    if (a.key == "device_name") o.provenance.accelerator = a.value;
    else if (a.key == "driver_version") o.provenance.driver_version = a.value;
  }
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
  // The worker's own id while its loop runs, so a call made from inside it (a
  // subscriber or a provider) can be told apart from a call made by its owner.
  std::atomic<std::thread::id> worker_id_{};
  std::mutex cv_m_;
  std::condition_variable cv_;

  // Collection passes may run on the worker and on callers' threads at once.
  std::atomic<std::int64_t> next_seq_{0};
  std::uint64_t samples_taken_ = 0;
  std::uint64_t failures_ = 0;
  std::uint64_t recoveries_ = 0;

  SequenceNo take_sequence() noexcept { return SequenceNo{next_seq_.fetch_add(1)}; }

  void thread_loop() {
    worker_id_.store(std::this_thread::get_id());
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
    worker_id_.store(std::thread::id{});
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
  // Only the worker's own pass is cut short by a stop. A pass a caller asked
  // for runs to the end, whether or not the sampler was started and stopped.
  const bool worker_pass = impl_->worker_id_.load() == std::this_thread::get_id();
  for (const auto& entry : snapshot) {
    if (worker_pass && impl_->stopping_.load()) {
      break;
    }
    // Provider code is foreign. A throw is a failed collection: it must not
    // unwind through the sampler, where on the worker it would end the process.
    Provider::CollectResult cr;
    try {
      if (entry.provider->available()) {
        cr = entry.provider->collect();
      }
    } catch (...) {
      cr = Provider::CollectResult{};
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
          impl_->take_sequence(), now);
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
          impl_->take_sequence(), now);
      (void)impl_->observatory.observe(std::move(ev));
    }
    for (const auto& sample : cr.samples) {
      Observation o = sample_to_observation(sample, entry.source, entry.domain,
                                            entry.provider->primary_domain(),
                                            *entry.provider, impl_->take_sequence(),
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
  // The worker waits one period between passes; without a positive period it
  // would not wait at all.
  if (impl_->config.period.nanoseconds() <= 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "sampling period must be positive");
  }
  if (impl_->worker_.joinable()) {
    impl_->worker_.join();  // a worker that stopped itself has left its loop
  }
  impl_->stopping_.store(false);
  impl_->running_.store(true);
  try {
    impl_->worker_ = std::thread([this] { impl_->thread_loop(); });
  } catch (...) {
    impl_->running_.store(false);
    return make_error(ErrorCode::kInternal, "could not start the sampler thread");
  }
  return Error{};
}

Error Sampler::stop() {
  {
    // Set under the lock the worker waits on, so the worker cannot test the
    // flag, miss this notification and then sleep out a whole period.
    std::lock_guard<std::mutex> lk(impl_->cv_m_);
    impl_->stopping_.store(true);
  }
  impl_->cv_.notify_all();
  if (impl_->worker_id_.load() == std::this_thread::get_id()) {
    // Called from inside the worker, which cannot join itself. It leaves its
    // loop after the current pass and is joined by the next stop(), by start()
    // or by the destructor.
    return Error{};
  }
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
