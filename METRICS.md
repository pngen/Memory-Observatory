# Metrics

Memory Observatory models metrics with strong types from `aggregate.h`: `Aggregate` (min/max/mean/sum/count), `Percentiles` (p50/p90/p95/p99), `Histogram` (bounded buckets + counts), `Rate` (per-second over a `Duration`), `Delta` (with a `reset` flag), and `Window` (a bounded monotonic time span).

`BoundedAggregator` is a numerically safe online aggregator. It uses Kahan-style compensation and ignores non-finite (NaN/Inf) inputs, mirroring the "unknown stays unknown" rule. `compute_percentiles` sorts a bounded set and returns an empty `Percentiles` when there is no usable data. The aggregation engine detects overflow and never treats a reset counter as a gigantic negative delta.
