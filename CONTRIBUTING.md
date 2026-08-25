# Contributing

Contributions are welcome under the Apache-2.0 license (Copyright 2026 Summon Software Labs). Guidelines:

1. Keep the ownership boundary: Memory Observatory observes, it never mutates external runtime state.
2. Keep the public API exception-free at the boundary (use `Result<T>`/`Error`).
3. Keep C++20, `/W4 /WX` clean with zero warnings.
4. Keep all history/queues/snapshots bounded by explicit configuration.
5. Never fabricate a value; report unavailable/failed structurally.
6. Add tests for new diagnostics, providers, and serialization paths.
7. Run the full Release and Debug suites before submitting.
