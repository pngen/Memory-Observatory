// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/observatory.h"
#include "memory_observatory/version.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace memory_observatory {

double Confidence::normalize(double v) noexcept {
  if (!std::isfinite(v)) {
    return 0.0;
  }
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 1.0) {
    return 1.0;
  }
  return v;
}

namespace {

std::optional<double> metric_value(const Observation& obs) {
  const auto* p = std::get_if<MetricSample>(&obs.payload);
  if (!p) {
    return std::nullopt;
  }
  return std::visit(
      [](const auto& s) -> std::optional<double> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, GaugeSample>) {
          return (s.value && std::isfinite(*s.value))
                     ? std::optional<double>{*s.value}
                     : std::nullopt;
        } else if constexpr (std::is_same_v<T, CounterSample>) {
          return s.value ? std::optional<double>{static_cast<double>(*s.value)}
                         : std::nullopt;
        } else if constexpr (std::is_same_v<T, DistributionSample>) {
          return (s.summary.mean && std::isfinite(*s.summary.mean))
                     ? std::optional<double>{*s.summary.mean}
                     : std::nullopt;
        } else {
          return std::nullopt;
        }
      },
      *p);
}

MemoryKind metric_kind(const Observation& obs) {
  const auto* p = std::get_if<MetricSample>(&obs.payload);
  if (!p) {
    return MemoryKind::kUnknown;
  }
  return std::visit([](const auto& s) { return s.kind; }, *p);
}

bool is_metric(const Observation& obs) {
  return std::holds_alternative<MetricSample>(obs.payload);
}

bool is_event(const Observation& obs) {
  return std::holds_alternative<Event>(obs.payload);
}

struct SeriesPoint {
  MonotonicTimestamp t;
  double v;
};

std::vector<SeriesPoint> series_for(const std::vector<Observation>& span,
                                    DomainId domain, MemoryKind kind) {
  std::vector<SeriesPoint> out;
  for (const auto& obs : span) {
    if (obs.domain != domain) {
      continue;
    }
    if (metric_kind(obs) != kind || !is_metric(obs)) {
      continue;
    }
    auto v = metric_value(obs);
    if (v) {
      out.push_back({obs.timestamp.monotonic, *v});
    }
  }
  return out;
}

// The commit-like kinds a domain carries; used to find a growth series.
constexpr MemoryKind kGrowthKinds[] = {MemoryKind::kAllocated,
                                       MemoryKind::kLive,
                                       MemoryKind::kCommitted,
                                       MemoryKind::kPhysical};

std::string fmt_bytes(double b) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(0);
  ss << b;
  return ss.str();
}

std::string fmt_ratio(double r) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(2);
  ss << r;
  return ss.str();
}

Diagnostic make_diagnostic(DiagnosticCode code, DiagnosticSeverity sev,
                           DiagnosticKind kind, std::string summary,
                           Confidence confidence, std::string rule) {
  Diagnostic d;
  d.code = code;
  d.severity = sev;
  d.kind = kind;
  d.summary = std::move(summary);
  d.confidence = confidence;
  d.rule_used = std::move(rule);
  d.freshness = Freshness{};
  return d;
}

// When each source was last heard from, on this process's own steady clock. An
// observation's timestamp belongs to its producer: a replayed trace or a
// producer on another machine is in a different clock domain, so whether a
// source has gone quiet is measured from when its observations arrived here.
using ArrivalMap =
    std::unordered_map<SourceId, MonotonicTimestamp, IdHash<SourceTag>>;

// How long a source has been silent. Unknown (nullopt) for a source that was
// removed or has not produced anything yet.
std::optional<Duration> silence_of(const SourceInfo& si, const ArrivalMap& arrivals,
                                   MonotonicTimestamp now) {
  if (si.health == SourceHealth::kRemoved || !si.last_observation_known) {
    return std::nullopt;
  }
  const auto it = arrivals.find(si.id);
  if (it == arrivals.end()) {
    return std::nullopt;
  }
  return now - it->second;
}

// History is kept in arrival order and out-of-order arrival is accepted by
// default, while the diagnostic rules read each series in time order. Puts a
// span in monotonic order, keeping arrival order between equal timestamps.
void order_by_time(std::vector<Observation>& span) {
  std::stable_sort(span.begin(), span.end(),
                   [](const Observation& a, const Observation& b) {
                     return a.timestamp.monotonic < b.timestamp.monotonic;
                   });
}

}  // namespace

// ============================================================================
// Diagnostic engine (file-local). Operates on a bounded span + registry copies.
// ============================================================================

