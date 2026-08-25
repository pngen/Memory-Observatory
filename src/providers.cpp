// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/provider.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace memory_observatory {

namespace {

class SyntheticProvider final : public Provider {
 public:
  SyntheticProvider(std::string name, std::uint64_t seed, DomainKind kind)
      : name_(std::move(name)), seed_(seed), kind_(kind) {}
  std::string name() const override { return name_; }
  std::string kind() const override { return "synthetic"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return kind_; }
  bool available() const override { return true; }
  CollectResult collect() override {
    ++tick_;
    const double base = 16.0 * 1024.0 * 1024.0 * 1024.0;  // 16 GiB physical
    // A deterministic, gently rising allocation series so diagnostics can fire.
    const double alloc = base * (0.40 + 0.10 * std::sin(static_cast<double>(tick_) * 0.1) +
                                 0.03 * static_cast<double>(tick_ % 20) / 20.0);
    const double avail = base - alloc;
    CollectResult r;
    r.ok = true;
    Sample alloc_s;
    alloc_s.domain_kind = kind_;
    alloc_s.kind = MemoryKind::kAllocated;
    alloc_s.scale = Scale::kBytes;
    alloc_s.gauge_value = alloc;
    r.samples.push_back(std::move(alloc_s));
    Sample avail_s;
    avail_s.domain_kind = kind_;
    avail_s.kind = MemoryKind::kAvailable;
    avail_s.scale = Scale::kBytes;
    avail_s.gauge_value = avail;
    r.samples.push_back(std::move(avail_s));
    Sample phys_s;
    phys_s.domain_kind = kind_;
    phys_s.kind = MemoryKind::kPhysical;
    phys_s.scale = Scale::kBytes;
    phys_s.gauge_value = base;
    r.samples.push_back(std::move(phys_s));
    return r;
  }
 private:
  std::string name_;
  std::uint64_t seed_;
  DomainKind kind_;
  std::uint64_t tick_ = 0;
};

#if defined(_WIN32)
class HostMemoryProvider final : public Provider {
 public:
  std::string name() const override { return "host"; }
  std::string kind() const override { return "host"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return DomainKind::kHostRam; }
  bool available() const override { return true; }
  CollectResult collect() override {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    CollectResult r;
    if (!GlobalMemoryStatusEx(&m)) {
      r.ok = false;
      r.error = Error{ErrorCode::kProviderError, "GlobalMemoryStatusEx failed"};
      return r;
    }
    r.ok = true;
    Sample phys;
    phys.domain_kind = DomainKind::kHostRam;
    phys.kind = MemoryKind::kPhysical;
    phys.scale = Scale::kBytes;
    phys.gauge_value = static_cast<double>(m.ullTotalPhys);
    r.samples.push_back(std::move(phys));
    Sample avail;
    avail.domain_kind = DomainKind::kHostRam;
    avail.kind = MemoryKind::kAvailable;
    avail.scale = Scale::kBytes;
    avail.gauge_value = static_cast<double>(m.ullAvailPhys);
    r.samples.push_back(std::move(avail));
    return r;
  }
};

class SystemCommitProvider final : public Provider {
 public:
  std::string name() const override { return "system-commit"; }
  std::string kind() const override { return "system-commit"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return DomainKind::kSystemCommit; }
  bool available() const override { return true; }
  CollectResult collect() override {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    CollectResult r;
    if (!GlobalMemoryStatusEx(&m)) {
      r.ok = false;
      r.error = Error{ErrorCode::kProviderError, "GlobalMemoryStatusEx failed"};
      return r;
    }
    r.ok = true;
    // Commit charge = page file committed.
    Sample committed;
    committed.domain_kind = DomainKind::kSystemCommit;
    committed.kind = MemoryKind::kCommitted;
    committed.scale = Scale::kBytes;
    committed.counter_value = static_cast<std::uint64_t>(m.ullTotalPageFile - m.ullAvailPageFile);
    r.samples.push_back(std::move(committed));
    Sample commit_limit;
    commit_limit.domain_kind = DomainKind::kSystemCommit;
    commit_limit.kind = MemoryKind::kGoverned;
    commit_limit.scale = Scale::kBytes;
    commit_limit.gauge_value = static_cast<double>(m.ullTotalPageFile);
    r.samples.push_back(std::move(commit_limit));
    return r;
  }
};

class ProcessMemoryProvider final : public Provider {
 public:
  std::string name() const override { return "process-" + std::to_string(GetCurrentProcessId()); }
  std::string kind() const override { return "process"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return DomainKind::kProcessCommit; }
  bool available() const override { return true; }
  CollectResult collect() override {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    CollectResult r;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
      r.ok = false;
      r.error = Error{ErrorCode::kProviderError, "GetProcessMemoryInfo failed"};
      return r;
    }
    r.ok = true;
    Sample working_set;
    working_set.domain_kind = DomainKind::kProcessCommit;
    working_set.kind = MemoryKind::kResidency;
    working_set.scale = Scale::kBytes;
    working_set.gauge_value = static_cast<double>(pmc.WorkingSetSize);
    r.samples.push_back(std::move(working_set));
    Sample commit;
    commit.domain_kind = DomainKind::kProcessCommit;
    commit.kind = MemoryKind::kCommitted;
    commit.scale = Scale::kBytes;
    commit.counter_value = static_cast<std::uint64_t>(pmc.PagefileUsage);
    r.samples.push_back(std::move(commit));
    Sample private_bytes;
    private_bytes.domain_kind = DomainKind::kProcessCommit;
    private_bytes.kind = MemoryKind::kAllocated;
    private_bytes.scale = Scale::kBytes;
    private_bytes.gauge_value = static_cast<double>(pmc.PrivateUsage);
    r.samples.push_back(std::move(private_bytes));
    return r;
  }
};
#endif  // _WIN32

