# Providers

A `Provider` collects memory-domain observations without mutating the system. It returns raw `Sample` records (gauge/counter/distribution plus domain kind and annotations); the `Sampler` turns them into `Observation`s, attaching source identity, domain mapping, provenance, and a monotonic timestamp.

Built-in providers: `synthetic` (deterministic seeded), `host` (Windows physical memory), `process` (Windows process working set/commit/private bytes), `system-commit` (Windows commit charge), `cuda` (accelerator free/total via a dynamically loaded driver API), and `push` (a sink for externally pushed samples).

When a backend is unavailable, the provider returns `ok=false` with a structured `no_backend`/provider error -- never a fabricated value. Optional seams exist for HIP/ROCm, Level Zero, Metal, Vulkan, NVML, and CXL, but no claim of implementation is made unless it is implemented and validated.
