// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/error.h"

namespace memory_observatory {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone: return "none";
    case ErrorCode::kInvalidArgument: return "invalid_argument";
    case ErrorCode::kOutOfRange: return "out_of_range";
    case ErrorCode::kBoundExceeded: return "bound_exceeded";
    case ErrorCode::kInvalidState: return "invalid_state";
    case ErrorCode::kNotInitialized: return "not_initialized";
    case ErrorCode::kProviderUnavailable: return "provider_unavailable";
    case ErrorCode::kBackendUnavailable: return "backend_unavailable";
    case ErrorCode::kProviderError: return "provider_error";
    case ErrorCode::kNotImplemented: return "not_implemented";
    case ErrorCode::kMalformedInput: return "malformed_input";
    case ErrorCode::kTruncatedInput: return "truncated_input";
    case ErrorCode::kCorruption: return "corruption";
    case ErrorCode::kInvalidEnum: return "invalid_enum";
    case ErrorCode::kInvalidTimestamp: return "invalid_timestamp";
    case ErrorCode::kNegativeCapacity: return "negative_capacity";
    case ErrorCode::kImpossibleAccounting: return "impossible_accounting";
    case ErrorCode::kOverflow: return "overflow";
    case ErrorCode::kAllocationFailure: return "allocation_failure";
    case ErrorCode::kUnsupportedVersion: return "unsupported_version";
    case ErrorCode::kSourceNotFound: return "source_not_found";
    case ErrorCode::kDuplicateSource: return "duplicate_source";
    case ErrorCode::kQueueFull: return "queue_full";
    case ErrorCode::kQueueOverflow: return "queue_overflow";
    case ErrorCode::kNoBackend: return "no_backend";
    case ErrorCode::kUnknownField: return "unknown_field";
    case ErrorCode::kIoError: return "io_error";
    case ErrorCode::kIncompatibleVersion: return "incompatible_version";
    case ErrorCode::kSessionError: return "session_error";
    case ErrorCode::kInvalidTrace: return "invalid_trace";
    case ErrorCode::kShutdown: return "shutdown";
    case ErrorCode::kTimeout: return "timeout";
    case ErrorCode::kRejected: return "rejected";
    case ErrorCode::kInternal: return "internal";
  }
  return "unknown";
}

}  // namespace memory_observatory
