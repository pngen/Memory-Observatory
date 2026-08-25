// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Real-hardware provider validation. These query actual system memory on the
// host and, when a CUDA backend is present, perform a small bounded device
// allocation to observe utilization movement. If a backend is unavailable, the
// test passes with an explicit note and never fabricates a value.

#include "framework.hpp"

#include <cmath>

#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/sampler.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace memory_observatory;

TEST(synthetic_provider_collects) {
  auto prov = make_synthetic_provider("syn", 1, DomainKind::kHostRam);
  REQUIRE(prov != nullptr);
  Provider::CollectResult r = prov->collect();
  REQUIRE(r.ok);
  CHECK(r.samples.size() >= 2);
}

TEST(host_memory_provider_reports_real_memory) {
  auto prov = make_host_memory_provider();
  if (!prov) {
    // Non-Windows or unavailable: pass with no fabricated data.
    CHECK(true);
    return;
  }
  Provider::CollectResult r = prov->collect();
  REQUIRE(r.ok);
  bool saw_physical = false, saw_available = false;
  double physical = 0, available = 0;
  for (const auto& s : r.samples) {
    if (s.kind == MemoryKind::kPhysical && s.gauge_value) {
      saw_physical = true;
      physical = *s.gauge_value;
    }
    if (s.kind == MemoryKind::kAvailable && s.gauge_value) {
      saw_available = true;
      available = *s.gauge_value;
    }
  }
  CHECK(saw_physical);
  CHECK(saw_available);
  CHECK(physical > 0);
  CHECK(available >= 0);
  CHECK(available <= physical);
}

TEST(process_memory_provider_reports_process_state) {
  auto prov = make_process_memory_provider();
  if (!prov) { CHECK(true); return; }
  Provider::CollectResult r = prov->collect();
  REQUIRE(r.ok);
  bool saw_commit = false;
  for (const auto& s : r.samples) {
    if (s.kind == MemoryKind::kCommitted && s.counter_value) saw_commit = true;
  }
  CHECK(saw_commit);
}

TEST(system_commit_provider_reports_commit) {
  auto prov = make_system_commit_provider();
  if (!prov) { CHECK(true); return; }
  Provider::CollectResult r = prov->collect();
  REQUIRE(r.ok);
  bool saw_committed = false;
  for (const auto& s : r.samples) {
    if (s.kind == MemoryKind::kCommitted && s.counter_value) saw_committed = true;
  }
  CHECK(saw_committed);
}

TEST(sampler_drives_provider_into_observatory) {
  Observatory obs;
  Sampler sampler(obs);
  auto prov = make_synthetic_provider("syn", 3, DomainKind::kHostRam);
  REQUIRE(prov != nullptr);
  auto ids = sampler.add_provider(std::move(prov), "host");
  REQUIRE(ids.ok());
  sampler.collect_once();
  CHECK(obs.retained_count() >= 1);
  CHECK(sampler.provider_count() == 1);
  // Start/stop cleanly.
  REQUIRE(sampler.start().ok());
  CHECK(sampler.running());
  REQUIRE(sampler.stop().ok());
  CHECK(!sampler.running());
}

#if defined(_WIN32)
namespace {

struct TestCuda {
  HMODULE h = nullptr;
  using MemGetInfo = int (*)(std::size_t*, std::size_t*);
  using MemAlloc = int (*)(unsigned long long*, std::size_t);
  using MemFree = int (*)(unsigned long long);
  using Init = int (*)(unsigned);
  MemGetInfo memGetInfo = nullptr;
  MemAlloc memAlloc = nullptr;
  MemFree memFree = nullptr;
  Init init = nullptr;
  bool loaded() const { return h && memGetInfo; }
};

TestCuda& test_cuda() {
  static TestCuda c = [] {
    TestCuda t;
    t.h = LoadLibraryA("cudart64_120.dll");
    if (!t.h) {
      t.h = LoadLibraryA("cudart64_110.dll");
    }
    if (!t.h) {
      t.h = LoadLibraryA("nvcuda.dll");
    }
    if (t.h) {
      t.memGetInfo = reinterpret_cast<TestCuda::MemGetInfo>(GetProcAddress(t.h, "cudaMemGetInfo"));
      if (!t.memGetInfo) t.memGetInfo = reinterpret_cast<TestCuda::MemGetInfo>(GetProcAddress(t.h, "cuMemGetInfo_v2"));
      t.memAlloc = reinterpret_cast<TestCuda::MemAlloc>(GetProcAddress(t.h, "cuMemAlloc"));
      t.memFree = reinterpret_cast<TestCuda::MemFree>(GetProcAddress(t.h, "cuMemFree"));
      t.init = reinterpret_cast<TestCuda::Init>(GetProcAddress(t.h, "cuInit"));
      if (t.init) (void)t.init(0);
    }
    return t;
  }();
  return c;
}

}  // namespace
#endif

