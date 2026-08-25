# Domains

A domain is a named region of memory resources. Domain identity (`DomainId`) is stable and independent of any transient provider handle. `DomainKind` classifies the kind: host RAM, process commit, system commit, pinned host, accelerator/device, shared, file-backed/mapped, persistent storage, or user-defined.

`DomainCapacity` carries explicit, optional capacity fields: physical, governed, committed, reserved, allocated, live, available, cached, pinned, mapped. A missing field is unknown, never zero. `accounting_is_possible()` conservatively rejects impossible accounting (e.g., governed > physical when both known). Memory Observatory does not infer internal allocator fragmentation from generic allocated/free totals.
