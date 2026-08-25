// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/compare.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "memory_observatory/observatory.h"
#include "memory_observatory/report.h"

namespace memory_observatory {

namespace {

std::optional<double> metric_value(const Observation& obs) {
  const auto* p = std::get_if<MetricSample>(&obs.payload);
  if (!p) return std::nullopt;
  return std::visit(
      [](const auto& s) -> std::optional<double> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, GaugeSample>) {
          return (s.value && std::isfinite(*s.value)) ? std::optional<double>{*s.value} : std::nullopt;
        } else if constexpr (std::is_same_v<T, CounterSample>) {
          return s.value ? std::optional<double>{static_cast<double>(*s.value)} : std::nullopt;
        } else if constexpr (std::is_same_v<T, DistributionSample>) {
          return (s.summary.mean && std::isfinite(*s.summary.mean)) ? std::optional<double>{*s.summary.mean} : std::nullopt;
        } else {
          return std::nullopt;
        }
      },
      *p);
}

MemoryKind metric_kind(const Observation& obs) {
  const auto* p = std::get_if<MetricSample>(&obs.payload);
  if (!p) return MemoryKind::kUnknown;
  return std::visit([](const auto& s) { return s.kind; }, *p);
}

bool is_event(const Observation& obs) { return std::holds_alternative<Event>(obs.payload); }

struct Side {
  AccountingCounters counters;
  std::vector<SourceInfo> sources;
  std::vector<MemoryDomain> domains;
  std::vector<Observation> timeline;
  std::vector<Diagnostic> diagnostics;
  std::vector<Correlation> correlations;
};

Side build_side(const Trace& trace) {
  Side side;
  Observatory obs;
  for (const auto& rec : trace.records) {
    Observation o = rec.observation;
    (void)obs.observe(std::move(o));
  }
  side.counters = obs.counters();
  side.sources = obs.sources();
  auto tl = obs.query_timeline(TimelineFilter{});
  if (tl.ok()) {
    for (const auto& e : tl.value().entries) side.timeline.push_back(e.observation);
  }
  side.diagnostics = obs.evaluate_diagnostics();
  side.correlations = obs.evaluate_correlations();
  // Derive the observed domain inventory from the timeline (the Observatory only
  // tracks explicitly registered domains; observed-but-unregistered domains are
  // still part of the inventory).
  std::map<DomainId, DomainKind> dom_map;
  for (const auto& o : side.timeline) dom_map[o.domain] = o.domain_kind;
  for (const auto& kv : dom_map) {
    MemoryDomain d;
    d.id = kv.first;
    d.name = "domain-" + std::to_string(kv.first.value());
    d.kind = kv.second;
    side.domains.push_back(std::move(d));
  }
  return side;
}

// Per-kind metric statistics from a timeline.
std::map<MemoryKind, std::pair<BoundedAggregator, std::vector<double>>> summarize(
    const Side& side) {
  std::map<MemoryKind, std::pair<BoundedAggregator, std::vector<double>>> out;
  for (const auto& o : side.timeline) {
    if (!std::holds_alternative<MetricSample>(o.payload)) continue;
    auto v = metric_value(o);
    if (!v) continue;
    auto& entry = out[metric_kind(o)];
    entry.first.add(*v);
    entry.second.push_back(*v);
  }
  return out;
}

std::string jnum(double v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  return os.str();
}

