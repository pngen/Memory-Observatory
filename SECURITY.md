# Security

Treat traces and serialized input as untrusted. Bounds are enforced on counts, lengths, nesting, strings, histories, queues, event payloads, file sizes where practical, and report sizes where configurable. Hostile integer values are rejected before allocation. Output naming is sanitized (no path traversal). Raw pointers and platform handles are never deserialized. Source-provided capacities are not trusted blindly where basic invariants can detect impossibility. Fuzz tests hammer the deserializer to prove no memory-safety defect on malformed input.
