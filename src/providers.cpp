// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/provider.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

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
      : name_(std::move(name)), kind_(kind) {
    (void)seed;
  }
  std::string name() const override { return name_; }
  std::string kind() const override { return "synthetic"; }
  std::string provider_version() const override { return "1.0.0"; }
  DomainKind primary_domain() const override { return kind_; }
  bool available() const override { return true; }
  CollectResult collect() override {
    ++tick_;
    const double base = 16.0 * 1024.0 * 1024.0 * 1024.0;  // 16 GiB physical
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
  (void)seed;
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

// ============================================================================
// CUDA provider.
//
// Root cause note: cuMemGetInfo_v2 (driver API) requires a CUDA context to be
// current on the calling thread. A typical fresh process has no context, so the
// call returns CUDA_ERROR_INVALID_CONTEXT. We therefore retain the device's
// PRIMARY context (cuDevicePrimaryCtxRetain) and bind it with cuCtxSetCurrent
// before querying. The CUDA runtime (cudart64_*), when present, auto-creates a
// context on first use and is preferred; it simply is not installed on every
// machine. We never fabricate a value.
// ============================================================================
#if defined(_WIN32)
namespace {

struct CudaAccess {
  bool is_runtime = false;
  HMODULE h = nullptr;

  using RMemGetInfo = int (*)(std::size_t*, std::size_t*);
  using RSetDevice = int (*)(int);
  using RGetDevice = int (*)(int*);
  using RSync = int (*)(void);
  using RDeviceCount = int (*)(int*);
  using RDeviceProps = int (*)(void*, int);
  using RDriverVersion = int (*)(int*);
  using RRuntimeVersion = int (*)(int*);
  RMemGetInfo rMemGetInfo = nullptr;
  RSetDevice rSetDevice = nullptr;
  RGetDevice rGetDevice = nullptr;
  RSync rSync = nullptr;
  RDeviceCount rDeviceCount = nullptr;
  RDeviceProps rDeviceProps = nullptr;
  RDriverVersion rDriverVersion = nullptr;
  RRuntimeVersion rRuntimeVersion = nullptr;

  using DMemGetInfo = int (*)(std::size_t*, std::size_t*);
  using DInit = int (*)(unsigned);
  using DDeviceCount = int (*)(int*);
  using DDeviceGet = int (*)(int*, int);
  using DDeviceName = int (*)(char*, int, int);
  using DCtxRetain = int (*)(std::uint64_t*, int);
  using DCtxSet = int (*)(std::uint64_t);
  using DCtxGet = int (*)(std::uint64_t*);
  using DSync = int (*)(void);
  using DDriverVersion = int (*)(int*);
  using DGetErrorString = int (*)(int, const char**);
  DMemGetInfo dMemGetInfo = nullptr;
  DInit dInit = nullptr;
  DDeviceCount dDeviceCount = nullptr;
  DDeviceGet dDeviceGet = nullptr;
  DDeviceName dDeviceName = nullptr;
  DCtxRetain dCtxRetain = nullptr;
  DCtxSet dCtxSet = nullptr;
  DCtxGet dCtxGet = nullptr;
  DSync dSync = nullptr;
  DDriverVersion dDriverVersion = nullptr;
  DGetErrorString dGetErrorString = nullptr;

  std::uint64_t context = 0;
  bool context_ready = false;

  bool loaded() const { return h != nullptr && (rMemGetInfo != nullptr || dMemGetInfo != nullptr); }

  std::string error_string(int code) const {
    if (is_runtime) {
      using ErrStr = const char* (*)(int);
      ErrStr f = reinterpret_cast<ErrStr>(GetProcAddress(h, "cudaGetErrorString"));
      if (f) {
        const char* s = f(code);
        return s ? std::string(s) : std::string("unknown");
      }
    } else {
      if (dGetErrorString) {
        const char* s = nullptr;
        if (dGetErrorString(code, &s) == 0 && s) return std::string(s);
      }
    }
    return "unknown";
  }

  ~CudaAccess() {
    if (h) FreeLibrary(h);
  }
};

template <typename T>
T loadfn(HMODULE h, const char* name) {
  return reinterpret_cast<T>(GetProcAddress(h, name));
}

CudaAccess load_cuda() {
  CudaAccess a;
  const char* rt_candidates[] = {"cudart64_120.dll", "cudart64_110.dll", "cudart64_100.dll"};
  for (const char* c : rt_candidates) {
    HMODULE h = LoadLibraryA(c);
    if (h) {
      a.h = h;
      a.is_runtime = true;
      a.rMemGetInfo = loadfn<CudaAccess::RMemGetInfo>(h, "cudaMemGetInfo");
      a.rSetDevice = loadfn<CudaAccess::RSetDevice>(h, "cudaSetDevice");
      a.rGetDevice = loadfn<CudaAccess::RGetDevice>(h, "cudaGetDevice");
      a.rSync = loadfn<CudaAccess::RSync>(h, "cudaDeviceSynchronize");
      a.rDeviceCount = loadfn<CudaAccess::RDeviceCount>(h, "cudaGetDeviceCount");
      a.rDeviceProps = loadfn<CudaAccess::RDeviceProps>(h, "cudaGetDeviceProperties");
      a.rDriverVersion = loadfn<CudaAccess::RDriverVersion>(h, "cudaDriverGetVersion");
      a.rRuntimeVersion = loadfn<CudaAccess::RRuntimeVersion>(h, "cudaRuntimeGetVersion");
      break;
    }
  }
  if (!a.h) {
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (h) {
      a.h = h;
      a.is_runtime = false;
      a.dInit = loadfn<CudaAccess::DInit>(h, "cuInit");
      a.dDeviceCount = loadfn<CudaAccess::DDeviceCount>(h, "cuDeviceGetCount");
      a.dDeviceGet = loadfn<CudaAccess::DDeviceGet>(h, "cuDeviceGet");
      a.dDeviceName = loadfn<CudaAccess::DDeviceName>(h, "cuDeviceGetName");
      a.dMemGetInfo = loadfn<CudaAccess::DMemGetInfo>(h, "cuMemGetInfo_v2");
      if (!a.dMemGetInfo) a.dMemGetInfo = loadfn<CudaAccess::DMemGetInfo>(h, "cuMemGetInfo");
      a.dCtxRetain = loadfn<CudaAccess::DCtxRetain>(h, "cuDevicePrimaryCtxRetain");
      a.dCtxSet = loadfn<CudaAccess::DCtxSet>(h, "cuCtxSetCurrent");
      a.dCtxGet = loadfn<CudaAccess::DCtxGet>(h, "cuCtxGetCurrent");
      a.dSync = loadfn<CudaAccess::DSync>(h, "cuCtxSynchronize");
      a.dDriverVersion = loadfn<CudaAccess::DDriverVersion>(h, "cuDriverGetVersion");
      a.dGetErrorString = loadfn<CudaAccess::DGetErrorString>(h, "cuGetErrorString");
    }
  }
  if (!a.loaded()) {
    if (a.h) FreeLibrary(a.h);
    a.h = nullptr;
  }
  return a;
}

CudaAccess& cuda() {
  static CudaAccess a = load_cuda();
  return a;
}

std::string query_cuda(std::size_t& free_b, std::size_t& total_b, int& device_ordinal,
                       std::string& device_name, std::string& driver_version,
                       std::string& runtime_version) {
  // The backend handle and its retained context are shared by every provider
  // instance and every thread that samples.
  static std::mutex query_mutex;
  std::lock_guard<std::mutex> query_lock(query_mutex);
  CudaAccess& a = cuda();
  int rc = 0;
  int dev_count = 0;
  if (a.is_runtime) {
    if (!a.rMemGetInfo) return "cudaMemGetInfo symbol missing";
    rc = a.rDeviceCount ? a.rDeviceCount(&dev_count) : -1;
    if (rc != 0) return "cudaGetDeviceCount: " + a.error_string(rc);
    if (dev_count <= 0) return "no CUDA device present";
    // Sampling is observation only: the calling thread goes back to the device
    // it was using, on every way out.
    int previous_device = 0;
    const bool have_previous = a.rGetDevice && a.rGetDevice(&previous_device) == 0;
    rc = a.rSetDevice ? a.rSetDevice(0) : -1;
    if (rc != 0) return "cudaSetDevice(0): " + a.error_string(rc);
    const int sync_rc = a.rSync ? a.rSync() : 0;
    const int mem_rc = sync_rc == 0 ? a.rMemGetInfo(&free_b, &total_b) : 0;
    if (have_previous && previous_device != 0) (void)a.rSetDevice(previous_device);
    if (sync_rc != 0) return "cudaDeviceSynchronize: " + a.error_string(sync_rc);
    if (mem_rc != 0) return "cudaMemGetInfo: " + a.error_string(mem_rc);
    device_ordinal = 0;
    int dv = 0;
    if (a.rDriverVersion) a.rDriverVersion(&dv);
    driver_version = std::to_string(dv);
    int rv = 0;
    if (a.rRuntimeVersion) a.rRuntimeVersion(&rv);
    runtime_version = std::to_string(rv);
    if (a.rDeviceProps) {
      // The runtime fills in a whole cudaDeviceProp, which is far larger than
      // its leading name field and grows between toolkit versions.
      std::vector<unsigned char> props(16384, 0);
      if (a.rDeviceProps(props.data(), 0) == 0) {
        const char* name = reinterpret_cast<const char*>(props.data());
        device_name.assign(name, strnlen(name, 256));
      }
    }
    if (device_name.empty()) device_name = "cuda-device-0";
    return "";
  }

  if (!a.dMemGetInfo) return "cuMemGetInfo symbol missing";
  rc = a.dInit ? a.dInit(0) : -1;
  if (rc != 0) return "cuInit: " + a.error_string(rc);
  rc = a.dDeviceCount ? a.dDeviceCount(&dev_count) : -1;
  if (rc != 0) return "cuDeviceGetCount: " + a.error_string(rc);
  if (dev_count <= 0) return "no CUDA device present";
  int dev = 0;
  rc = a.dDeviceGet ? a.dDeviceGet(&dev, 0) : -1;
  if (rc != 0) return "cuDeviceGet(0): " + a.error_string(rc);
  if (!a.context_ready) {
    if (!a.dCtxRetain) return "cuDevicePrimaryCtxRetain symbol missing";
    rc = a.dCtxRetain(&a.context, 0);
    if (rc != 0) return "cuDevicePrimaryCtxRetain: " + a.error_string(rc);
    a.context_ready = true;
  }
  // Sampling is observation only: whatever context the calling thread had
  // current is current again once the reading is taken.
  std::uint64_t previous_context = 0;
  const bool have_previous = a.dCtxGet && a.dCtxGet(&previous_context) == 0;
  rc = a.dCtxSet ? a.dCtxSet(a.context) : -1;
  if (rc != 0) return "cuCtxSetCurrent: " + a.error_string(rc);
  rc = a.dMemGetInfo(&free_b, &total_b);
  if (have_previous && previous_context != a.context) {
    (void)a.dCtxSet(previous_context);
  }
  if (rc != 0) return "cuMemGetInfo_v2: " + a.error_string(rc);
  device_ordinal = dev;
  char nm[256] = {};
  if (a.dDeviceName) a.dDeviceName(nm, sizeof(nm), dev);
  device_name = nm[0] ? std::string(nm) : "cuda-device-0";
  int dv = 0;
  if (a.dDriverVersion) a.dDriverVersion(&dv);
  driver_version = std::to_string(dv);
  runtime_version = "n/a (runtime not loaded; using cuda driver API)";
  return "";
}

}  // namespace
#endif

class CudaProvider final : public Provider {
 public:
  std::string name() const override { return "cuda"; }
  std::string kind() const override { return "cuda"; }
  std::string provider_version() const override { return "cuda-provider-1.0"; }
  DomainKind primary_domain() const override { return DomainKind::kAcceleratorDevice; }
  bool available() const override { return cuda().loaded(); }
  CollectResult collect() override {
    CollectResult r;
    std::size_t free_b = 0, total_b = 0;
    int ordinal = 0;
    std::string dev_name, drv_ver, rt_ver;
    std::string err = query_cuda(free_b, total_b, ordinal, dev_name, drv_ver, rt_ver);
    if (!err.empty()) {
      ErrorCode code = ErrorCode::kBackendUnavailable;
      if (err.find("no CUDA device") != std::string::npos ||
          err.find("symbol missing") != std::string::npos) {
        code = ErrorCode::kNoBackend;
      } else {
        code = ErrorCode::kProviderError;
      }
      r.ok = false;
      r.error = Error{code, err};
      return r;
    }
    r.ok = true;
    std::vector<Annotation> ann = {
        {"device_ordinal", std::to_string(ordinal)},
        {"device_name", dev_name},
        {"driver_version", drv_ver},
        {"runtime_version", rt_ver},
        {"cuda_backend", cuda().is_runtime ? "cuda_runtime" : "cuda_driver"},
    };
    Sample avail;
    avail.domain_kind = DomainKind::kAcceleratorDevice;
    avail.kind = MemoryKind::kAvailable;
    avail.scale = Scale::kBytes;
    avail.gauge_value = static_cast<double>(free_b);
    avail.annotations = ann;
    r.samples.push_back(std::move(avail));
    Sample total;
    total.domain_kind = DomainKind::kAcceleratorDevice;
    total.kind = MemoryKind::kPhysical;
    total.scale = Scale::kBytes;
    total.gauge_value = static_cast<double>(total_b);
    total.annotations = std::move(ann);
    r.samples.push_back(std::move(total));
    return r;
  }
};

std::unique_ptr<Provider> make_cuda_provider() {
#if defined(_WIN32)
  if (!cuda().loaded()) {
    return nullptr;  // no CUDA backend -> no fabricated observation
  }
  return std::make_unique<CudaProvider>();
#else
  return nullptr;
#endif
}

}  // namespace memory_observatory