namespace {

std::vector<Diagnostic> run_diagnostics(
    const std::vector<Observation>& span,
    const std::unordered_map<SourceId, SourceInfo, IdHash<SourceTag>>& sources,
    const ArrivalMap& arrivals,
    const AccountingCounters& counters, const DiagnosticConfig& cfg,
    const std::unordered_map<DomainId, MemoryDomain, IdHash<DomainTag>>& domains,
    MonotonicTimestamp now) {
  std::vector<Diagnostic> out;

  // ---- Staleness, per source --------------------------------------------
  for (const auto& kv : sources) {
    const SourceInfo& si = kv.second;
    if (si.health == SourceHealth::kRemoved) {
      continue;  // a removed source is neither missing nor stale
    }
    if (!si.last_observation_known) {
      // A registered source that never produced anything.
      Diagnostic d = make_diagnostic(DiagnosticCode::kMissingProvider,
                                     DiagnosticSeverity::kWarning,
                                     DiagnosticKind::kInferred,
                                     "source registered but has produced no observations",
                                     Confidence{0.7}, "missing_provider");
      d.source = si.id;
      d.explanation =
          "Source '" + si.name + "' is registered but has not produced any "
          "observation within the observed window. This is a missing-provider "
          "situation, not a measured fact about memory.";
      out.push_back(std::move(d));
      continue;
    }
    const std::optional<Duration> silence = silence_of(si, arrivals, now);
    if (silence && *silence > cfg.stale_after) {
      const Duration age = *silence;
      Diagnostic d = make_diagnostic(DiagnosticCode::kStaleTelemetry,
                                     DiagnosticSeverity::kWarning,
                                     DiagnosticKind::kMeasuredFact,
                                     "telemetry from source is stale",
                                     Confidence{0.85}, "stale_after");
      d.source = si.id;
      DiagnosticEvidence ev;
      ev.description = "age since last observation exceeds configured stale_after";
      ev.source = si.id;
      ev.timestamp = si.last_observation;
      d.evidence.push_back(ev);
      d.explanation = "Source '" + si.name + "' last produced an observation " +
                      std::to_string(age.nanoseconds() / 1000000) +
                      " ms ago, which exceeds the configured stale_after. "
                      "This is a measured staleness fact.";
      out.push_back(std::move(d));
    }
  }

  // ---- Event loss / queue overflow --------------------------------------
  if (counters.dropped_overflow > 0) {
    Diagnostic d = make_diagnostic(DiagnosticCode::kQueueOverflow,
                                   DiagnosticSeverity::kCritical,
                                   DiagnosticKind::kMeasuredFact,
                                   "ingestion queue overflowed and dropped observations",
                                   Confidence{0.95}, "overflow_counter");
    DiagnosticEvidence ev;
    ev.description = "dropped_overflow == " + std::to_string(counters.dropped_overflow);
    ev.value = static_cast<double>(counters.dropped_overflow);
    d.evidence.push_back(ev);
    d.explanation = "The bounded ingestion queue overflowed and dropped " +
                    std::to_string(counters.dropped_overflow) +
                    " observations. This is a measured loss fact; the count is "
                    "carried in accounting counters.";
    out.push_back(std::move(d));
  }
  if (counters.rejected_malformed > 0 || counters.rejected_invalid > 0) {
    Diagnostic d = make_diagnostic(DiagnosticCode::kEventLoss,
                                   DiagnosticSeverity::kWarning,
                                   DiagnosticKind::kMeasuredFact,
                                   "some observations were rejected at ingestion",
                                   Confidence{0.9}, "rejection_counts");
    DiagnosticEvidence ev;
    ev.description = "rejected_malformed=" + std::to_string(counters.rejected_malformed) +
                     " rejected_invalid=" + std::to_string(counters.rejected_invalid);
    d.evidence.push_back(ev);
    d.explanation = "Malformed or invalid observations were rejected. Each is "
                    "counted precisely; nothing was silently discarded.";
    out.push_back(std::move(d));
  }

  // ---- Source reset / generation rollover -------------------------------
  for (const auto& kv : sources) {
    const SourceInfo& si = kv.second;
    if (si.generation_rollovers > 0) {
      Diagnostic d = make_diagnostic(DiagnosticCode::kSourceReset,
                                     DiagnosticSeverity::kInfo,
                                     DiagnosticKind::kMeasuredFact,
                                     "source generation has rolled over",
                                     Confidence{0.9}, "generation_rollover");
      d.source = si.id;
      d.explanation =
          "Source '" + si.name + "' advanced its generation " +
          std::to_string(si.generation_rollovers) +
          " time(s). Continuity of that source's metrics across the rollover "
          "should be treated with caution.";
      out.push_back(std::move(d));
    }
  }

  // ---- Per-domain capacity / growth rules -------------------------------
  // Collect distinct domains present in the span.
  std::vector<DomainId> domains_present;
  for (const auto& obs : span) {
    if (std::find(domains_present.begin(), domains_present.end(), obs.domain) ==
        domains_present.end()) {
      domains_present.push_back(obs.domain);
    }
  }

  for (const DomainId domain : domains_present) {
    // Near-capacity and impossible accounting from the most recent DomainState.
    // DomainState carries no timestamp in its payload; resolve by iterating
    // observations in order (the span is bounded and time-ordered) and taking
    // the last DomainState seen for this domain.
    const DomainState* ds_latest = nullptr;
    for (const auto& obs : span) {
      if (obs.domain == domain) {
        if (const auto* ds = std::get_if<DomainState>(&obs.payload)) {
          ds_latest = ds;
        }
      }
    }
    if (ds_latest) {
      const DomainCapacity& cap = ds_latest->capacity;
      if (!cap.accounting_is_possible()) {
        Diagnostic d = make_diagnostic(DiagnosticCode::kImpossibleAccounting,
                                       DiagnosticSeverity::kCritical,
                                       DiagnosticKind::kMeasuredFact,
                                       "domain capacity accounting is impossible",
                                       Confidence{0.95}, "accounting_invariants");
        d.domain = domain;
        d.explanation =
            "Reported capacity fields for this domain violate the conservative "
            "accounting invariants (e.g. governed > physical). The observation "
            "data is self-inconsistent or mislabeled.";
        out.push_back(std::move(d));
      }
      if (cap.available && cap.physical && *cap.physical > 0) {
        const double ratio =
            static_cast<double>(*cap.available) / static_cast<double>(*cap.physical);
        if (ratio < 1.0 - cfg.near_capacity_ratio) {
          // available fraction below threshold => near capacity.
          Diagnostic d = make_diagnostic(
              DiagnosticCode::kNearCapacity, DiagnosticSeverity::kWarning,
              DiagnosticKind::kDerivedStatistic, "domain near capacity",
              Confidence{0.8}, "near_capacity_ratio");
          d.domain = domain;
          DiagnosticEvidence ev;
          ev.description = "available/physical == " + fmt_ratio(ratio);
          ev.domain = domain;
          ev.kind = MemoryKind::kAvailable;
          ev.value = static_cast<double>(*cap.available);
          d.evidence.push_back(ev);
          d.explanation =
              "Available capacity is " + fmt_ratio(ratio) +
              " of physical capacity, below the configured near-capacity "
              "threshold, on a derived-statistic basis.";
          out.push_back(std::move(d));
        }
      }
    }

    // Growth / continuity rules on a commit-like kind series.
    for (MemoryKind kind : kGrowthKinds) {
      const auto series = series_for(span, domain, kind);
      if (series.size() < 2) {
        continue;
      }
      const std::size_t n = series.size();
      const double first = series.front().v;
      const double last = series.back().v;
      const double growth = last - first;
      const double frac = std::fabs(first) > 1e-9 ? growth / std::fabs(first) : 0.0;

      // Sustained growth: monotonic non-decreasing over the window.
      bool monotonic_up = true;
      for (std::size_t i = 1; i < n; ++i) {
        if (series[i].v < series[i - 1].v - 1e-9) {
          monotonic_up = false;
          break;
        }
      }
      if (monotonic_up && growth > 0 &&
          frac >= cfg.sustained_growth_ratio * static_cast<double>(n)) {
        Diagnostic d = make_diagnostic(
            DiagnosticCode::kSustainedCapacityGrowth, DiagnosticSeverity::kWarning,
            DiagnosticKind::kDerivedStatistic,
            "sustained upward trend in committed/allocated capacity",
            Confidence{0.7}, "monotonic_growth");
        d.domain = domain;
        DiagnosticEvidence ev;
        ev.description = std::string(to_string(kind)) + " grew by " +
                         fmt_bytes(growth) + " over " + std::to_string(n) +
                         " samples";
        ev.domain = domain;
        ev.kind = kind;
        ev.value = last;
        ev.delta = growth;
        d.evidence.push_back(ev);
        d.explanation =
            "The '" + std::string(to_string(kind)) +
            "' series moved monotonically upward across " + std::to_string(n) +
            " samples, a derived-statistic trend. This is not a leak "
            "conclusion and does not imply a cause.";
        out.push_back(std::move(d));
      }

      // Rapid growth: a large jump across the most recent rapid_growth_window
      // samples. Growth across the whole retained span is the sustained rule.
      const std::size_t rapid_window =
          std::max<std::size_t>(cfg.rapid_growth_window, 2);
      double rapid_growth = 0.0;
      double rapid_frac = 0.0;
      if (n >= rapid_window) {
        const double from = series[n - rapid_window].v;
        rapid_growth = last - from;
        rapid_frac = std::fabs(from) > 1e-9 ? rapid_growth / std::fabs(from) : 0.0;
      }
      if (n >= rapid_window && rapid_frac >= cfg.rapid_growth_ratio) {
        Diagnostic d = make_diagnostic(DiagnosticCode::kRapidGrowth,
                                       DiagnosticSeverity::kCritical,
                                       DiagnosticKind::kDerivedStatistic,
                                       "rapid growth in committed capacity",
                                       Confidence{0.8}, "rapid_growth_ratio");
        d.domain = domain;
        DiagnosticEvidence ev;
        ev.description = std::string(to_string(kind)) + " grew " +
                         fmt_ratio(rapid_frac) + " fractionally";
        ev.domain = domain;
        ev.kind = kind;
        ev.delta = rapid_growth;
        d.evidence.push_back(ev);
        d.explanation =
            "The '" + std::string(to_string(kind)) +
            "' series increased by " + fmt_ratio(rapid_frac) +
            " fractionally within a short window, exceeding the rapid-growth "
            "threshold. This is a derived statistic, not a causal claim.";
        out.push_back(std::move(d));
      }

      // Oscillation: values cross a threshold multiple times.
      if (series.size() >= 4) {
        const double mid = (series.front().v + series.back().v) / 2.0;
        int transitions = 0;
        bool above = series.front().v > mid;
        for (std::size_t i = 1; i < n; ++i) {
          const bool now_above = series[i].v > mid;
          if (now_above != above) {
            ++transitions;
            above = now_above;
          }
        }
        if (static_cast<std::size_t>(transitions) >= cfg.oscillation_min_transitions) {
          Diagnostic d = make_diagnostic(
              DiagnosticCode::kOscillationFlapping, DiagnosticSeverity::kWarning,
              DiagnosticKind::kCorrelation, "capacity value oscillates",
              Confidence{0.6}, "oscillation_transitions");
          d.domain = domain;
          DiagnosticEvidence ev;
          ev.description = std::to_string(transitions) + " threshold crossings";
          ev.domain = domain;
          ev.kind = kind;
          d.evidence.push_back(ev);
          d.explanation =
              "The '" + std::string(to_string(kind)) +
              "' series crossed a central value " + std::to_string(transitions) +
              " times, suggesting flapping. This is a correlation/pattern, not a "
              "causal explanation.";
          out.push_back(std::move(d));
        }
      }

      // Possible leak pattern: monotonic non-decreasing growth over a long
      // window with no observed release. Labeled heuristic/suspect.
      if (monotonic_up && growth > 0 && n >= 8) {
        Diagnostic d = make_diagnostic(
            DiagnosticCode::kPossibleLeakPattern, DiagnosticSeverity::kWarning,
            DiagnosticKind::kHeuristicInterpretation,
            "possible leak pattern (heuristic/suspect)",
            Confidence{0.4}, "monotonic_growth_heuristic");
        d.domain = domain;
        DiagnosticEvidence ev;
        ev.description =
            std::string(to_string(kind)) + " grew monotonically over " +
            std::to_string(n) + " samples with no observed release";
        ev.domain = domain;
        ev.kind = kind;
        ev.value = last;
        ev.delta = growth;
        d.evidence.push_back(ev);
        d.explanation =
            "The '" + std::string(to_string(kind)) +
            "' series grew monotonically across a long window with no observed "
            "release. THIS IS A HEURISTIC, SUSPECT INTERPRETATION. It does not "
            "prove a leak; it only flags a pattern. Only direct ownership "
            "evidence could establish a leak.";
        out.push_back(std::move(d));
      }
    }

    // Persistent pressure / recovery after pressure.
    std::vector<double> pressure_levels;
    for (const auto& obs : span) {
      if (obs.domain == domain) {
        if (const auto* ps = std::get_if<PressureState>(&obs.payload)) {
          if (ps->level) {
            pressure_levels.push_back(*ps->level);
          }
        }
      }
    }
    if (!pressure_levels.empty()) {
      const double threshold = 0.7;
      bool all_persistent = true;
      bool any_recovered = false;
      for (std::size_t i = 0; i < pressure_levels.size(); ++i) {
        if (pressure_levels[i] < threshold) {
          all_persistent = false;
          if (i > 0 && pressure_levels[i - 1] >= threshold) {
            any_recovered = true;
          }
        }
      }
      if (all_persistent && pressure_levels.size() >= 5) {
        Diagnostic d = make_diagnostic(DiagnosticCode::kPersistentPressure,
                                       DiagnosticSeverity::kCritical,
                                       DiagnosticKind::kDerivedStatistic,
                                       "persistent memory pressure",
                                       Confidence{0.8}, "persistent_pressure");
        d.domain = domain;
        d.explanation =
            "Pressure stayed above the threshold across " +
            std::to_string(pressure_levels.size()) + " samples. Derived statistic.";
        out.push_back(std::move(d));
      }
      if (any_recovered) {
        Diagnostic d = make_diagnostic(DiagnosticCode::kRecoveryAfterPressure,
                                       DiagnosticSeverity::kInfo,
                                       DiagnosticKind::kMeasuredFact,
                                       "pressure recovered after an elevated period",
                                       Confidence{0.7}, "recovery_after_pressure");
        d.domain = domain;
        d.explanation =
            "Pressure crossed back below the threshold after an elevated period. "
            "This is a measured transition, not a cause.";
        out.push_back(std::move(d));
      }
    }
  }

  // ---- Divergent sources for a shared domain ----------------------------
  std::map<std::pair<DomainId, SourceId>, double> last_by_domain_source;
  for (const auto& obs : span) {
    if (!is_metric(obs)) {
      continue;
    }
    if (metric_kind(obs) != MemoryKind::kCommitted) {
      continue;
    }
    auto v = metric_value(obs);
    if (v) {
      last_by_domain_source[{obs.domain, obs.source}] = *v;
    }
  }
  std::map<std::pair<DomainId, SourceId>, double> committed_by_source;
  for (const auto& obs : span) {
    if (!is_metric(obs) || metric_kind(obs) != MemoryKind::kCommitted) {
      continue;
    }
    auto v = metric_value(obs);
    if (!v) {
      continue;
    }
    committed_by_source[{obs.domain, obs.source}] = *v;
  }
  std::map<DomainId, std::vector<std::pair<SourceId, double>>> per_domain;
  for (const auto& kv : committed_by_source) {
    per_domain[kv.first.first].push_back({kv.first.second, kv.second});
  }
  for (const auto& kv : per_domain) {
    if (kv.second.size() < 2) {
      continue;
    }
    double minv = kv.second.front().second;
    double maxv = minv;
    for (const auto& p : kv.second) {
      minv = std::min(minv, p.second);
      maxv = std::max(maxv, p.second);
    }
    if (minv > 1e-9 && (maxv - minv) / minv > 0.35) {
      Diagnostic d = make_diagnostic(
          DiagnosticCode::kDivergentSourceObservations, DiagnosticSeverity::kWarning,
          DiagnosticKind::kCorrelation,
          "sources report divergent committed values for a shared domain",
          Confidence{0.6}, "source_divergence");
      d.domain = kv.first;
      d.explanation =
          "Two or more sources report widely divergent committed-byte values for "
          "the same domain, suggesting a mapping or unit mismatch. This is a "
          "correlation of observed divergence, not a causal finding.";
      out.push_back(std::move(d));
    }
  }

  (void)domains;
  return out;
}

std::vector<Correlation> run_correlations(
    const std::vector<Observation>& span,
    const std::vector<Diagnostic>& diagnostics) {
  std::vector<Correlation> out;
  // Estimate allocation-rate vs free-capacity correlation from the span.
  std::map<DomainId, std::vector<double>> alloc_series;
  std::map<DomainId, std::vector<double>> free_series;
  for (const auto& obs : span) {
    if (!is_metric(obs)) {
      continue;
    }
    auto v = metric_value(obs);
    if (!v) {
      continue;
    }
    if (metric_kind(obs) == MemoryKind::kAllocated) {
      alloc_series[obs.domain].push_back(*v);
    } else if (metric_kind(obs) == MemoryKind::kAvailable) {
      free_series[obs.domain].push_back(*v);
    }
  }
  for (const auto& kv : alloc_series) {
    if (kv.second.size() < 3) {
      continue;
    }
    const auto& alloc = kv.second;
    const auto fc = free_series.find(kv.first);
    if (fc == free_series.end() || fc->second.size() < 3) {
      continue;
    }
    const auto& free = fc->second;
    const std::size_t n = std::min(alloc.size(), free.size());
    double sum_xy = 0, sum_x = 0, sum_y = 0, sum_x2 = 0, sum_y2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
      sum_x += alloc[i];
      sum_y += free[i];
      sum_xy += alloc[i] * free[i];
      sum_x2 += alloc[i] * alloc[i];
      sum_y2 += free[i] * free[i];
    }
    const double denom = std::sqrt((n * sum_x2 - sum_x * sum_x) *
                                   (n * sum_y2 - sum_y * sum_y));
    double corr = 0.0;
    if (denom > 1e-9) {
      corr = (n * sum_xy - sum_x * sum_y) / denom;
    }
    if (corr < -0.6) {
      Correlation c;
      c.kind = CorrelationKind::kAllocationPressure;
      c.summary =
          "rising allocation activity is negatively correlated with free capacity";
      c.explanation =
          "Across the observed span, allocated bytes and available capacity moved "
          "in opposite directions (correlation " + fmt_ratio(corr) +
          "). This is an evidence-based CORRELATION; it is not a claim of "
          "causation.";
      c.confidence = Confidence{std::min(0.9, std::fabs(corr))};
      c.limitations.push_back("Correlation, not causation.");
      c.limitations.push_back("Uses a bounded span; results may not generalize.");
      out.push_back(std::move(c));
    }
  }
  (void)diagnostics;
  return out;
}

}  // namespace

