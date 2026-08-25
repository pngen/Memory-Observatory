# Serialization

All serialized input is treated as untrusted. Deserializers bound counts, lengths, nesting, string bytes, and payload sizes, and reject: malformed input, truncated input, length overflow, absurd allocation requests, invalid enums, invalid timestamps, negative capacities, impossible sizes, integer overflow, and incompatible versions where no migration path exists.

The binary trace format has a magic value, an explicit endianness flag, a format version, and a CRC-32 checksum over the body. The JSON parser is strict (rejects trailing data, malformed numbers/strings/escapes). Fuzz/property tests hammer deserialization with random bytes.
