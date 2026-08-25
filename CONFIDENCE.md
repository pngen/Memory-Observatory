# Confidence and Freshness

`Confidence` is a value in [0,1]; an empty confidence is "unknown", not zero. Non-finite or out-of-range inputs are clamped. `ConfidenceKind` classifies epistemic grounding: measured, derived, correlated, inferred, heuristic, synthetic, or replayed.

`Freshness` records the age of an observation and whether it is considered stale under the relevant configuration. A source that stops producing is reported as stale/missing by the diagnostics engine.
