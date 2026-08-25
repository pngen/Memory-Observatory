// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Domain model. A domain is a named region of memory resources. Domain identity
// (DomainId) is stable and independent of any transient provider handle.
//
// Memory Observatory never fabricates values. Every capacity field is an
// optional: an absent (nullopt) value means "unknown/unavailable", and is NOT
// equivalent to zero. A missing metric stays unknown through the whole pipeline.

#ifndef MEMORY_OBSERVATORY_DOMAIN_HPP
#define MEMORY_OBSERVATORY_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

#include "memory_observatory/id.h"
#include "memory_observatory/types.h"

namespace memory_observatory {

// Kinds of memory domains Memory Observatory understands. User-defined domains
// are permitted and are represented with a stable id plus a name; the enum only
// classifies the kind.
enum class DomainKind : std::int32_t {
  kUnknown = 0,
  kHostRam,
  kProcessCommit,
  kSystemCommit,
  kPinnedHost,
  kAcceleratorDevice,
  kSharedMemory,
  kFileBackedMapped,
  kPersistentStorage,
  kUserDefined,
};

inline const char* to_string(DomainKind kind) noexcept {
  switch (kind) {
    case DomainKind::kUnknown: return "unknown";
    case DomainKind::kHostRam: return "host_ram";
    case DomainKind::kProcessCommit: return "process_commit";
    case DomainKind::kSystemCommit: return "system_commit";
    case DomainKind::kPinnedHost: return "pinned_host";
    case DomainKind::kAcceleratorDevice: return "accelerator_device";
    case DomainKind::kSharedMemory: return "shared_memory";
    case DomainKind::kFileBackedMapped: return "file_backed_mapped";
    case DomainKind::kPersistentStorage: return "persistent_storage";
    case DomainKind::kUserDefined: return "user_defined";
  }
  return "unknown";
}

inline DomainKind domain_kind_from_string(const std::string& s) noexcept {
  if (s == "host_ram") return DomainKind::kHostRam;
  if (s == "process_commit") return DomainKind::kProcessCommit;
  if (s == "system_commit") return DomainKind::kSystemCommit;
  if (s == "pinned_host") return DomainKind::kPinnedHost;
  if (s == "accelerator_device") return DomainKind::kAcceleratorDevice;
  if (s == "shared_memory") return DomainKind::kSharedMemory;
  if (s == "file_backed_mapped") return DomainKind::kFileBackedMapped;
  if (s == "persistent_storage") return DomainKind::kPersistentStorage;
  if (s == "user_defined") return DomainKind::kUserDefined;
  return DomainKind::kUnknown;
}

// A byte count. Always non-negative. Use std::uint64_t so negative counts are
// impossible at the type level; callers cannot construct a negative value.
using Bytes = std::uint64_t;

// The kinds of a memory quantity Memory Observatory can represent. A single
// domain snapshot may carry several of these; availability is expressed per
// field via optional.
enum class MemoryKind : std::int32_t {
  kUnknown = 0,
  kPhysical,
  kGoverned,    // capacity the governing entity exposes for this domain
  kCommitted,
  kReserved,
  kAllocated,
  kLive,
  kAvailable,
  kCached,
  kPinned,
  kMapped,
  kPressure,
  kTransfer,
  kReclaim,
  kResidency,
  kOther,
};

inline const char* to_string(MemoryKind kind) noexcept {
  switch (kind) {
    case MemoryKind::kUnknown: return "unknown";
    case MemoryKind::kPhysical: return "physical";
    case MemoryKind::kGoverned: return "governed";
    case MemoryKind::kCommitted: return "committed";
    case MemoryKind::kReserved: return "reserved";
    case MemoryKind::kAllocated: return "allocated";
    case MemoryKind::kLive: return "live";
    case MemoryKind::kAvailable: return "available";
    case MemoryKind::kCached: return "cached";
    case MemoryKind::kPinned: return "pinned";
    case MemoryKind::kMapped: return "mapped";
    case MemoryKind::kPressure: return "pressure";
    case MemoryKind::kTransfer: return "transfer";
    case MemoryKind::kReclaim: return "reclaim";
    case MemoryKind::kResidency: return "residency";
    case MemoryKind::kOther: return "other";
  }
  return "unknown";
}

// A single domain's observed capacity fields. Each is an optional; nullopt means
// the value is genuinely unknown/unavailable and care is taken to preserve that
// unknownness (a missing value never silently becomes zero).
struct DomainCapacity {
  std::optional<Bytes> physical;
  std::optional<Bytes> governed;
  std::optional<Bytes> committed;
  std::optional<Bytes> reserved;
  std::optional<Bytes> allocated;
  std::optional<Bytes> live;
  std::optional<Bytes> available;
  std::optional<Bytes> cached;
  std::optional<Bytes> pinned;
  std::optional<Bytes> mapped;

  [[nodiscard]] bool empty() const noexcept {
    return !physical && !governed && !committed && !reserved && !allocated &&
           !live && !available && !cached && !pinned && !mapped;
  }

  // Returns true only if the accounting is self-consistent. Conservative: it
  // never asserts relationships the model does not guarantee. It only rejects
  // values that are impossible under the documented capacity invariants.
  // - If both physical and governed are present, we require governed <= physical.
  // - available <= physical when both known.
  [[nodiscard]] bool accounting_is_possible() const noexcept {
    if (physical && governed && *governed > *physical) {
      return false;
    }
    if (physical && available && *available > *physical) {
      return false;
    }
    return true;
  }

  friend bool operator==(const DomainCapacity&, const DomainCapacity&) = default;
};

// A named region of memory resources.
struct MemoryDomain {
  DomainId id;
  std::string name;
  DomainKind kind = DomainKind::kUnknown;

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }

  friend bool operator==(const MemoryDomain&, const MemoryDomain&) = default;
};

// Convenience spec used when registering a domain.
struct DomainSpec {
  std::string name;
  DomainKind kind = DomainKind::kUserDefined;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_DOMAIN_HPP
