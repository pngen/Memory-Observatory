# Benchmarks

The benchmark (`benchmarks/`) measures Memory Observatory itself: ingestion throughput, snapshot latency, timeline query latency, diagnostic evaluation, metadata, and trace serialize/deserialize. It uses a deterministic seeded workload and synchronizes asynchronous work before timing.

Interpretation is honest: results are single-machine measurements, not claims of universal scalability. Algorithmic scaling is called out where observable (for example, timeline materialization sorts the bounded result set).
