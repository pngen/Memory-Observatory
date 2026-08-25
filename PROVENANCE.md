# Provenance

Provenance is first-class for every observation. Each observation carries a `Provenance` with: source id/name, provider version, optional machine id (privacy opt-in), process, runtime version, toolchain, OS, architecture, optional accelerator, driver version, observation mechanism (live/synthetic/replayed/injected/external), wall-clock-trust flag, confidence, capture session, freshness, and annotations.

Reports separate measured, supplied, derived, inferred, synthetic, and replayed outcomes. Machine identity is only recorded when explicitly supplied (privacy-safe).