// ============================================================================
// Observatory::Impl
// ============================================================================

struct Observatory::Impl {
  explicit Impl(const ObservatoryConfig& cfg) : config(cfg) {}

  ObservatoryConfig config;
  SessionId session;
  Epoch epoch;

  std::uint64_t next_observation_id = 1;
  std::uint64_t next_source_id = 1;
  std::uint64_t next_domain_id = 1;
  std::uint64_t next_snapshot_id = 1;
  std::uint64_t next_trace_id = 1;
  std::uint64_t next_subscription_id = 1;
  std::uint64_t next_session_id = 1;

  mutable std::mutex mutex_;
  std::deque<Observation> history_;
  // Parallel to history_: whether the producer supplied that observation's id.
  std::deque<bool> history_supplied_id_;

  // Ids that producers supplied, tracked for duplicate suppression.
  std::unordered_set<ObservationId, IdHash<ObservationTag>> dedup_set_;
  std::deque<ObservationId> dedup_order_;

  std::unordered_map<SourceId, SourceInfo, IdHash<SourceTag>> sources_;
  ArrivalMap arrivals_;
  std::unordered_map<DomainId, MemoryDomain, IdHash<DomainTag>> domains_;

  AccountingCounters counters_;

  mutable std::mutex sub_mutex_;
  std::unordered_map<SubscriptionId, SubscriberFn, IdHash<SubscriptionTag>>
      subscribers_;

