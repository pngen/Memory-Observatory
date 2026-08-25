# Snapshots

A snapshot is a logically immutable, coherent observational view at a defined point. Snapshot creation is thread-safe.

1.0 snapshot semantics are **ingestion-consistent / best-effort**: a snapshot represents a bounded, coherent view of the retained history at capture time, not a claim of exact global atomicity. This is documented on the snapshot (`Snapshot::semantics`).

A snapshot carries its `SnapshotId`, capture time, epoch, session, `SnapshotSemantics`, per-domain `DomainState` views, source info, the `AccountingCounters`, active diagnostics, a provenance/confidence/freshness summary, and the count of stale sources. Filters support whole-system, per-domain, and per-source subsets.
