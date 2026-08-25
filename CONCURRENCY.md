# Concurrency

The Observatory supports concurrent producers and readers. Ingestion is serialized under a single lock protecting history, registries, counters, and the dedup set. Subscriber callbacks are never invoked while an internal lock is held; they are collected under the subscriber lock and dispatched after all internal locks are released, making reentrancy safe. Unsubscribe from inside a callback is safe.

The `Sampler` worker uses a condition variable and an atomic stop flag; `stop()` joins without holding state the worker needs. `collect_once` releases the sampler lock before calling provider code and the Observatory, avoiding lock-order inversion.

The self-deadlock and lock-reentrancy audit checks: no read-then-write upgrade on the same lock, no callbacks under internal locks, no joining a worker while holding state it needs, consistent multi-lock ordering, and snapshot/report/replay paths that do not recursively acquire ingestion locks.
