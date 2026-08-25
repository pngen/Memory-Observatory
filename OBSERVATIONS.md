# Observations

An `Observation` is the currency of the ingestion pipeline. It carries a stable `ObservationId`, `SourceId` + `Provenance`, an `ObservationTimestamp` (monotonic, optional wall, source-local sequence), `DomainId` + `DomainKind`, `Generation` and `Epoch`, optional object/process/device/worker context, a typed `ObservationPayload`, and annotations.

`ObservationPayload` is a variant of `std::monostate`, `MetricSample`, `Event`, and state payloads (`DomainState`, `ObjectState`, `ResidencyState`, `PressureState`, `TransferState`, `AllocationState`, `ReclaimState`, `CacheState`, `SystemState`). `MetricSample` is a variant of `GaugeSample`, `CounterSample`, and `DistributionSample`. A `CounterSample` carries an explicit `reset` flag so a counter wrap is never mistaken for a huge negative delta.

Unknown values remain unknown: every numeric payload field is optional and `nullopt` means genuinely unavailable, never zero.