class PushProvider final : public Provider {
 public:
  explicit PushProvider(std::string name) : name_(std::move(name)) {}
  std::string name() const override { return name_; }
  std::string kind() const override { return "push"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return DomainKind::kUserDefined; }
  bool available() const override { return true; }
  CollectResult collect() override {
    // A push provider does not collect; it receives observations externally.
    CollectResult r;
    r.ok = true;
    return r;
  }
  void push(Sample s) { pending_.push_back(std::move(s)); }
  std::vector<Sample> drain() { return std::move(pending_); }
 private:
  std::string name_;
  std::vector<Sample> pending_;
};

}  // namespace

std::unique_ptr<Provider> make_synthetic_provider(const std::string& name,
                                                  std::uint64_t seed,
                                                  DomainKind domain_kind) {
  return std::make_unique<SyntheticProvider>(name, seed, domain_kind);
}
std::unique_ptr<Provider> make_host_memory_provider() {
#if defined(_WIN32)
  return std::make_unique<HostMemoryProvider>();
#else
  return nullptr;
#endif
}
std::unique_ptr<Provider> make_process_memory_provider() {
#if defined(_WIN32)
  return std::make_unique<ProcessMemoryProvider>();
#else
  return nullptr;
#endif
}
std::unique_ptr<Provider> make_system_commit_provider() {
#if defined(_WIN32)
  return std::make_unique<SystemCommitProvider>();
#else
  return nullptr;
#endif
}
std::unique_ptr<Provider> make_push_provider(const std::string& name) {
  return std::make_unique<PushProvider>(name);
}

