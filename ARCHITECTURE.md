# Architecture

Memory Observatory is a single C++20 library with a clean, layered design. The public surface lives under `include/memory_observatory/`; implementation under `src/`.

## Layers

1. **Type layer** (`id.h`, `time.h`, `error.h`, `domain.h`, `provenance.h`, `types.h`, `counters.h`, `aggregate.h`): strong ids, a two-clock time model, value errors and `Result<T>`, domain capacity, provenance, confidence/freshness, accounting counters, and aggregation primitives.
2. **Observation model** (`observation.h`): the `Observation` currency and typed payloads (metric samples, events, state payloads).
3. **Runtime layer** (`observatory.h/.cpp`): the central `Observatory` with bounded history, source/domain registry, ingestion, snapshots, timelines, diagnostics, correlations, subscriptions, trace capture, and reporting.
4. **Provider layer** (`provider.h/.cpp`): a `Provider` interface and built-in providers (synthetic, host, process, system-commit, CUDA, push).
5. **Sampling layer** (`sampler.h/.cpp`): a bounded background sampler with clean shutdown that turns provider samples into observations.
6. **Serialization / replay** (`trace.h/.cpp`, `replay.h`): strict binary + JSON trace formats and deterministic offline replay.
7. **Reporting** (`report.h/.cpp`): pure renderers for text/JSON/CSV.
8. **Frontends**: the CLI (`cli/`), examples (`examples/`), and a self-benchmark (`benchmarks/`).

## Threading model

The `Observatory` guards its history, registries, and counters with a single mutex. Subscriber callbacks are **never** invoked while that lock (or the subscriber lock) is held; they are collected under the subscriber lock and dispatched after all internal locks are released. This makes subscription dispatch reentrancy-safe, and it means a callback may safely call back into the Observatory.

The `Sampler` runs a single worker thread on a condition-variable period. It never busy-waits. `stop()` sets an atomic flag, notifies the condition variable, and joins without holding any state the worker needs to exit.

`Result<T>` and `Error` are value types; exceptions never cross the public API boundary.

## Key invariants

- **Bounded everything**: history, dedup set, queue, snapshots, timelines.
- **Accounting invariant**: ingested_total == accepted (retained + evicted) + non-accepted (rejected/dropped/duplicate). Nothing is silently discarded.
- **Unknown stays unknown**: missing metrics are `nullopt`, never zero.
- **No mutation**: the runtime never allocates/moves/reclaims external state.
- **Causal honesty**: diagnostics expose evidence; correlations are labeled correlations; leak-suspect findings are heuristic.

## Lock ordering

The Observatory has two independent locks: `mutex_` (history/registry/counters) and `sub_mutex_` (subscribers). Subscribers are never invoked while holding `mutex_`; the subscriber lock is held only to copy the callback list. The sampler lock protects the provider list and is never held while calling `observatory.observe()` (which locks `mutex_`), avoiding lock-order inversion.