  // Mints the next observation id, passing over any id a producer supplied that
  // is still tracked, so a minted id never reads as a duplicate of it.
  std::uint64_t mint_observation_id() {
    while (dedup_set_.count(ObservationId{next_observation_id}) != 0) {
      ++next_observation_id;
    }
    return next_observation_id++;
  }
};

// ============================================================================
// Observatory lifecycle
// ============================================================================

Observatory::Observatory(const ObservatoryConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
  impl_->session = SessionId{impl_->next_session_id++};
}

Observatory::~Observatory() = default;

const ObservatoryConfig& Observatory::config() const noexcept {
  return impl_->config;
}

SessionId Observatory::session_id() const noexcept { return impl_->session; }

Epoch Observatory::epoch() const noexcept { return impl_->epoch; }

// ============================================================================
// Source registry
// ============================================================================

Result<SourceId> Observatory::register_source(const SourceDescriptor& desc) {
  if (desc.name.empty()) {
    return fail<SourceId>(ErrorCode::kInvalidArgument,
                          "source name must not be empty");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (impl_->sources_.size() >= impl_->config.max_sources) {
    return fail<SourceId>(ErrorCode::kBoundExceeded, "source limit reached");
  }
  // Ids are also taken by sources registered on first sight of an observation;
  // one that is already in use is never handed out again.
  while (impl_->sources_.count(SourceId{impl_->next_source_id}) != 0) {
    ++impl_->next_source_id;
  }
  const SourceId id{impl_->next_source_id++};
  SourceInfo info;
  info.id = id;
  info.name = desc.name;
  info.kind = desc.kind;
  info.health = SourceHealth::kUnknown;
  impl_->sources_.emplace(id, std::move(info));
  return ok(id);
}

Error Observatory::remove_source(SourceId id) {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto it = impl_->sources_.find(id);
  if (it == impl_->sources_.end()) {
    return make_error(ErrorCode::kSourceNotFound, "source not found");
  }
  it->second.health = SourceHealth::kRemoved;
  return Error{};
}

Result<SourceInfo> Observatory::source_info(SourceId id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto it = impl_->sources_.find(id);
  if (it == impl_->sources_.end()) {
    return fail<SourceInfo>(ErrorCode::kSourceNotFound, "source not found");
  }
  return ok(it->second);
}

std::vector<SourceInfo> Observatory::sources() const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  std::vector<SourceInfo> out;
  out.reserve(impl_->sources_.size());
  for (const auto& kv : impl_->sources_) {
    out.push_back(kv.second);
  }
  return out;
}

