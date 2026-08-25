// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Version constants for Memory Observatory.

#ifndef MEMORY_OBSERVATORY_VERSION_HPP
#define MEMORY_OBSERVATORY_VERSION_HPP

#define MEMORY_OBSERVATORY_VERSION_MAJOR 1
#define MEMORY_OBSERVATORY_VERSION_MINOR 0
#define MEMORY_OBSERVATORY_VERSION_PATCH 0

#define MEMORY_OBSERVATORY_VERSION_STR "1.0.0"

#define MEMORY_OBSERVATORY_VERSION \
  (MEMORY_OBSERVATORY_VERSION_MAJOR * 10000 + \
   MEMORY_OBSERVATORY_VERSION_MINOR * 100 + \
   MEMORY_OBSERVATORY_VERSION_PATCH)

namespace memory_observatory {
namespace version {

constexpr int kMajor = MEMORY_OBSERVATORY_VERSION_MAJOR;
constexpr int kMinor = MEMORY_OBSERVATORY_VERSION_MINOR;
constexpr int kPatch = MEMORY_OBSERVATORY_VERSION_PATCH;

// Canonical "major.minor.patch" string.
constexpr const char* kString = MEMORY_OBSERVATORY_VERSION_STR;

}  // namespace version
}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_VERSION_HPP
