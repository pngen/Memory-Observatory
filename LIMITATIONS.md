# Limitations

Memory Observatory is honest about what it does not do:

- Snapshots are ingestion-consistent/best-effort, not globally atomic.
- Correlations are evidence-based correlations, never causations.
- Leak-suspect findings are heuristic interpretations, not proven leaks.
- Fragmentation is not inferred from generic allocated/free totals without provider-supplied fragmentation data.
- Global ordering is only guaranteed up to source-local ordering plus ingestion ordering; exact global ordering is not claimed.
- Zero event loss is only true under tested conditions (overflow is counted, and overflow/drop counters are exposed).
- CUDA observation: on this machine the driver API returns CUDA_ERROR_INVALID_CONTEXT (201) for cuMemGetInfo_v2 and cuMemAlloc in a fresh process because no context is current. The provider now retains and binds the device primary context before querying (which resolves the read), and a real bounded 256 MiB cuMemAlloc/cuMemFree proof is included. A documented driver difference is that cuMemGetInfo_v2 works in the primary context while cuMemAlloc requires a cuCtxCreate context on this driver, so the proof allocates in a fresh context and reads device-global free memory through the primary context. No value is ever fabricated.
- Platform support is validated on Windows 11 x64 / MSVC 19.44+. Other backends are labeled not validated.
- Report/JSON output encodes NaN/Infinity as null under the documented representation.
- The compare CLI compares trace inputs (binary/JSON) and produces measured/derived/heuristic findings; it never declares a universal winner, and it is deterministic for identical inputs.
- The multiprocess proof uses shared trace-file exchange between two real OS processes (sender/receiver) on Windows; it is Windows-only in this build.