Error Observatory::advance_generation(SourceId id) {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto it = impl_->sources_.find(id);
  if (it == impl_->sources_.end()) {
    return make_error(ErrorCode::kSourceNotFound, "source not found");
  }
  ++it->second.generation;
  ++it->second.generation_rollovers;
  it->second.last_generation_change = MonotonicTimestamp::now();
  return Error{};
}

Error Observatory::set_source_health(SourceId id, SourceHealth health) {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto it = impl_->sources_.find(id);
  if (it == impl_->sources_.end()) {
    return make_error(ErrorCode::kSourceNotFound, "source not found");
  }
  it->second.health = health;
  return Error{};
}

// ============================================================================
// Domain registry
// ============================================================================

Result<DomainId> Observatory::register_domain(const DomainSpec& spec) {
  if (spec.name.empty()) {
    return fail<DomainId>(ErrorCode::kInvalidArgument,
                          "domain name must not be empty");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (impl_->domains_.size() >= impl_->config.max_domains) {
    return fail<DomainId>(ErrorCode::kBoundExceeded, "domain limit reached");
  }
  const DomainId id{impl_->next_domain_id++};
  MemoryDomain d;
  d.id = id;
  d.name = spec.name;
  d.kind = spec.kind;
  impl_->domains_.emplace(id, std::move(d));
  return ok(id);
}

std::vector<MemoryDomain> Observatory::domains() const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  std::vector<MemoryDomain> out;
  out.reserve(impl_->domains_.size());
  for (const auto& kv : impl_->domains_) {
    out.push_back(kv.second);
  }
  return out;
}

// ============================================================================
// Ingestion
// ============================================================================

IngestResult Observatory::observe(Observation observation) {
  IngestResult result;
  result.id = observation.id;

  std::vector<SubscriberFn> fns;
  Observation dispatch_obs;

  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    AccountingCounters& counters = impl_->counters_;

    // Every observation offered to the pipeline is counted under exactly one
    // disposition, including the ones refused before anything else is looked at.
    const auto refuse = [&](IngestOutcome outcome, std::uint64_t& reason_counter,
                            const char* reason) {
      result.outcome = outcome;
      result.reason = reason;
      ++reason_counter;
      ++counters.ingested_total;
    };

    if (!observation.source.valid()) {
      refuse(IngestOutcome::kRejectedInvalid, counters.rejected_invalid,
             "observation has no source");
      return result;
    }
    if (observation.payload.index() == 0) {
      refuse(IngestOutcome::kRejectedMalformed, counters.rejected_malformed,
             "observation has no payload");
      return result;
    }

    auto src_it = impl_->sources_.find(observation.source);
    const bool known_source = src_it != impl_->sources_.end();
    if (!known_source && impl_->sources_.size() >= impl_->config.max_sources) {
      refuse(IngestOutcome::kRejectedInvalid, counters.rejected_invalid,
             "unknown source and source limit reached");
      return result;
    }
    if (known_source && src_it->second.health == SourceHealth::kRemoved) {
      refuse(IngestOutcome::kRejectedInvalid, counters.rejected_invalid,
             "source has been removed");
      return result;
    }

    // Duplicate suppression compares the ids that producers supplied. An id
    // minted here is new by construction and is never a duplicate of anything.
    const bool supplied_id = observation.id.valid();
    if (supplied_id && impl_->config.deduplicate_ids &&
        impl_->dedup_set_.count(observation.id) != 0) {
      refuse(IngestOutcome::kDuplicateSuppressed, counters.duplicate_suppressed,
             "duplicate observation id");
      return result;
    }

    // An observation naming an older generation than its source has reached
    // comes from before the source's last reset.
    const bool superseded =
        known_source && observation.generation != Generation{} &&
        observation.generation < src_it->second.generation;
    if (impl_->config.reject_stale &&
        (observation.freshness.stale || superseded)) {
      refuse(IngestOutcome::kRejectedStale, counters.rejected_stale,
             superseded ? "observation from a superseded source generation"
                        : "stale observation rejected by configuration");
      return result;
    }

    if (impl_->config.reject_out_of_order && known_source &&
        src_it->second.last_observation_known &&
        observation.timestamp.monotonic < src_it->second.last_observation) {
      refuse(IngestOutcome::kRejectedOutOfOrder, counters.rejected_out_of_order,
             "observation older than source's last accepted monotonic time");
      return result;
    }

    // Accept. Nothing above changed any state, so a refused observation leaves
    // the registry, its source and the id sequence exactly as they were.
    if (!supplied_id) {
      observation.id = ObservationId{impl_->mint_observation_id()};
      result.id = observation.id;
    }

    if (!known_source) {
      SourceInfo info;
      info.id = observation.source;
      // Honor an explicit provenance source name so cross-process source identity
      // is preserved when a producer's observations are ingested by a fresh
      // Observatory that has never seen this source.
      info.name = !observation.provenance.source_name.empty()
                      ? observation.provenance.source_name
                      : "ephemeral-" + std::to_string(observation.source.value());
      info.kind = "ephemeral";
      info.health = SourceHealth::kUnknown;
      src_it = impl_->sources_.emplace(observation.source, std::move(info)).first;
    }
    SourceInfo& source = src_it->second;

    // A source's generation only moves forward.
    if (observation.generation > source.generation) {
      if (source.generation != Generation{} && source.last_observation_known) {
        ++source.generation_rollovers;
      }
      source.generation = observation.generation;
    }

    if (supplied_id && impl_->config.deduplicate_ids) {
      impl_->dedup_set_.insert(observation.id);
      impl_->dedup_order_.push_back(observation.id);
      const std::size_t dedup_cap =
          std::max<std::size_t>(impl_->config.dedup_capacity, 1);
      while (impl_->dedup_order_.size() > dedup_cap) {
        impl_->dedup_set_.erase(impl_->dedup_order_.front());
        impl_->dedup_order_.pop_front();
      }
    }

    const MonotonicTimestamp stamp = observation.timestamp.monotonic;
    const bool event = is_event(observation);

    dispatch_obs = observation;
    impl_->history_.push_back(std::move(observation));
    impl_->history_supplied_id_.push_back(supplied_id);
    ++counters.retained;

    // Bounded history: the oldest retained observations make room. A capacity
    // of zero retains nothing, so the observation just accepted is evicted.
    while (impl_->history_.size() > impl_->config.history_capacity) {
      const ObservationId evicted_id = impl_->history_.front().id;
      const bool evicted_supplied = impl_->history_supplied_id_.front();
      impl_->history_.pop_front();
      impl_->history_supplied_id_.pop_front();
      ++counters.evicted;
      --counters.retained;
      if (evicted_supplied && impl_->dedup_set_.erase(evicted_id) != 0) {
        auto oit = std::find(impl_->dedup_order_.begin(),
                             impl_->dedup_order_.end(), evicted_id);
        if (oit != impl_->dedup_order_.end()) {
          impl_->dedup_order_.erase(oit);
        }
      }
    }

    source.observations_ingested += 1;
    if (event) {
      source.events_ingested += 1;
      source.last_event = stamp;
    }
    source.last_observation = stamp;
    source.last_observation_known = true;
    if (!source.first_observation) {
      source.first_observation = stamp;
    }
    if (source.health == SourceHealth::kUnknown) {
      source.health = SourceHealth::kHealthy;
    }
    impl_->arrivals_[source.id] = MonotonicTimestamp::now();

    ++counters.ingested_total;

    // Copy subscriber functions under their own lock only.
    std::lock_guard<std::mutex> slock(impl_->sub_mutex_);
    fns.reserve(impl_->subscribers_.size());
    for (const auto& kv : impl_->subscribers_) {
      fns.push_back(kv.second);
    }

    result.outcome = IngestOutcome::kAccepted;
  }

  // Dispatch OUTSIDE the history lock and outside the subscriber lock. A
  // subscriber that throws does not take the ingestion result, or the other
  // subscribers' delivery, down with it.
  for (const auto& fn : fns) {
    try {
      fn(dispatch_obs);
    } catch (...) {
    }
  }

  return result;
}