std::string jstr(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

Result<ComparisonReport> compare_traces(const Trace& left, const Trace& right) {
  ComparisonReport report;
  const Side L = build_side(left);
  const Side R = build_side(right);

  // Counts (measured).
  report.left_observations = static_cast<std::uint64_t>(L.timeline.size());
  report.right_observations = static_cast<std::uint64_t>(R.timeline.size());
  for (const auto& o : L.timeline) if (is_event(o)) ++report.left_events;
  for (const auto& o : R.timeline) if (is_event(o)) ++report.right_events;
  report.left_dropped = L.counters.dropped_overflow;
  report.right_dropped = R.counters.dropped_overflow;
  report.left_rejected = L.counters.rejected_malformed + L.counters.rejected_invalid +
                          L.counters.rejected_stale + L.counters.rejected_out_of_order;
  report.right_rejected = R.counters.rejected_malformed + R.counters.rejected_invalid +
                          R.counters.rejected_stale + R.counters.rejected_out_of_order;
  report.left_source_healthy = 0;
  report.right_source_healthy = 0;
  for (const auto& s : L.sources) if (s.health == SourceHealth::kHealthy || s.health == SourceHealth::kUnknown) ++report.left_source_healthy;
  for (const auto& s : R.sources) if (s.health == SourceHealth::kHealthy || s.health == SourceHealth::kUnknown) ++report.right_source_healthy;

  // Metric diffs (derived).
  const auto ls = summarize(L);
  const auto rs = summarize(R);
  std::set<MemoryKind> allKinds;
  for (const auto& kv : ls) allKinds.insert(kv.first);
  for (const auto& kv : rs) allKinds.insert(kv.first);
  for (MemoryKind kind : allKinds) {
    MetricDiff md;
    md.kind = kind;
    const auto lit = ls.find(kind);
    const auto rit = rs.find(kind);
    if (lit != ls.end()) {
      const auto& [agg, samples] = lit->second;
      auto a = agg.to_aggregate();
      auto p = compute_percentiles(samples);
      md.left_count = static_cast<std::uint64_t>(samples.size());
      md.left_min = a.min; md.left_max = a.max; md.left_mean = a.mean;
      md.left_p50 = p.p50; md.left_p95 = p.p95; md.left_p99 = p.p99;
    }
    if (rit != rs.end()) {
      const auto& [agg, samples] = rit->second;
      auto a = agg.to_aggregate();
      auto p = compute_percentiles(samples);
      md.right_count = static_cast<std::uint64_t>(samples.size());
      md.right_min = a.min; md.right_max = a.max; md.right_mean = a.mean;
      md.right_p50 = p.p50; md.right_p95 = p.p95; md.right_p99 = p.p99;
    }
    if (md.left_mean && md.right_mean) {
      md.mean_delta = *md.right_mean - *md.left_mean;
    }
    report.metrics.push_back(std::move(md));
  }

  // Domain inventory diff (measured).
  std::map<DomainId, std::string> ld, rd;
  for (const auto& d : L.domains) ld[d.id] = d.name;
  for (const auto& d : R.domains) rd[d.id] = d.name;
  for (const auto& kv : ld) if (!rd.count(kv.first)) report.domains_only_left.push_back(kv.second);
  for (const auto& kv : rd) if (!ld.count(kv.first)) report.domains_only_right.push_back(kv.second);

  // Diagnostics added/removed/severity changed (derived).
  std::map<DiagnosticCode, DiagnosticSeverity> lse, rse;
  for (const auto& d : L.diagnostics) lse[d.code] = d.severity;
  for (const auto& d : R.diagnostics) rse[d.code] = d.severity;
  for (const auto& kv : lse) {
    if (!rse.count(kv.first)) report.diagnostics_only_left.push_back(to_string(kv.first));
    else if (rse[kv.first] != kv.second) report.diagnostics_severity_changed.push_back(to_string(kv.first));
  }
  for (const auto& kv : rse) if (!lse.count(kv.first)) report.diagnostics_only_right.push_back(to_string(kv.first));

  // Provenance differences (measured/heuristic).
  std::set<std::string> lprov, rprov;
  for (const auto& o : L.timeline) {
    std::string key = std::to_string(o.source.value()) + "/" + o.provenance.source_name + "/" +
                      to_string(o.provenance.mechanism) + "/g" + std::to_string(o.generation.value());
    lprov.insert(key);
  }
  for (const auto& o : R.timeline) {
    std::string key = std::to_string(o.source.value()) + "/" + o.provenance.source_name + "/" +
                      to_string(o.provenance.mechanism) + "/g" + std::to_string(o.generation.value());
    rprov.insert(key);
  }
  for (const auto& k : lprov) if (!rprov.count(k)) report.provenance_diff.push_back("left-only " + k);
  for (const auto& k : rprov) if (!lprov.count(k)) report.provenance_diff.push_back("right-only " + k);

  // Findings (labeled).
  if (report.left_observations != report.right_observations) {
    report.findings.push_back({CompareKind::kMeasured, "observation_count",
        "observations differ: " + std::to_string(report.left_observations) + " -> " +
        std::to_string(report.right_observations)});
  }
  if (report.left_dropped != report.right_dropped) {
    report.findings.push_back({CompareKind::kMeasured, "dropped",
        "dropped differs: " + std::to_string(report.left_dropped) + " -> " +
        std::to_string(report.right_dropped)});
  }
  for (const auto& md : report.metrics) {
    if (md.left_count > 0 && md.right_count == 0) {
      report.findings.push_back({CompareKind::kMeasured, to_string(md.kind),
          "metric present only in " + report.left_label});
    } else if (md.right_count > 0 && md.left_count == 0) {
      report.findings.push_back({CompareKind::kMeasured, to_string(md.kind),
          "metric present only in " + report.right_label});
    } else if (md.left_mean && md.right_mean && md.mean_delta &&
               std::fabs(*md.mean_delta) > 1e-9) {
      report.findings.push_back({CompareKind::kDerived, to_string(md.kind),
          "mean delta " + jnum(*md.mean_delta)});
    }
  }
  if (!report.domains_only_left.empty() || !report.domains_only_right.empty()) {
    report.findings.push_back({CompareKind::kMeasured, "domain_inventory",
        "domain sets differ"}); 
  }
  if (!report.diagnostics_only_left.empty() || !report.diagnostics_only_right.empty() ||
      !report.diagnostics_severity_changed.empty()) {
    report.findings.push_back({CompareKind::kHeuristic, "diagnostics",
        "diagnostic sets differ (compare for explanation, not causation)"});
  }
  if (!report.provenance_diff.empty()) {
    report.findings.push_back({CompareKind::kMeasured, "provenance",
        "provenance differs; sources/mechanisms are not identical"});
  }

  report.identical_inputs = report.findings.empty();

  return ok(std::move(report));
}

std::string render_compare_text(const ComparisonReport& r) {
  std::ostringstream os;
  os << "=== comparison: " << r.left_label << " vs " << r.right_label << " ===\n";
  os << "observations: " << r.left_observations << " -> " << r.right_observations << "\n";
  os << "events: " << r.left_events << " -> " << r.right_events << "\n";
  os << "dropped: " << r.left_dropped << " -> " << r.right_dropped << "\n";
  os << "rejected: " << r.left_rejected << " -> " << r.right_rejected << "\n";
  os << "source_healthy: " << r.left_source_healthy << " -> " << r.right_source_healthy << "\n";
  os << "identical_inputs: " << (r.identical_inputs ? "yes" : "no") << "\n";

  os << "\nmetric diffs (derived):\n";
  for (const auto& md : r.metrics) {
    os << "  " << to_string(md.kind);
    if (md.left_mean) os << " mean: " << jnum(*md.left_mean);
    if (md.right_mean) os << " -> " << jnum(*md.right_mean);
    if (md.mean_delta) os << " (delta " << jnum(*md.mean_delta) << ")";
    os << "\n";
  }

  if (!r.domains_only_left.empty() || !r.domains_only_right.empty()) {
    os << "\ndomain inventory differences (measured):\n";
    for (const auto& d : r.domains_only_left) os << "  left-only: " << d << "\n";
    for (const auto& d : r.domains_only_right) os << "  right-only: " << d << "\n";
  }

  if (!r.diagnostics_only_left.empty() || !r.diagnostics_only_right.empty() ||
      !r.diagnostics_severity_changed.empty()) {
    os << "\ndiagnostic differences (heuristic):\n";
    for (const auto& d : r.diagnostics_only_left) os << "  left-only: " << d << "\n";
    for (const auto& d : r.diagnostics_only_right) os << "  right-only: " << d << "\n";
    for (const auto& d : r.diagnostics_severity_changed) os << "  severity changed: " << d << "\n";
  }

  if (!r.provenance_diff.empty()) {
    os << "\nprovenance differences (measured):\n";
    for (const auto& p : r.provenance_diff) os << "  " << p << "\n";
  }

  os << "\nfindings:\n";
  for (const auto& f : r.findings) {
    os << "  [" << to_string(f.kind) << "] " << f.category << ": " << f.detail << "\n";
  }
  return os.str();
}

std::string render_compare_json(const ComparisonReport& r) {
  std::ostringstream os;
  os << "{";
  os << "\"identical\":" << (r.identical_inputs ? "true" : "false") << ",";
  os << "\"observations\":{" << r.left_observations << "," << r.right_observations << "},";
  os << "\"events\":{" << r.left_events << "," << r.right_events << "},";
  os << "\"dropped\":{" << r.left_dropped << "," << r.right_dropped << "},";
  os << "\"rejected\":{" << r.left_rejected << "," << r.right_rejected << "},";
  os << "\"metrics\":[";
  bool first = true;
  for (const auto& md : r.metrics) {
    if (!first) os << ",";
    first = false;
    os << "{\"kind\":\"" << to_string(md.kind) << "\","
       << "\"left_mean\":" << (md.left_mean ? jnum(*md.left_mean) : std::string("null")) << ","
       << "\"right_mean\":" << (md.right_mean ? jnum(*md.right_mean) : std::string("null")) << ","
       << "\"mean_delta\":" << (md.mean_delta ? jnum(*md.mean_delta) : std::string("null")) << "}";
  }
  os << "],";
  os << "\"findings\":[";
  first = true;
  for (const auto& f : r.findings) {
    if (!first) os << ",";
    first = false;
    os << "{\"kind\":\"" << to_string(f.kind) << "\",\"category\":\"" << f.category
       << "\",\"detail\":\"" << f.detail << "\"}";
  }
  os << "]";
  os << "}";
  return os.str();
}

std::string render_compare_csv(const ComparisonReport& r) {
  std::ostringstream os;
  os << "category,kind,left,right,delta\n";
  os << "observations,measured," << r.left_observations << "," << r.right_observations << ","
     << static_cast<long long>(r.right_observations) - static_cast<long long>(r.left_observations) << "\n";
  for (const auto& md : r.metrics) {
    os << to_string(md.kind) << ",derived,"
       << (md.left_mean ? jnum(*md.left_mean) : std::string("")) << ","
       << (md.right_mean ? jnum(*md.right_mean) : std::string("")) << ","
       << (md.mean_delta ? jnum(*md.mean_delta) : std::string("")) << "\n";
  }
  return os.str();
}

}  // namespace memory_observatory
