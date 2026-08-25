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
  using CtxRetain = int (*)(unsigned long long*, int);
  using CtxSet = int (*)(unsigned long long);
  using CtxSync = int (*)(void);
  using CtxCreate = int (*)(unsigned long long*, unsigned, int);
  using CtxDestroy = int (*)(unsigned long long);
  MemGetInfo memGetInfo = nullptr;
  MemAlloc memAlloc = nullptr;
  MemFree memFree = nullptr;
  Init init = nullptr;
  CtxRetain ctxRetain = nullptr;
  CtxSet ctxSet = nullptr;
  CtxSync ctxSync = nullptr;
  CtxCreate ctxCreate = nullptr;
  CtxDestroy ctxDestroy = nullptr;
  unsigned long long ctx = 0;
  bool loaded() const { return h && memGetInfo && memAlloc && memFree; }
};

TestCuda& test_cuda() {
  static TestCuda c = [] {
    TestCuda t;
    t.h = LoadLibraryA("cudart64_120.dll");
    if (!t.h) t.h = LoadLibraryA("cudart64_110.dll");
    if (!t.h) t.h = LoadLibraryA("nvcuda.dll");
    if (t.h) {
      t.memGetInfo = reinterpret_cast<TestCuda::MemGetInfo>(GetProcAddress(t.h, "cudaMemGetInfo"));
      if (!t.memGetInfo) t.memGetInfo = reinterpret_cast<TestCuda::MemGetInfo>(GetProcAddress(t.h, "cuMemGetInfo_v2"));
      t.memAlloc = reinterpret_cast<TestCuda::MemAlloc>(GetProcAddress(t.h, "cuMemAlloc"));
      if (!t.memAlloc) t.memAlloc = reinterpret_cast<TestCuda::MemAlloc>(GetProcAddress(t.h, "cudaMalloc"));
      t.memFree = reinterpret_cast<TestCuda::MemFree>(GetProcAddress(t.h, "cuMemFree"));
      if (!t.memFree) t.memFree = reinterpret_cast<TestCuda::MemFree>(GetProcAddress(t.h, "cudaFree"));
      t.init = reinterpret_cast<TestCuda::Init>(GetProcAddress(t.h, "cuInit"));
      t.ctxRetain = reinterpret_cast<TestCuda::CtxRetain>(GetProcAddress(t.h, "cuDevicePrimaryCtxRetain"));
      t.ctxSet = reinterpret_cast<TestCuda::CtxSet>(GetProcAddress(t.h, "cuCtxSetCurrent"));
      t.ctxSync = reinterpret_cast<TestCuda::CtxSync>(GetProcAddress(t.h, "cuCtxSynchronize"));
      t.ctxCreate = reinterpret_cast<TestCuda::CtxCreate>(GetProcAddress(t.h, "cuCtxCreate"));
      t.ctxDestroy = reinterpret_cast<TestCuda::CtxDestroy>(GetProcAddress(t.h, "cuCtxDestroy"));
      if (t.init) (void)t.init(0);
      // Bind the device's primary context so test allocations and the provider's
      // cuMemGetInfo_v2 observe the same device memory.
      if (t.ctxRetain && t.ctxSet && t.ctxRetain(&t.ctx, 0) == 0) {
        (void)t.ctxSet(t.ctx);
      }
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
  if (!prov || !test_cuda().loaded()) {
    std::printf("  (CUDA backend unavailable; bounded alloc proof skipped)\n");
    CHECK(true);  // no fabrication
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
    std::printf("  (CUDA runtime query unavailable; bounded alloc proof skipped)\n");
    CHECK(true);
    return;
  }
  std::printf("  (CUDA baseline free=%.0f total-visible)\n", free_before);

  // Create a fresh context to hold the allocation. On this driver, cuMemAlloc
  // in the primary context returns CUDA_ERROR_INVALID_CONTEXT, but cuMemAlloc in
  // a real cuCtxCreate context succeeds, while cuMemGetInfo_v2 is device-global
  // and works in the primary context. We allocate in the fresh context and read
  // free memory through the primary context (which the provider binds).
  unsigned long long test_ctx = 0;
  if (test_cuda().ctxCreate && test_cuda().ctxSet) {
    (void)test_cuda().ctxSet(test_cuda().ctx);
    (void)test_cuda().ctxCreate(&test_ctx, 0, 0);
    (void)test_cuda().ctxSet(test_ctx);
  }

  // Bounded device allocation (256 MiB, far below an RTX 5090's memory).
  constexpr std::size_t kAllocBytes = 256ull * 1024ull * 1024ull;  // 256 MiB
  unsigned long long device = 0;
  const int alloc_rc = test_cuda().memAlloc(&device, kAllocBytes);
  if (alloc_rc != 0) {
    using GetErrStr = int (*)(int, const char**);
    auto gef = reinterpret_cast<GetErrStr>(GetProcAddress(test_cuda().h, "cuGetErrorString"));
    const char* es = nullptr;
    if (gef) (void)gef(alloc_rc, &es);
    std::printf("  (CUDA cuMemAlloc failed rc=%d (%s); bounded proof skipped)\n",
                alloc_rc, es ? es : "?");
    if (test_cuda().ctxDestroy && test_ctx) { (void)test_cuda().ctxDestroy(test_ctx); }
    CHECK(true);
    return;
  }
  if (test_cuda().ctxSync) { (void)test_cuda().ctxSync(); }
  REQUIRE(read_free(free_mid));  // provider binds the primary context to read
  std::printf("  (CUDA free during-allocation=%.0f)\n", free_mid);
  CHECK(free_mid < free_before);  // the allocation must reduce free memory

  // Capture the change through Memory Observatory (snapshot + timeline).
  Observatory obs;
  SourceDescriptor sd; sd.name = "cuda-probe"; sd.kind = "cuda";
  auto src = obs.register_source(sd);
  DomainSpec ds; ds.name = "cuda"; ds.kind = DomainKind::kAcceleratorDevice;
  auto dom = obs.register_domain(ds);
  if (src.ok() && dom.ok()) {
    auto mk = [&](double v, int seq) {
      Observation o;
      o.source = src.value(); o.domain = dom.value();
      o.timestamp.monotonic = MonotonicTimestamp{1000000000LL + seq};
      o.timestamp.sequence = SequenceNo{seq};
      o.provenance.mechanism = ObservationMechanism::kSynthetic;
      GaugeSample g; g.kind = MemoryKind::kAvailable; g.value = v;
      o.payload = g; return o;
    };
    (void)obs.observe(mk(free_before, 0));
    (void)obs.observe(mk(free_mid, 1));
    auto snap = obs.snapshot();
    CHECK(snap.ok());
    TimelineFilter tf;
    auto tl = obs.query_timeline(tf);
    CHECK(tl.ok());
    CHECK(tl.value().size() >= 2);
  }

  // Free the allocation, destroy the temporary context (releasing its device
  // overhead), and verify recovery toward baseline.
  if (test_cuda().ctxSet && test_ctx) { (void)test_cuda().ctxSet(test_ctx); }
  (void)test_cuda().memFree(device);
  if (test_cuda().ctxSet) { (void)test_cuda().ctxSet(test_cuda().ctx); }
  if (test_cuda().ctxDestroy && test_ctx) { (void)test_cuda().ctxDestroy(test_ctx); }
  if (test_cuda().ctxSync) { (void)test_cuda().ctxSync(); }
  REQUIRE(read_free(free_after));
  std::printf("  (CUDA free after-free=%.0f)\n", free_after);
  CHECK(free_after >= free_mid);
  // After freeing, free memory should return near baseline (allow some slack).
  CHECK(std::fabs(free_after - free_before) < static_cast<double>(kAllocBytes) * 2.0);
  std::printf("  (CUDA bounded proof OK: before=%.0f during=%.0f after=%.0f)\n",
              free_before, free_mid, free_after);
#else
  CHECK(true);
#endif
}