std::vector<IngestResult> Observatory::observe_batch(std::vector<Observation> batch) {
  std::vector<IngestResult> out;
  out.reserve(batch.size());
  for (auto& obs : batch) {
    out.push_back(observe(std::move(obs)));
  }
  return out;
}

AccountingCounters Observatory::counters() const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->counters_;
}

std::size_t Observatory::retained_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->history_.size();
}

// ============================================================================
// Timeline query
// ============================================================================

Result<Timeline> Observatory::query_timeline(const TimelineFilter& filter) const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  Timeline tl;
  // Every retained observation belongs to this Observatory's session.
  if (filter.session && *filter.session != impl_->session) {
    return ok(std::move(tl));
  }
  const std::string trace_value =
      filter.trace ? std::to_string(filter.trace->value()) : std::string();

  std::vector<const Observation*> matched;
  for (const auto& obs : impl_->history_) {
    if (filter.domain && obs.domain != *filter.domain) continue;
    if (filter.object && obs.object != *filter.object) continue;
    if (filter.process && obs.process != *filter.process) continue;
    if (filter.device && obs.device != *filter.device) continue;
    if (filter.source && obs.source != *filter.source) continue;
    if (filter.trace) {
      // Trace provenance is carried as a "trace_id" annotation naming the trace.
      bool in_trace = false;
      for (const auto& a : obs.annotations) {
        if (a.key == "trace_id" && a.value == trace_value) {
          in_trace = true;
          break;
        }
      }
      if (!in_trace) continue;
    }
    if (filter.event_type) {
      const auto* e = std::get_if<Event>(&obs.payload);
      if (!e || e->type != *filter.event_type) continue;
    }
    if (filter.severity) {
      const auto* e = std::get_if<Event>(&obs.payload);
      if (!e || e->severity != *filter.severity) continue;
    }
    if (filter.memory_kind) {
      if (!is_metric(obs) || metric_kind(obs) != *filter.memory_kind) continue;
    }
    if (filter.window && !filter.window->contains(obs.timestamp.monotonic)) {
      continue;
    }
    matched.push_back(&obs);
  }

  // Chronological order first, then the cap: a capped result is the earliest
  // max_results matches rather than whichever ones happened to arrive first.
  std::stable_sort(matched.begin(), matched.end(),
                   [](const Observation* a, const Observation* b) {
                     return a->timestamp.monotonic < b->timestamp.monotonic;
                   });
  if (filter.max_results != 0 && matched.size() > filter.max_results) {
    matched.resize(filter.max_results);
  }

  tl.entries.reserve(matched.size());
  for (const Observation* obs : matched) {
    TimelineEntry entry;
    entry.observation = *obs;
    tl.entries.push_back(std::move(entry));
  }
  tl.matched_count = static_cast<std::uint64_t>(tl.entries.size());
  return ok(std::move(tl));
}

Result<WindowedResult> Observatory::aggregate_timeline(
    const TimelineFilter& filter, const SampleExtractor& extractor,
    const Window& window) const {
  if (!extractor) {
    return fail<WindowedResult>(ErrorCode::kInvalidArgument,
                                "no sample extractor provided");
  }
  auto tl_res = query_timeline(filter);
  if (!tl_res.ok()) {
    return Result<WindowedResult>{tl_res.error()};
  }
  WindowedResult out;
  BoundedAggregator agg;
  std::vector<double> samples;
  for (const auto& entry : tl_res.value().entries) {
    if (!window.contains(entry.observation.timestamp.monotonic)) {
      continue;
    }
    auto v = extractor(entry.observation);
    if (!v) {
      continue;
    }
    agg.add(*v);
    samples.push_back(*v);
    ++out.sample_count;
  }
  out.aggregate = agg.to_aggregate();
  out.percentiles = compute_percentiles(samples);
  return ok(std::move(out));
}

// ============================================================================
// Snapshot
// ============================================================================

