// Example: trace capture, serialize to binary, deserialize, and replay into a
// fresh Observatory. Demonstrates deterministic offline trace/replay.
#include <cstdio>
#include <vector>
#include "memory_observatory/observatory.h"
#include "memory_observatory/provider.h"
#include "memory_observatory/sampler.h"
#include "memory_observatory/trace.h"

using namespace memory_observatory;

int main() {
  // Capture.
  Observatory obs1;
  Sampler sampler(obs1);
  auto synth = make_synthetic_provider("syn", 5, DomainKind::kHostRam);
  if (synth) {
    (void)sampler.add_provider(std::move(synth), "host");
    for (int i = 0; i < 10; ++i) (void)sampler.collect_once();
  }
  auto trace_res = obs1.capture_trace();
  if (!trace_res.ok()) { std::printf("capture failed\n"); return 1; }
  auto bin = serialize_trace_binary(trace_res.value());
  if (!bin.ok()) { std::printf("serialize failed\n"); return 1; }
  std::printf("captured %zu records (%zu bytes binary)\n",
              trace_res.value().size(), bin.value().size());

  // Round-trip.
  auto parsed = deserialize_trace_binary(bin.value().data(), bin.value().size());
  if (!parsed.ok()) { std::printf("parse failed: %s\n", parsed.error().message().c_str()); return 1; }

  // Replay into a fresh observatory (deterministic, accelerated).
  Observatory obs2;
  ReplayResult rr = replay_trace(obs2, parsed.value(), ReplayOptions{});
  std::printf("replayed consumed=%llu accepted=%llu rejected=%llu\n",
              (unsigned long long)rr.consumed, (unsigned long long)rr.accepted,
              (unsigned long long)rr.rejected);
  return 0;
}
