# Timelines

A timeline is a bounded, chronological projection of the history ordered by monotonic time. All timelines are bounded by the history capacity; there is no unbounded accumulation.

Queries can filter by domain, object, process, device, source, event type, event severity, memory kind, time window, and trace annotation. Results are returned in chronological (ascending monotonic) order and may be capped by `max_results`. Windowing, aggregation, rate/delta calculation, and percentile computation are provided through `aggregate_timeline` with an explicit `SampleExtractor`.