Result<Snapshot> Observatory::snapshot(const SnapshotFilter& filter) const {
  Snapshot snap;
  std::vector<Observation> span;
  ArrivalMap arrivals;

  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    snap.id = SnapshotId{impl_->next_snapshot_id++};
    snap.captured_at = MonotonicTimestamp::now();
    snap.wall = WallTimestamp::now();
    snap.epoch = impl_->epoch;
    snap.session = impl_->session;
    snap.observation_count = static_cast<std::uint64_t>(impl_->history_.size());
    snap.counters = impl_->counters_;

    // Latest DomainState per domain: the newest by monotonic time, arrival
    // order deciding between equal timestamps. whole_system takes precedence
    // over the per-domain and per-source subsets.
    std::map<DomainId, std::pair<MonotonicTimestamp, DomainState>> latest_domains;
    for (const auto& obs : impl_->history_) {
      if (!filter.whole_system && filter.source && obs.source != *filter.source) {
        continue;
      }
      if (const auto* ds = std::get_if<DomainState>(&obs.payload)) {
        auto found = latest_domains.find(obs.domain);
        if (found == latest_domains.end()) {
          latest_domains.emplace(obs.domain,
                                 std::make_pair(obs.timestamp.monotonic, *ds));
        } else if (obs.timestamp.monotonic >= found->second.first) {
          found->second = std::make_pair(obs.timestamp.monotonic, *ds);
        }
      }
    }
    for (const auto& kv : latest_domains) {
      if (!filter.whole_system && filter.domain && kv.first != *filter.domain) {
        continue;
      }
      snap.domains.push_back(kv.second.second);
    }

    for (const auto& kv : impl_->sources_) {
      snap.sources.push_back(kv.second);
    }
    arrivals = impl_->arrivals_;

    // Copy a bounded span for diagnostics (the engine only needs the tail).
    const std::size_t cap = std::min<std::size_t>(impl_->history_.size(), 4096);
    if (cap > 0) {
      span.reserve(cap);
      auto it = impl_->history_.end();
      std::advance(it, -static_cast<std::ptrdiff_t>(cap));
      for (; it != impl_->history_.end(); ++it) {
        span.push_back(*it);
      }
    }
  }

  std::unordered_map<SourceId, SourceInfo, IdHash<SourceTag>> src_map;
  for (const auto& si : snap.sources) {
    src_map[si.id] = si;
  }
  std::unordered_map<DomainId, MemoryDomain, IdHash<DomainTag>> domain_map;
  const DiagnosticConfig& dcfg = impl_->config.diagnostics;
  const MonotonicTimestamp now = MonotonicTimestamp::now();
  if (dcfg.enabled) {
    order_by_time(span);
    snap.active_diagnostics = run_diagnostics(span, src_map, arrivals,
                                              snap.counters, dcfg, domain_map,
                                              now);
  }

  // Provenance/confidence/freshness summary from the active sources.
  for (const auto& si : snap.sources) {
    Provenance p;
    p.source = si.id;
    p.source_name = si.name;
    p.provider_version = si.kind;
    p.mechanism = ObservationMechanism::kLive;
    snap.provenance_summary.push_back(std::move(p));
  }
  snap.confidence_summary = Confidence{0.5};
  snap.freshness_summary = Freshness{};
  // A source counts as stale when it is marked so, or when it has been silent
  // for longer than the staleness rule allows (the rule the diagnostics use).
  for (const auto& si : snap.sources) {
    const std::optional<Duration> silence = silence_of(si, arrivals, now);
    const bool silent_too_long =
        dcfg.enabled && silence && *silence > dcfg.stale_after;
    if (si.health == SourceHealth::kStale || si.health == SourceHealth::kFailed ||
        silent_too_long) {
      ++snap.stale_sources;
    }
  }
  return ok(std::move(snap));
}

// ============================================================================
// Diagnostics / correlation / explain
// ============================================================================

std::vector<Diagnostic> Observatory::evaluate_diagnostics() const {
  if (!impl_->config.diagnostics.enabled) {
    return {};
  }
  std::vector<Observation> span;
  std::unordered_map<SourceId, SourceInfo, IdHash<SourceTag>> src_map;
  std::unordered_map<DomainId, MemoryDomain, IdHash<DomainTag>> domain_map;
  ArrivalMap arrivals;
  AccountingCounters cnt;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    const std::size_t cap = std::min<std::size_t>(impl_->history_.size(), 4096);
    if (cap > 0) {
      span.reserve(cap);
      auto it = impl_->history_.end();
      std::advance(it, -static_cast<std::ptrdiff_t>(cap));
      for (; it != impl_->history_.end(); ++it) {
        span.push_back(*it);
      }
    }
    for (const auto& kv : impl_->sources_) {
      src_map[kv.first] = kv.second;
    }
    for (const auto& kv : impl_->domains_) {
      domain_map[kv.first] = kv.second;
    }
    arrivals = impl_->arrivals_;
    cnt = impl_->counters_;
  }
  order_by_time(span);
  return run_diagnostics(span, src_map, arrivals, cnt, impl_->config.diagnostics,
                         domain_map, MonotonicTimestamp::now());
}

std::vector<Correlation> Observatory::evaluate_correlations() const {
  if (!impl_->config.diagnostics.enabled ||
      !impl_->config.diagnostics.correlation_enabled) {
    return {};
  }
  std::vector<Observation> span;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    const std::size_t cap = std::min<std::size_t>(impl_->history_.size(), 4096);
    if (cap > 0) {
      span.reserve(cap);
      auto it = impl_->history_.end();
      std::advance(it, -static_cast<std::ptrdiff_t>(cap));
      for (; it != impl_->history_.end(); ++it) {
        span.push_back(*it);
      }
    }
  }
  order_by_time(span);
  auto diagnostics = evaluate_diagnostics();
  return run_correlations(span, diagnostics);
}

ExplainResult Observatory::explain(DiagnosticCode code) const {
  auto diagnostics = evaluate_diagnostics();
  for (const auto& d : diagnostics) {
    if (d.code == code) {
      ExplainResult ex;
      ex.diagnostic = d;
      ex.narrative = d.explanation;
      ex.evidence = d.evidence;
      ex.assumptions = d.assumptions;
      ex.limitations = d.limitations;
      ex.conclusion_kind = ConfidenceKind::kInferred;
      if (d.kind == DiagnosticKind::kMeasuredFact) {
        ex.conclusion_kind = ConfidenceKind::kMeasured;
      } else if (d.kind == DiagnosticKind::kDerivedStatistic) {
        ex.conclusion_kind = ConfidenceKind::kDerived;
      } else if (d.kind == DiagnosticKind::kCorrelation) {
        ex.conclusion_kind = ConfidenceKind::kCorrelated;
      } else if (d.kind == DiagnosticKind::kHeuristicInterpretation) {
        ex.conclusion_kind = ConfidenceKind::kHeuristic;
      } else if (d.kind == DiagnosticKind::kSynthetic) {
        ex.conclusion_kind = ConfidenceKind::kSynthetic;
      } else if (d.kind == DiagnosticKind::kReplayed) {
        ex.conclusion_kind = ConfidenceKind::kReplayed;
      }
      ex.rule_used = d.rule_used;
      return ex;
    }
  }
  ExplainResult ex;
  ex.narrative = "No diagnostic for the requested code is currently active. "
                 "This is not a measurement conclusion; it simply means the "
                 "evidence in the current bounded history did not trigger that "
                 "rule.";
  ex.conclusion_kind = ConfidenceKind::kInferred;
  return ex;
}

