# Diagnostics

The diagnostic engine produces structured, fully explainable `Diagnostic`s. Each carries a `DiagnosticCode`, severity, epistemic `DiagnosticKind`, summary, affected domain/source/object, evidence (observations used), relevant timestamps, metric deltas, confidence, provenance, freshness, the rule used, assumptions, limitations, and a human-readable explanation.

Implemented rules include: source missing/stale, event loss / queue overflow, source reset / generation rollover, impossible accounting, near capacity, sustained capacity growth, rapid growth, oscillation/flapping, allocation churn, possible leak pattern (heuristic), divergent source observations, and persistent pressure / recovery after pressure.

**Leak-related findings are labeled heuristic/suspect** unless direct ownership evidence proves a leak. Fragmentation is only reported when the supplied provider data supports a meaningful fragmentation metric; it is never inferred from generic allocated/free totals.
