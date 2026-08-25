// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Explanation result. For a given diagnostic, an explanation exposes the
// summary, severity, affected entities, evidence, relevant timestamps, metric
// deltas, confidence, provenance, freshness, the rule used, assumptions,
// limitations, and the epistemic kind (measured/derived/correlated/heuristic).
// Explanations never fabricate causal certainty.

#ifndef MEMORY_OBSERVATORY_EXPLAIN_HPP
#define MEMORY_OBSERVATORY_EXPLAIN_HPP

#include "memory_observatory/diagnostic.h"

namespace memory_observatory {

// A full explanation of one diagnostic. Deliberately mirrors the diagnostic but
// adds a narrative, evidence list, and explicit assumptions/limitations.
struct ExplainResult {
  Diagnostic diagnostic;
  std::string narrative;
  std::vector<DiagnosticEvidence> evidence;
  std::vector<std::string> assumptions;
  std::vector<std::string> limitations;
  ConfidenceKind conclusion_kind = ConfidenceKind::kInferred;
  std::string rule_used;
};

}  // namespace memory_observatory

#endif  // MEMORY_OBSERVATORY_EXPLAIN_HPP
