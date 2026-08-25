// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/report.h"

#include <cmath>
#include <sstream>

namespace memory_observatory {

namespace {

std::string json_escape(std::string_view s) {
  std::ostringstream os;
  for (char c : s) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          os << buf;
        } else {
          os << c;
        }
    }
  }
  return os.str();
}

// JSON numbers: NaN/Infinity are encoded as null (documented representation).
std::string jnum(std::optional<double> v) {
  if (!v || !std::isfinite(*v)) return "null";
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << *v;
  return os.str();
}

std::string jnum(double v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  return os.str();
}

std::string jstr(const std::string& s) { return "\"" + json_escape(s) + "\""; }

template <typename T>
std::string opt_value(const std::optional<T>& v, std::string (*fmt)(const T&)) {
  return v ? fmt(*v) : "null";
}

std::string fmt_count(std::uint64_t v) { return std::to_string(v); }

}  // namespace

std::string render_report_text(const Report& report) {
  std::ostringstream os;
  os << "=== " << report.title << " ===\n";
  os << "version: " << report.version << "\n";
  os << "session: " << report.session << "\n";
  os << "environment: " << report.os << " / " << report.architecture
     << " / " << report.toolchain << "\n";
  os << "sources: " << report.sources.size() << " domains: "
     << report.domains.size() << "\n";
  os << "retained: " << report.retained
     << "  ingested_total: " << report.counters.ingested_total
     << "  dropped_overflow: " << report.counters.dropped_overflow
     << "  rejected: " << report.counters.rejected_malformed +
                             report.counters.rejected_invalid +
                             report.counters.rejected_stale +
                             report.counters.rejected_out_of_order
     << "  duplicate_suppressed: " << report.counters.duplicate_suppressed << "\n";
  os << "accounting_balanced: " << (report.counters.is_balanced() ? "yes" : "no")
     << "\n";

  if (!report.provenance.empty()) {
    os << "\nprovenance:\n";
    for (const auto& p : report.provenance) {
      os << "  " << p.source << " " << p.source_name << " mechanism="
         << to_string(p.mechanism) << "\n";
    }
  }

  os << "\nmetric summaries:\n";
  for (const auto& ms : report.metric_summaries) {
    os << "  kind=" << to_string(ms.kind) << " samples=" << ms.sample_count;
    if (ms.aggregate.min) os << " min=" << jnum(ms.aggregate.min);
    if (ms.aggregate.max) os << " max=" << jnum(ms.aggregate.max);
    if (ms.aggregate.mean) os << " mean=" << jnum(ms.aggregate.mean);
    if (ms.percentiles.p95) os << " p95=" << jnum(ms.percentiles.p95);
    if (ms.percentiles.p99) os << " p99=" << jnum(ms.percentiles.p99);
    os << "\n";
  }

  os << "\nactive diagnostics (" << report.diagnostics.size() << "):\n";
  for (const auto& d : report.diagnostics) {
    os << "  [" << to_string(d.severity) << "] " << to_string(d.code) << ": "
       << d.summary << " (kind=" << to_string(d.kind) << ")\n";
  }

  os << "\ncorrelations (" << report.correlations.size() << "):\n";
  for (const auto& c : report.correlations) {
    os << "  [" << to_string(c.kind) << "] " << c.summary << "\n";
  }

  if (!report.timeline_excerpt.empty()) {
    os << "\ntimeline excerpt (" << report.timeline_excerpt.size() << " entries):\n";
    for (const auto& e : report.timeline_excerpt.entries) {
      os << "  t=" << e.observation.timestamp.monotonic
         << " src=" << e.observation.source
         << " dom=" << e.observation.domain
         << " payload=" << e.observation.payload.index() << "\n";
    }
  }

  os << "\nlimitations:\n";
  for (const auto& lim : report.limitations) {
    os << "  - " << lim << "\n";
  }
  return os.str();
}