// ============================================================================
// Subscriptions (callbacks dispatched outside all internal locks)
// ============================================================================

SubscriptionId Observatory::subscribe(SubscriberFn fn) {
  if (!fn) {
    return SubscriptionId{};  // nothing to call: no subscription is created
  }
  std::lock_guard<std::mutex> lock(impl_->sub_mutex_);
  const SubscriptionId id{impl_->next_subscription_id++};
  impl_->subscribers_[id] = std::move(fn);
  return id;
}

Error Observatory::unsubscribe(SubscriptionId id) {
  std::lock_guard<std::mutex> lock(impl_->sub_mutex_);
  auto it = impl_->subscribers_.find(id);
  if (it == impl_->subscribers_.end()) {
    return make_error(ErrorCode::kInvalidArgument, "subscription not found");
  }
  impl_->subscribers_.erase(it);
  return Error{};
}

// ============================================================================
// Trace capture
// ============================================================================

Result<Trace> Observatory::capture_trace() const {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  Trace trace;
  trace.id = TraceId{impl_->next_trace_id++};
  trace.version = kTraceFormatVersion;
  trace.session = impl_->session;
  trace.capture_mechanism = "live";
  for (const auto& obs : impl_->history_) {
    TraceRecord record;
    record.observation = obs;
    trace.records.push_back(std::move(record));
  }
  return ok(std::move(trace));
}

// ============================================================================
// Report
// ============================================================================

Result<Report> Observatory::make_report() const {
  Report report;
  report.title = "Memory Observatory Report";
  report.version = version::kString;
  report.generated_at = MonotonicTimestamp::now();
  report.wall = WallTimestamp::now();

  std::vector<Observation> span;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    report.session = impl_->session;
    report.retained = static_cast<std::uint64_t>(impl_->history_.size());
    report.counters = impl_->counters_;
    for (const auto& kv : impl_->sources_) {
      report.sources.push_back(kv.second);
    }
    for (const auto& kv : impl_->domains_) {
      report.domains.push_back(kv.second);
    }
    const std::size_t cap = std::min<std::size_t>(impl_->history_.size(), 16384);
    if (cap > 0) {
      span.reserve(cap);
      auto it = impl_->history_.end();
      std::advance(it, -static_cast<std::ptrdiff_t>(cap));
      for (; it != impl_->history_.end(); ++it) {
        span.push_back(*it);
      }
    }
  }

  std::map<MemoryKind, std::vector<double>> by_kind;
  for (const auto& obs : span) {
    if (!is_metric(obs)) {
      continue;
    }
    const MemoryKind kind = metric_kind(obs);
    auto v = metric_value(obs);
    if (!v) {
      continue;
    }
    by_kind[kind].push_back(*v);
  }
  for (const auto& kv : by_kind) {
    MetricSummary ms;
    ms.kind = kv.first;
    ms.scale = Scale::kBytes;
    ms.sample_count = static_cast<std::uint64_t>(kv.second.size());
    BoundedAggregator agg;
    for (double v : kv.second) {
      agg.add(v);
    }
    ms.aggregate = agg.to_aggregate();
    ms.percentiles = compute_percentiles(kv.second);
    report.metric_summaries.push_back(std::move(ms));
  }

  report.diagnostics = evaluate_diagnostics();
  report.correlations = evaluate_correlations();

  TimelineFilter tf;
  tf.max_results = 20;
  auto tl = query_timeline(tf);
  if (tl.ok()) {
    report.timeline_excerpt = std::move(tl.value());
  }

  for (const auto& si : report.sources) {
    Provenance p;
    p.source = si.id;
    p.source_name = si.name;
    p.provider_version = si.kind;
    report.provenance.push_back(std::move(p));
  }

  report.limitations.push_back(
      "Snapshots are ingestion-consistent/best-effort, not globally atomic.");
  report.limitations.push_back(
      "Correlations are evidence-based correlations, never causations.");
  report.limitations.push_back(
      "Leak-suspect findings are heuristic interpretations, not proven leaks.");
  report.limitations.push_back(
      "Fragmentation is not inferred from generic allocated/free totals without "
      "provider-supplied fragmentation data.");

  return ok(std::move(report));
}

// ============================================================================
// Replay entry point
// ============================================================================

ReplayResult replay_trace(Observatory& observatory, const Trace& trace,
                          const ReplayOptions& options) {
  SourceDescriptor sd;
  sd.name = "replay";
  sd.kind = "replay";
  auto src_res = observatory.register_source(sd);
  // If registration fails, fall back to a fixed id; observe() auto-registers
  // unknown sources, so replay still proceeds deterministically.
  const SourceId source = src_res.ok() ? src_res.value() : SourceId{1000};

  ReplayResult result;
  result.trace = trace.id;
  result.seed = options.accelerated ? "accelerated" : "relative";
  std::size_t consumed = 0;
  for (const auto& record : trace.records) {
    if (options.record_limit != 0 && consumed >= options.record_limit) {
      break;
    }
    Observation obs = record.observation;
    obs.id = ObservationId{};  // let the observatory mint ids for replayed input.
    obs.source = source;
    // What is ingested here is a replay of a recording, not a live measurement.
    obs.provenance.mechanism = ObservationMechanism::kReplayed;
    if (options.include_wall_times) {
      obs.timestamp.wall = WallTimestamp::now();
    }
    IngestResult ir = observatory.observe(std::move(obs));
    if (ir.accepted()) {
      ++result.accepted;
    } else if (ir.outcome == IngestOutcome::kDuplicateSuppressed) {
      ++result.duplicates;
    } else {
      ++result.rejected;
    }
    ++result.consumed;
    ++consumed;
  }
  result.session = observatory.session_id();
  auto snap = observatory.snapshot();
  if (snap.ok()) {
    result.snapshot_after = snap.value().id;
    result.diagnostics = snap.value().active_diagnostics;
  }
  return result;
}

}  // namespace memory_observatory