// CUDA provider: dynamically loads the driver API for cuMemGetInfo/cudaMemGetInfo.
// Never fabricates an accelerator observation; returns unavailable when CUDA is
// not present. The driver handle is loaded once as a process-lifetime singleton.
#if defined(_WIN32)
namespace {
struct CudaDriverAccess {
  HMODULE h = nullptr;
  using MemGetInfo = int (*)(std::size_t*, std::size_t*);
  using Init = int (*)(unsigned);
  MemGetInfo memGetInfo = nullptr;
  Init init = nullptr;
  bool initialized = false;
  bool loaded() const { return h != nullptr && memGetInfo != nullptr; }
  ~CudaDriverAccess() {
    if (h) FreeLibrary(h);
  }
};

CudaDriverAccess load_cuda_driver() {
  CudaDriverAccess d;
  const char* candidates[] = {"cudart64_120.dll", "cudart64_110.dll",
                              "cudart64_100.dll", "nvcuda.dll"};
  for (const char* c : candidates) {
    d.h = LoadLibraryA(c);
    if (d.h) {
      d.memGetInfo = reinterpret_cast<CudaDriverAccess::MemGetInfo>(
          GetProcAddress(d.h, "cudaMemGetInfo"));
      if (!d.memGetInfo) {
        d.memGetInfo = reinterpret_cast<CudaDriverAccess::MemGetInfo>(
            GetProcAddress(d.h, "cuMemGetInfo_v2"));
      }
      d.init = reinterpret_cast<CudaDriverAccess::Init>(GetProcAddress(d.h, "cuInit"));
      // If we loaded the runtime library, cudaMemGetInfo auto-initializes; no
      // separate cuInit is needed.
      if (d.memGetInfo && std::strstr(c, "cudart")) d.initialized = true;
      break;
    }
  }
  if (!d.loaded()) {
    if (d.h) FreeLibrary(d.h);
    d.h = nullptr;
    d.memGetInfo = nullptr;
  }
  return d;
}

CudaDriverAccess& cuda_driver() {
  static CudaDriverAccess d = load_cuda_driver();
  return d;
}
}  // namespace
#endif
std::unique_ptr<Provider> make_cuda_provider() {
#if defined(_WIN32)
  if (!cuda_driver().loaded()) {
    return nullptr;  // backend unavailable -> no fabricated observation
  }
  struct CudaProvider final : public Provider {
    std::string name() const override { return "cuda"; }
    std::string kind() const override { return "cuda"; }
    std::string provider_version() const override { return "dynamic"; }
    DomainKind primary_domain() const override { return DomainKind::kAcceleratorDevice; }
    bool available() const override { return cuda_driver().loaded(); }
    CollectResult collect() override {
      CollectResult r;
      CudaDriverAccess& d = cuda_driver();
      if (!d.loaded()) {
        r.ok = false;
        r.error = Error{ErrorCode::kNoBackend, "CUDA backend unavailable"};
        return r;
      }
      if (!d.initialized) {
        if (!d.init) {
          r.ok = false;
          r.error = Error{ErrorCode::kProviderError, "CUDA driver lacks cuInit"};
          return r;
        }
        if (d.init(0) != 0) {
          r.ok = false;
          r.error = Error{ErrorCode::kProviderError, "cuInit failed"};
          return r;
        }
        d.initialized = true;
      }
      std::size_t free_b = 0, total_b = 0;
      const int rc = d.memGetInfo(&free_b, &total_b);
      if (rc != 0) {
        r.ok = false;
        r.error = Error{ErrorCode::kProviderError, "cuMemGetInfo failed"};
        return r;
      }
      r.ok = true;
      Sample avail;
      avail.domain_kind = DomainKind::kAcceleratorDevice;
      avail.kind = MemoryKind::kAvailable;
      avail.scale = Scale::kBytes;
      avail.gauge_value = static_cast<double>(free_b);
      r.samples.push_back(std::move(avail));
      Sample total;
      total.domain_kind = DomainKind::kAcceleratorDevice;
      total.kind = MemoryKind::kPhysical;
      total.scale = Scale::kBytes;
      total.gauge_value = static_cast<double>(total_b);
      r.samples.push_back(std::move(total));
      return r;
    }
  };
  return std::make_unique<CudaProvider>();
#else
  return nullptr;
#endif
}

}  // namespace memory_observatory

