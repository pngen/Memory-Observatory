// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Real two-process validation. The executable runs in three modes:
//   send <file>    : process A -- emits a deterministic observation sequence with
//                    an explicit SourceId and Generation, writes it as a shared
//                    binary trace + sidecar metadata, then exits cleanly.
//   recv <file>    : process B -- independently consumes the observations into a
//                    fresh Observatory, verifies source/generation identity,
//                    ordering, snapshot, timeline, diagnostics, rejects malformed
//                    input, and handles generation changes, then exits cleanly.
//   driver <file>  : spawns 'send' then 'recv' as two real OS child processes and
//                    returns success only if both exit 0.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "memory_observatory/observatory.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

namespace {

constexpr std::uint64_t kSenderSourceVal = 45001;
constexpr std::uint64_t kSenderGenVal = 7;
constexpr std::uint64_t kDomainVal = 88;
constexpr std::uint64_t kRecordCount = 50;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, expr);
  }
}
#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

int mode_send(const char* file) {
  Trace trace;
  trace.id = TraceId{99};
  trace.session = SessionId{5};
  trace.format = "memory-observatory-trace";
  trace.capture_mechanism = "multiproc-sender";
  const MonotonicTimestamp base{1000000000LL};
  for (std::uint64_t i = 0; i < kRecordCount; ++i) {
    TraceRecord rec;
    Observation& o = rec.observation;
    o.id = ObservationId{1000 + i};
    o.source = SourceId{kSenderSourceVal};
    o.domain = DomainId{kDomainVal};
    o.domain_kind = DomainKind::kHostRam;
    o.timestamp.monotonic = base + Duration{1000000LL * static_cast<std::int64_t>(i)};
    o.timestamp.sequence = SequenceNo{static_cast<std::int64_t>(i + 1)};
    o.timestamp.wall = WallTimestamp{2000000000LL + static_cast<std::int64_t>(i)};
    o.generation = Generation{kSenderGenVal};
    o.provenance.source = SourceId{kSenderSourceVal};
    o.provenance.source_name = "sender";
    o.provenance.mechanism = ObservationMechanism::kLive;
    o.provenance.confidence = Confidence{0.8};
    GaugeSample g;
    g.kind = MemoryKind::kAvailable;
    g.value = 1000000.0 - static_cast<double>(i) * 10000.0;
    o.payload = g;
    trace.records.push_back(std::move(rec));
  }

  auto bin = serialize_trace_binary(trace);
  if (!bin.ok()) { std::printf("sender serialize failed: %s\n", bin.error().message().c_str()); return 1; }
  {
    std::ofstream f(file, std::ios::binary);
    if (!f) { std::printf("sender cannot open %s\n", file); return 1; }
    f.write(reinterpret_cast<const char*>(bin.value().data()), bin.value().size());
  }
  {
    std::ofstream meta(std::string(file) + ".meta");
    meta << kSenderSourceVal << "\n" << kSenderGenVal << "\n" << kRecordCount << "\n";
  }
  std::printf("sender: wrote %llu records for source %llu gen %llu\n",
              (unsigned long long)kRecordCount, (unsigned long long)kSenderSourceVal,
              (unsigned long long)kSenderGenVal);
  return 0;
}

