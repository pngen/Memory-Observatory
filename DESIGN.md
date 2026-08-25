# Design Notes

Memory Observatory is designed to *observe without acting*. This document records the key decisions and their justifications.

## Two clocks

The runtime separates monotonic time (all ordering, duration, rate math) from wall time (human correlation). Duration is never computed from wall timestamps, so clock jumps cannot corrupt a rate. See `time.h`.

## Strong identifiers

Stable public identifiers are integer-backed strong handles, never raw pointers. Value zero is invalid.

## Unknown remains unknown

Every numeric payload field is optional. A missing metric is `nullopt`, not zero. No value is ever fabricated; a provider reports unavailable/failed rather than inventing a number.

## Ownership boundary as a type discipline

The provider interface returns `Sample`/state data only. There is no method that mutates external runtime state. Any recommendation-like output is a `Diagnostic`/`Correlation` marked as measured/derived/correlated/heuristic.

## Bounded everything

History, dedup, queues, snapshots, timelines, and trace record counts are all bounded by explicit configuration.

## Deterministic replay

A trace is a versioned record of observations. Serialization is deterministic (canonical field order, fixed endianness, checksum). Replay feeds a fresh Observatory and regenerates snapshots/diagnostics without sleeping for wall-clock durations.

## Error handling

`Result<T>` / `Error` value types. Exceptions never cross the public API boundary.

## Explicability

Every diagnostic carries a summary, severity, affected entities, evidence, relevant timestamps, metric deltas, confidence, provenance, freshness, the rule used, assumptions, limitations, and the epistemic kind.
