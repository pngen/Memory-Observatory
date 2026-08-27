# Memory Observatory

Memory Observatory is an open-source, vendor-neutral observability runtime for
measuring, correlating, explaining, and replaying memory behavior across
heterogeneous AI infrastructure.

## The core question

> **What is memory doing across the system, why is it behaving that way, and what evidence explains how that behavior changed over time?**

Memory Observatory measures memory behavior: capacity, utilization, allocation
activity, residency changes, pressure, movement, reclaim activity, latency,
churn, fragmentation *where observable*, provenance, and temporal correlation.

## Ownership boundary

Memory Observatory is a *measurement* runtime. It **owns** observation,
measurement, metric collection, event ingestion, snapshots, timelines, traces,
correlation, diagnostics, provenance, replay, and reporting. It **does not**
own allocation policy, authoritative placement, byte movement, reclaim
decisions, cache eviction/admission, execution scheduling, quota enforcement,
resource admission, pressure control, memory persistence, or hardware topology
discovery (unless data is supplied through a provider).

> **Memory Observatory never silently turns observations into commands.** It
> never allocates, moves, reclaims, evicts, demotes, promotes, schedules,
> places, throttles, rejects, admits, pins, persists, or restores external
> runtime state merely because it observed something. Any recommendation-like
> output is explicitly diagnostic/advisory and clearly labeled as such.

## What it does

- Registers heterogeneous memory domains (host RAM, process/system commit, pinned host, accelerator/device, shared, file-backed, persistent, user-defined).
- Ingests observations (metrics, counters, gauges, distributions, events, state payloads) with first-class provenance, confidence, freshness, generation, and epoch.
- Maintains a **bounded** historical timeline; no unbounded accumulation.
- Produces coherent snapshots, renderable per-domain, per-source, or whole-system.
- Captures versioned, deterministic traces (strict binary + human-readable JSON) and replays them offline into a fresh Observatory.
- Runs an evidence-based diagnostic engine and a correlation engine. Correlations are explicitly *not* causation; leak findings are labeled heuristic/suspect.
- Renders human-readable, JSON, and CSV reports, plus an offline `compare` engine for two traces (measured/derived/heuristic findings, no universal winner).
- Provides a provider interface and a bounded sampling engine with clean shutdown, and a robust CUDA provider that reports exact error codes, records device/driver provenance, and never fabricates an accelerator value.
- Includes a real two-process validation (sender/receiver) and a bounded real-device CUDA allocate/sample/free proof.

## Platforms and toolchain

The **primary validated platform** is Windows 11 x64 with MSVC 19.44+, CMake 4.3+, and C++20. Providers for host physical memory, process memory, and system commit use native Windows APIs. Non-Windows platforms and non-implemented backends are posted as unavailable / `no_backend`, never fabricated. CUDA observation is implemented via a dynamically loaded driver API, retains and binds the device primary context before querying, reports exact error codes and device/driver provenance, and never fabricates an accelerator value.

## Building

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Debug
cmake --install build --config Release --prefix _install
```

The library is built with `/W4 /WX` (warnings-as-errors) and zero compiler warnings. Tests, the CLI, examples, and a benchmark build alongside it.

## Using from CMake

```cmake
find_package(memory_observatory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MemoryObservatory::memory_observatory)
```

See `downstream/` for a complete independent consumer that builds, links, runs, ingests observations, creates a snapshot, and exits successfully.

## CLI

```text
memory-observatory info | providers | domains | observe | snapshot | watch |
                   timeline | diagnostics | explain | trace-record |
                   trace-inspect | trace-replay | report | benchmark
```

All commands support `--help` (run `memory-observatory` with no args), strict argument validation, JSON output where useful, and nonzero exit codes on failure.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
