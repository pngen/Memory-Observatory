# Replay

Replay feeds a trace into a fresh Observatory, regenerates snapshots and diagnostics, and can compare output to recorded expectations. Replay is deterministic and accelerated by default (`ReplayOptions::accelerated = true`): it does not sleep for original wall-clock durations. Original-relative-timing replay is available but tests never rely on wall-clock sleeping.

`replay_trace` registers a replay source, ingests each record (allowing the Observatory to mint ids), and reports consumed/accepted/rejected/duplicate counts plus the resulting snapshot and diagnostics.