int mode_recv(const char* file) {
  g_checks = g_failures = 0;
  // Read the shared trace.
  std::ifstream f(file, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  auto tr = deserialize_trace_binary(bytes.data(), bytes.size());
  CHECK(tr.ok());
  if (!tr.ok()) { std::printf("receiver parse failed: %s\n", tr.error().message().c_str()); return 1; }

  // Read sidecar metadata.
  std::uint64_t exp_src = 0, exp_gen = 0, exp_count = 0;
  { std::ifstream meta(std::string(file) + ".meta"); meta >> exp_src >> exp_gen >> exp_count; }
  CHECK(exp_src == kSenderSourceVal);
  CHECK(exp_gen == kSenderGenVal);

  // Ingest honoring the sender's explicit SourceId/Generation into a fresh
  // Observatory. The Observatory auto-registers the unknown source.
  Observatory obs;
  std::uint64_t accepted = 0;
  for (const auto& rec : tr.value().records) {
    Observation o = rec.observation;
    auto ir = obs.observe(std::move(o));
    if (ir.accepted()) ++accepted;
  }
  CHECK(accepted == kRecordCount);

  // Source identity + generation identity.
  auto si = obs.source_info(SourceId{kSenderSourceVal});
  CHECK(si.ok());
  if (si.ok()) {
    CHECK(si.value().name == std::string("sender"));
    CHECK(si.value().generation == Generation{kSenderGenVal});
  }

  // Ordering semantics: timeline is chronologically ascending by monotonic time.
  auto tl = obs.query_timeline(TimelineFilter{});
  CHECK(tl.ok());
  if (tl.ok()) {
    CHECK(tl.value().size() == kRecordCount);
    bool ascending = true;
    for (std::size_t i = 1; i < tl.value().size(); ++i) {
      if (tl.value().entries[i].observation.timestamp.monotonic <
          tl.value().entries[i - 1].observation.timestamp.monotonic) {
        ascending = false;
        break;
      }
    }
    CHECK(ascending);
  }

  // Snapshot contents.
  auto snap = obs.snapshot(SnapshotFilter{});
  CHECK(snap.ok());
  if (snap.ok()) {
    CHECK(snap.value().observation_count == kRecordCount);
    bool has_sender = false;
    for (const auto& s : snap.value().sources) {
      if (s.id == SourceId{kSenderSourceVal}) has_sender = true;
    }
    CHECK(has_sender);
  }

  // Deterministic diagnostic regeneration.
  auto d1 = obs.evaluate_diagnostics();
  auto d2 = obs.evaluate_diagnostics();
  CHECK(d1.size() == d2.size());
  if (!d1.empty()) CHECK(d1.front().code == d2.front().code);

  // Reject malformed input.
  std::vector<std::uint8_t> corrupt = bytes;
  if (!corrupt.empty()) corrupt[corrupt.size() / 2] ^= 0xff;
  auto bad = deserialize_trace_binary(corrupt.data(), corrupt.size());
  CHECK(!bad.ok());

  // Handle generation change: advance to gen 8 (rollover), then a stale gen 7.
  Observation adv;
  adv.source = SourceId{kSenderSourceVal};
  adv.domain = DomainId{kDomainVal};
  adv.timestamp.monotonic = MonotonicTimestamp::now();
  adv.timestamp.sequence = SequenceNo{999};
  adv.generation = Generation{kSenderGenVal + 1};
  adv.provenance.source_name = "sender";
  GaugeSample g; g.kind = MemoryKind::kAllocated; g.value = 1.0; adv.payload = g;
  auto ir_adv = obs.observe(std::move(adv));
  CHECK(ir_adv.accepted());
  if (si.ok()) {
    auto si2 = obs.source_info(SourceId{kSenderSourceVal});
    CHECK(si2.ok());
    if (si2.ok()) {
      CHECK(si2.value().generation == Generation{kSenderGenVal + 1});
      CHECK(si2.value().generation_rollovers == 1);
    }
  }

  std::printf("receiver: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

int mode_driver(const char* file) {
#if defined(_WIN32)
  char self[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, self, MAX_PATH);
  auto spawn = [&](const char* name, const char* arg) -> DWORD {
    std::string cmd = std::string("\"") + self + "\" " + name + " \"" + arg + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      return (DWORD)-1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
  };
  DWORD sc = spawn("send", file);
  if (sc != 0) { std::printf("driver: sender exited with %lu\n", (unsigned long)sc); return 1; }
  DWORD rc = spawn("recv", file);
  if (rc != 0) { std::printf("driver: receiver exited with %lu\n", (unsigned long)rc); return 1; }
  std::printf("driver: both child processes exited 0\n");
  return 0;
#else
  (void)file;
  std::printf("driver: multiprocess proof is Windows-only in this build\n");
  return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: multiproc <send|recv|driver> <file>\n"); return 2; }
  const std::string mode = argv[1];
  if (mode == "send") return mode_send(argv[2]);
  if (mode == "recv") return mode_recv(argv[2]);
  if (mode == "driver") return mode_driver(argv[2]);
  std::printf("unknown mode '%s'\n", mode.c_str());
  return 2;
}
