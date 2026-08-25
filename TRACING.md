# Tracing

A trace is a versioned, deterministic record of observations. Two encodings are provided: a strict binary format (canonical, bounded, checksummed) and a human-readable JSON format. The format has a magic bytes value, an explicit little-endian flag, a format version, and deterministic serialization.

`serialize_trace_binary` / `deserialize_trace_binary` handle the binary format. `serialize_trace_json` / `deserialize_trace_json` handle JSON. All deserializers treat input as untrusted: they bound counts, lengths, and nesting, reject malformed/truncated/corrupt/oversized input, and validate the header (magic/version) before the checksum.