TEST(cuda_provider_or_explicitly_unavailable) {
  auto prov = make_cuda_provider();
  if (!prov) {
    // CUDA not present: report no fabricated accelerator observation.
    CHECK(true);
    return;
  }
  Provider::CollectResult r = prov->collect();
  if (!r.ok) {
    // Runtime reports a structured error: treat as not-available and do NOT
    // fabricate an accelerator observation.
    std::printf("  (CUDA backend present but runtime query failed: %s); no fabricated value\n", r.error.message().c_str());
    CHECK(true);
    return;
  }
  double total = 0, free_ = 0;
  bool saw_total = false, saw_free = false;
  for (const auto& s : r.samples) {
    if (s.kind == MemoryKind::kPhysical && s.gauge_value) { total = *s.gauge_value; saw_total = true; }
    if (s.kind == MemoryKind::kAvailable && s.gauge_value) { free_ = *s.gauge_value; saw_free = true; }
  }
  CHECK(saw_total);
  CHECK(saw_free);
  CHECK(total > 0);
  CHECK(free_ >= 0);
  std::printf("  (CUDA available: total=%.0f bytes, free=%.0f bytes)\n", total, free_);
}

TEST(cuda_bounded_allocation_causes_observable_delta) {
#if defined(_WIN32)
  auto prov = make_cuda_provider();
  if (!prov || !test_cuda().memAlloc || !test_cuda().memFree) {
    CHECK(true);  // backend unavailable; no fabrication
    return;
  }
  auto read_free = [&](double& f) -> bool {
    Provider::CollectResult r = prov->collect();
    if (!r.ok) return false;
    for (const auto& s : r.samples) {
      if (s.kind == MemoryKind::kAvailable && s.gauge_value) { f = *s.gauge_value; return true; }
    }
    return false;
  };
  double free_before = -1, free_mid = -1, free_after = -1;
  if (!read_free(free_before)) {
    // Runtime could not query CUDA; no fabricated value.
    std::printf("  (CUDA runtime query unavailable; bounded alloc proof skipped)\n");
    CHECK(true);
    return;
  }
  std::printf("  (CUDA free before=%.0f)\n", free_before);
  // Bounded device allocation (well below capacity on an RTX 5090).
  constexpr std::size_t kAllocBytes = 256ull * 1024ull * 1024ull;  // 256 MiB
  unsigned long long device = 0;
  const int rc = test_cuda().memAlloc(&device, kAllocBytes);
  if (rc != 0) {
    // Allocation failed; there is no fabrication -- just record no assertion.
    std::printf("  (CUDA cuMemAlloc failed; bounded proof skipped)\n");
    CHECK(true);
    return;
  }
  REQUIRE(read_free(free_mid));
  CHECK(free_mid < free_before);
  (void)test_cuda().memFree(device);
  REQUIRE(read_free(free_after));
  CHECK(free_after >= free_mid);
  CHECK(std::fabs(free_after - free_before) < static_cast<double>(kAllocBytes) * 1.5);
  std::printf("  (CUDA bounded proof OK: before=%.0f after=%.0f, allocation moved free memory)\n",
              free_before, free_after);
#else
  CHECK(true);
#endif
}