std::string render_report_json(const Report& report) {
  std::ostringstream os;
  os << "{";
  os << "\"title\":\"" << json_escape(report.title) << "\",";
  os << "\"version\":\"" << json_escape(report.version) << "\",";
  os << "\"session\":" << report.session.value() << ",";
  os << "\"os\":\"" << json_escape(report.os) << "\",";
  os << "\"architecture\":\"" << json_escape(report.architecture) << "\",";
  os << "\"toolchain\":\"" << json_escape(report.toolchain) << "\",";
  os << "\"retained\":" << report.retained << ",";
  os << "\"counters\":{";
  os << "\"ingested_total\":" << report.counters.ingested_total << ",";
  os << "\"retained\":" << report.counters.retained << ",";
  os << "\"dropped_overflow\":" << report.counters.dropped_overflow << ",";
  os << "\"rejected_malformed\":" << report.counters.rejected_malformed << ",";
  os << "\"rejected_invalid\":" << report.counters.rejected_invalid << ",";
  os << "\"duplicate_suppressed\":" << report.counters.duplicate_suppressed;
  os << "},";

  os << "\"sources\":[" ;
  bool first = true;
  for (const auto& s : report.sources) {
    if (!first) os << ",";
    first = false;
    os << "{\"id\":" << s.id.value()
       << ",\"name\":\"" << json_escape(s.name)
       << "\",\"kind\":\"" << json_escape(s.kind)
       << "\",\"generation\":" << s.generation.value()
       << ",\"health\":\"" << to_string(s.health) << "\"}";
  }
  os << "],";

  os << "\"metrics\":[" ;
  first = true;
  for (const auto& ms : report.metric_summaries) {
    if (!first) os << ",";
    first = false;
    os << "{\"kind\":\"" << to_string(ms.kind)
       << "\",\"samples\":" << ms.sample_count
       << ",\"min\":" << jnum(ms.aggregate.min)
       << ",\"max\":" << jnum(ms.aggregate.max)
       << ",\"mean\":" << jnum(ms.aggregate.mean)
       << ",\"p95\":" << jnum(ms.percentiles.p95)
       << ",\"p99\":" << jnum(ms.percentiles.p99) << "}";
  }
  os << "],";

  os << "\"diagnostics\":[" ;
  first = true;
  for (const auto& d : report.diagnostics) {
    if (!first) os << ",";
    first = false;
    os << "{\"code\":\"" << to_string(d.code)
       << "\",\"severity\":\"" << to_string(d.severity)
       << "\",\"kind\":\"" << to_string(d.kind)
       << "\",\"summary\":\"" << json_escape(d.summary)
       << "\",\"confidence\":" << jnum(d.confidence.valid() ? d.confidence.score() : 0.0) << "}";
  }
  os << "],";

  os << "\"correlations\":[" ;
  first = true;
  for (const auto& c : report.correlations) {
    if (!first) os << ",";
    first = false;
    os << "{\"kind\":\"" << to_string(c.kind)
       << "\",\"summary\":\"" << json_escape(c.summary) << "\"}";
  }
  os << "],";

  os << "\"limitations\":[";
  first = true;
  for (const auto& lim : report.limitations) {
    if (!first) os << ",";
    first = false;
    os << "\"" << json_escape(lim) << "\"";
  }
  os << "]";
  os << "}";
  return os.str();
}

std::string render_report_csv(const Report& report) {
  std::ostringstream os;
  os << "metric,count,min,max,mean,p50,p90,p95,p99\n";
  for (const auto& ms : report.metric_summaries) {
    os << to_string(ms.kind) << "," << ms.sample_count << ","
       << jnum(ms.aggregate.min) << "," << jnum(ms.aggregate.max) << ","
       << jnum(ms.aggregate.mean) << "," << jnum(ms.percentiles.p50) << ","
       << jnum(ms.percentiles.p90) << "," << jnum(ms.percentiles.p95) << ","
       << jnum(ms.percentiles.p99) << "\n";
  }
  return os.str();
}

}  // namespace memory_observatory
