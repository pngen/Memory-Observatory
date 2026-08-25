# Testing

The test suite (`tests/`) is dependency-free and built into a single executable. It covers: IDs, config validation, errors, the domain model, observations, metrics, provenance, confidence/freshness, the source registry, provider lifecycle, periodic sampling, provider failure and recovery, ingestion, bounded queues/history, overflow accounting, snapshots, timeline filtering, aggregation, counter resets, rates, percentiles, diagnostics, correlation, anomaly rules, staleness, generation rollover, subscriptions and reentrant callback safety, unsubscribe inside callbacks, shutdown, serialization, corruption rejection, trace replay, deterministic replay, randomized/property tests, concurrency, and real hardware provider tests (host/process/commit, and a bounded CUDA probe guarded by availability).

Randomized/property tests are deterministic by seed (printed on failure) and verify accounting balance, history bounds, and no crashes. Fuzz tests feed arbitrary bytes to the deserializer to prove it never crashes and never silently accepts garbage.

The suite is built and run repeatedly in both Release and Debug under `/W4 /WX`. No test timeout is used.
