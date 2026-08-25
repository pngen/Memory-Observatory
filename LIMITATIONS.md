# Limitations

Memory Observatory is honest about what it does not do:

- Snapshots are ingestion-consistent/best-effort, not globally atomic.
- Correlations are evidence-based correlations, never causations.
- Leak-suspect findings are heuristic interpretations, not proven leaks.
- Fragmentation is not inferred from generic allocated/free totals without provider-supplied fragmentation data.
- Global ordering is only guaranteed up to source-local ordering plus ingestion ordering; exact global ordering is not claimed.
- Zero event loss is only true under tested conditions (overflow is counted, and overflow/drop counters are exposed).
- CUDA observation on this machine reports a structured provider error at runtime when the driver query is not usable; no accelerator value is fabricated.
- Platform support is validated on Windows 11 x64 / MSVC 19.44+. Other backends are labeled not validated.
- Report/JSON output encodes NaN/Infinity as `null` under the documented representation.
