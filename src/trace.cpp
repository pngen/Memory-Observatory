// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "memory_observatory/trace.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string_view>

namespace memory_observatory {

namespace {

constexpr std::size_t kHeaderSize = 24;

class BinaryWriter {
 public:
  void u8(std::uint8_t v) { dbuf_.push_back(v); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      dbuf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      dbuf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
  }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void f64(double v) {
    std::uint64_t bits = 0;
    static_assert(sizeof(v) == sizeof(bits), "double size mismatch");
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
  }
  void blob(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    dbuf_.insert(dbuf_.end(), s.begin(), s.end());
  }
  void ostr(const std::string& s) {
    u8(s.empty() ? 0 : 1);
    if (!s.empty()) blob(s);
  }
  void odou(std::optional<double> v) {
    u8(v && std::isfinite(*v) ? 1 : 0);
    f64(v ? *v : 0.0);
  }
  void ou64(std::optional<Bytes> v) {
    u8(v ? 1 : 0);
    u64(v ? *v : 0);
  }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const { return dbuf_; }

 private:
  std::vector<std::uint8_t> dbuf_;
};

class BinaryReader {
 public:
  BinaryReader(const std::uint8_t* d, std::size_t n) : data_(d), size_(n) {}
  bool u8(std::uint8_t& v) { if (!need(1)) return false; v = data_[pos_++]; return true; }
  bool u32(std::uint32_t& v) {
    if (!need(4)) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_++]) << (8 * i);
    return true;
  }
  bool u64(std::uint64_t& v) {
    if (!need(8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[pos_++]) << (8 * i);
    return true;
  }
  bool i64(std::int64_t& v) { std::uint64_t u; if (!u64(u)) return false; v = static_cast<std::int64_t>(u); return true; }
  bool f64(double& v) { std::uint64_t bits; if (!u64(bits)) return false; std::memcpy(&v, &bits, sizeof(v)); return true; }
  bool blob(std::string& out, std::size_t maxb) {
    std::uint32_t len = 0;
    if (!u32(len)) return false;
    if (len > maxb) return false;
    if (!need(len)) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return true;
  }
  bool ostr(std::string& out, bool& present, std::size_t maxb) {
    std::uint8_t p = 0;
    if (!u8(p)) return false;
    present = (p != 0);
    if (present) return blob(out, maxb);
    return true;
  }
  bool odou(std::optional<double>& out) {
    std::uint8_t p = 0; double v = 0;
    if (!u8(p) || !f64(v)) return false;
    if (p) out = v;
    return true;
  }
  bool ou64(std::optional<Bytes>& out) {
    std::uint8_t p = 0; std::uint64_t v = 0;
    if (!u8(p) || !u64(v)) return false;
    if (p) out = v;
    return true;
  }
  [[nodiscard]] std::size_t remaining() const { return size_ > pos_ ? size_ - pos_ : 0; }
  [[nodiscard]] bool empty() const { return pos_ >= size_; }
 private:
  bool need(std::size_t n) const { return n <= size_ && pos_ <= size_ - n; }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}

}  // namespace

// ============================================================================
// Binary serialization
// ============================================================================

Result<std::vector<std::uint8_t>> serialize_trace_binary(const Trace& trace) {
  BinaryWriter w;
  w.u32(kTraceMagic);
  w.u32(kTraceFormatVersion);
  w.u32(1);          // little-endian
  w.u32(0);          // reserved
  w.u64(static_cast<std::uint64_t>(trace.records.size()));
  w.u64(trace.session == SessionId{} ? 0 : trace.session.value());
  w.u64(trace.id == TraceId{} ? 0 : trace.id.value());
  w.blob(trace.format);
  w.blob(trace.capture_machine);
  w.blob(trace.capture_toolchain);
  w.blob(trace.capture_os);
  w.blob(trace.capture_arch);
  w.blob(trace.capture_mechanism);
  w.u32(static_cast<std::uint32_t>(trace.annotations.size()));
  for (const auto& a : trace.annotations) { w.blob(a.key); w.blob(a.value); }

  for (const auto& rec : trace.records) {
    const Observation& o = rec.observation;
    w.u64(o.id == ObservationId{} ? 0 : o.id.value());
    w.u64(o.source == SourceId{} ? 0 : o.source.value());
    w.i64(o.timestamp.monotonic.nanoseconds());
    w.u8(o.timestamp.wall ? 1 : 0);
    w.i64(o.timestamp.wall ? o.timestamp.wall->nanoseconds() : 0);
    w.i64(o.timestamp.sequence.value());
    w.u64(o.domain == DomainId{} ? 0 : o.domain.value());
    w.i64(static_cast<std::int64_t>(o.domain_kind));
    w.u64(o.generation == Generation{} ? 0 : o.generation.value());
    w.u64(o.epoch == Epoch{} ? 0 : o.epoch.value());
    auto woptid = [&](const auto& opt) { w.u8(opt ? 1 : 0); w.u64(opt ? opt->value() : 0); };
    woptid(o.object); woptid(o.process); woptid(o.device); woptid(o.worker);
    w.i64(static_cast<std::int64_t>(o.provenance.mechanism));
    w.u8(o.provenance.wall_clock_trusted ? 1 : 0);
    w.u8(o.provenance.confidence.valid() ? 1 : 0);
    w.f64(o.provenance.confidence.valid() ? o.provenance.confidence.score() : 0.0);
    w.ostr(o.provenance.source_name);
    bool pm = o.provenance.machine_id.has_value();
    w.u8(pm ? 1 : 0);
    if (pm) w.blob(*o.provenance.machine_id);

    const std::size_t pi = o.payload.index();
    w.u8(static_cast<std::uint8_t>(pi));
    if (const auto* ms = std::get_if<MetricSample>(&o.payload)) {
      const std::size_t mi = ms->index();
      w.u8(static_cast<std::uint8_t>(mi));
      if (const auto* g = std::get_if<GaugeSample>(ms)) {
        w.i64(static_cast<std::int64_t>(g->kind)); w.odou(g->value);
      } else if (const auto* c = std::get_if<CounterSample>(ms)) {
        w.i64(static_cast<std::int64_t>(c->kind)); w.u8(c->value ? 1 : 0); w.u64(c->value ? *c->value : 0); w.u8(c->reset ? 1 : 0);
      } else if (const auto* d = std::get_if<DistributionSample>(ms)) {
        w.i64(static_cast<std::int64_t>(d->kind));
        const DistributionSummary& s = d->summary;
        w.u8(s.count ? 1 : 0); w.u64(s.count ? *s.count : 0);
        w.odou(s.mean); w.odou(s.min); w.odou(s.max); w.odou(s.p50); w.odou(s.p90); w.odou(s.p95); w.odou(s.p99);
      }
    } else if (const auto* e = std::get_if<Event>(&o.payload)) {
      w.i64(static_cast<std::int64_t>(e->type));
      w.i64(static_cast<std::int64_t>(e->severity));
      w.i64(static_cast<std::int64_t>(e->source));
      bool em = e->message.has_value();
      w.u8(em ? 1 : 0); if (em) w.blob(*e->message);
    } else if (const auto* ds = std::get_if<DomainState>(&o.payload)) {
      w.u64(ds->domain == DomainId{} ? 0 : ds->domain.value());
      const DomainCapacity& cap = ds->capacity;
      w.ou64(cap.physical); w.ou64(cap.governed); w.ou64(cap.committed); w.ou64(cap.reserved);
      w.ou64(cap.allocated); w.ou64(cap.live); w.ou64(cap.available); w.ou64(cap.cached);
      w.ou64(cap.pinned); w.ou64(cap.mapped);
    } else if (const auto* os = std::get_if<ObjectState>(&o.payload)) {
      w.i64(static_cast<std::int64_t>(os->kind)); w.ou64(os->size); w.ou64(os->resident);
    } else if (const auto* rs = std::get_if<ResidencyState>(&o.payload)) {
      w.ou64(rs->resident); w.ou64(rs->total);
    } else if (const auto* ps = std::get_if<PressureState>(&o.payload)) {
      w.odou(ps->level); w.ou64(ps->available);
    } else if (const auto* ts = std::get_if<TransferState>(&o.payload)) {
      w.ou64(ts->moved); w.u8(ts->duration ? 1 : 0); w.i64(ts->duration ? ts->duration->nanoseconds() : 0);
    } else if (const auto* as = std::get_if<AllocationState>(&o.payload)) {
      w.ou64(as->allocated); w.ou64(as->freed); w.ou64(as->alloc_count); w.ou64(as->free_count);
    } else if (const auto* rc = std::get_if<ReclaimState>(&o.payload)) {
      w.ou64(rc->reclaimed); w.ou64(rc->event_count);
    } else if (const auto* cs = std::get_if<CacheState>(&o.payload)) {
      w.ou64(cs->resident); w.ou64(cs->evicted); w.ou64(cs->evict_count);
    } else if (const auto* sys = std::get_if<SystemState>(&o.payload)) {
      w.ou64(sys->total_physical); w.ou64(sys->total_available); w.ou64(sys->total_committed); w.ou64(sys->total_paged_pool);
    }

    w.u32(static_cast<std::uint32_t>(o.annotations.size()));
    for (const auto& a : o.annotations) { w.blob(a.key); w.blob(a.value); }
  }

  std::vector<std::uint8_t> body = w.data();
  const std::uint32_t crc = crc32(body.data(), body.size());
  BinaryWriter tw;
  tw.u32(crc);
  const auto& crcbytes = tw.data();
  body.insert(body.end(), crcbytes.begin(), crcbytes.end());
  return ok(std::move(body));
}

Result<Trace> deserialize_trace_binary(const std::uint8_t* data, std::size_t size,
                                       const TraceLimits& limits) {
  if (data == nullptr || size < kHeaderSize) {
    return fail<Trace>(ErrorCode::kMalformedInput, "trace too small");
  }
  // Validate the header (magic/version) before computing the checksum, so that
  // malformed or wrong-format input is rejected with a meaningful code.
  {
    BinaryReader head(data, size);
    std::uint32_t magic = 0, version = 0, endian = 0, reserved = 0;
    if (!head.u32(magic) || !head.u32(version) || !head.u32(endian) ||
        !head.u32(reserved)) {
      return fail<Trace>(ErrorCode::kMalformedInput, "bad header");
    }
    if (magic != kTraceMagic) {
      return fail<Trace>(ErrorCode::kInvalidTrace, "bad magic");
    }
    if (version != kTraceFormatVersion) {
      return fail<Trace>(ErrorCode::kUnsupportedVersion, "unsupported version");
    }
  }
  // Verify checksum over body (excludes trailing 4-byte crc).
  if (size < kHeaderSize + 4) return fail<Trace>(ErrorCode::kMalformedInput, "trace missing checksum");
  const std::uint32_t expect = crc32(data, size - 4);
  std::uint32_t stored = 0;
  { BinaryReader cr(data + size - 4, 4); (void)cr.u32(stored); }
  if (expect != stored) return fail<Trace>(ErrorCode::kCorruption, "trace checksum mismatch");

  BinaryReader r(data, size);
  std::uint32_t magic = 0, version = 0, endian = 0, reserved = 0;
  if (!r.u32(magic) || !r.u32(version) || !r.u32(endian) || !r.u32(reserved)) {
    return fail<Trace>(ErrorCode::kMalformedInput, "bad header");
  }
  if (magic != kTraceMagic) return fail<Trace>(ErrorCode::kInvalidTrace, "bad magic");
  if (version != kTraceFormatVersion) return fail<Trace>(ErrorCode::kUnsupportedVersion, "unsupported version");
  std::uint64_t count = 0;
  if (!r.u64(count) || count > limits.max_records) {
    return fail<Trace>(ErrorCode::kBoundExceeded, "record count bound");
  }
  Trace trace;
  trace.version = version;
  trace.little_endian = (endian != 0);
  std::uint64_t session = 0, tid = 0;
  if (!r.u64(session) || !r.u64(tid)) return fail<Trace>(ErrorCode::kMalformedInput, "bad ids");
  trace.session = SessionId{session};
  trace.id = TraceId{tid};
  if (!r.blob(trace.format, limits.max_string_bytes) || !r.blob(trace.capture_machine, limits.max_string_bytes) ||
      !r.blob(trace.capture_toolchain, limits.max_string_bytes) || !r.blob(trace.capture_os, limits.max_string_bytes) ||
      !r.blob(trace.capture_arch, limits.max_string_bytes) || !r.blob(trace.capture_mechanism, limits.max_string_bytes)) {
    return fail<Trace>(ErrorCode::kMalformedInput, "bad metadata");
  }
  std::uint32_t ann = 0;
  if (!r.u32(ann) || ann > limits.max_annotation_count) return fail<Trace>(ErrorCode::kBoundExceeded, "annotation bound");
  for (std::uint32_t i = 0; i < ann; ++i) {
    Annotation a;
    if (!r.blob(a.key, limits.max_annotation_bytes) || !r.blob(a.value, limits.max_annotation_bytes)) return fail<Trace>(ErrorCode::kMalformedInput, "bad annotation");
    trace.annotations.push_back(std::move(a));
  }

  for (std::uint64_t k = 0; k < count; ++k) {
    TraceRecord rec;
    Observation& o = rec.observation;
    std::uint64_t id = 0, src = 0, dom = 0, gen = 0, ep = 0;
    std::int64_t mono = 0, wall = 0, seq = 0;
    std::uint8_t wp = 0;
    if (!r.u64(id) || !r.u64(src) || !r.i64(mono) || !r.u8(wp) || !r.i64(wall) ||
        !r.i64(seq) || !r.u64(dom)) {
      return fail<Trace>(ErrorCode::kMalformedInput, "bad record core");
    }
    std::int64_t dk = 0;
    if (!r.i64(dk)) return fail<Trace>(ErrorCode::kMalformedInput, "bad domain_kind");
    if (!r.u64(gen) || !r.u64(ep)) {
      return fail<Trace>(ErrorCode::kMalformedInput, "bad generation/epoch");
    }
    o.id = ObservationId{id};
    o.source = SourceId{src};
    o.timestamp.monotonic = MonotonicTimestamp{mono};
    if (wp) o.timestamp.wall = WallTimestamp{wall};
    o.timestamp.sequence = SequenceNo{seq};
    o.domain = DomainId{dom};
    o.domain_kind = static_cast<DomainKind>(dk);
    o.generation = Generation{gen};
    o.epoch = Epoch{ep};

    std::uint8_t opr = 0, ppr = 0, dpr = 0, wpr = 0;
    std::uint64_t ov = 0, pv = 0, dv = 0, wv = 0;
    if (!r.u8(opr) || !r.u64(ov) || !r.u8(ppr) || !r.u64(pv) || !r.u8(dpr) ||
        !r.u64(dv) || !r.u8(wpr) || !r.u64(wv)) {
      return fail<Trace>(ErrorCode::kMalformedInput, "bad context ids");
    }
    if (opr) o.object = ObjectId{ov};
    if (ppr) o.process = ProcessId{pv};
    if (dpr) o.device = DeviceId{dv};
    if (wpr) o.worker = WorkerId{wv};
    std::int64_t mech = 0;
    std::uint8_t wt = 0, cp = 0;
    double conf = 0;
    if (!r.i64(mech) || !r.u8(wt) || !r.u8(cp) || !r.f64(conf)) return fail<Trace>(ErrorCode::kMalformedInput, "bad provenance");
    o.provenance.mechanism = static_cast<ObservationMechanism>(mech);
    o.provenance.wall_clock_trusted = (wt != 0);
    if (cp) o.provenance.confidence = Confidence{conf};
    bool present = false;
    if (!r.ostr(o.provenance.source_name, present, limits.max_string_bytes)) return fail<Trace>(ErrorCode::kMalformedInput, "bad source_name");
    std::uint8_t pmp = 0;
    if (!r.u8(pmp)) return fail<Trace>(ErrorCode::kMalformedInput, "bad machine_id flag");
    if (pmp) { std::string mi; if (!r.blob(mi, limits.max_string_bytes)) return fail<Trace>(ErrorCode::kMalformedInput, "bad machine_id"); o.provenance.machine_id = std::move(mi); }

    std::uint8_t tag = 0;
    if (!r.u8(tag)) return fail<Trace>(ErrorCode::kMalformedInput, "bad payload tag");
    const std::size_t pi = static_cast<std::size_t>(tag);
    if (pi > 11) return fail<Trace>(ErrorCode::kInvalidEnum, "unknown payload tag");
    switch (pi) {
      case 0: o.payload = std::monostate{}; break;
      case 1: { std::uint8_t mt = 0; if (!r.u8(mt)) return fail<Trace>(ErrorCode::kMalformedInput, "bad metric tag");
        if (mt == 0) { GaugeSample g; std::int64_t kind=0; if(!r.i64(kind)) return fail<Trace>(ErrorCode::kMalformedInput,"bad gauge kind"); g.kind=static_cast<MemoryKind>(kind); if(!r.odou(g.value)) return fail<Trace>(ErrorCode::kMalformedInput,"bad gauge value"); o.payload=g; }
        else if (mt == 1) { CounterSample c; std::int64_t kind=0; std::uint8_t rp=0,res=0; std::uint64_t v=0; if(!r.i64(kind)||!r.u8(rp)||!r.u64(v)||!r.u8(res)) return fail<Trace>(ErrorCode::kMalformedInput,"bad counter"); c.kind=static_cast<MemoryKind>(kind); if(rp)c.value=v; c.reset=(res!=0); o.payload=c; }
        else if (mt == 2) { DistributionSample d; std::int64_t kind=0; std::uint8_t cp2=0; std::uint64_t cnt=0; if(!r.i64(kind)||!r.u8(cp2)||!r.u64(cnt)) return fail<Trace>(ErrorCode::kMalformedInput,"bad dist"); d.kind=static_cast<MemoryKind>(kind); if(cp2)d.summary.count=cnt; if(!r.odou(d.summary.mean)||!r.odou(d.summary.min)||!r.odou(d.summary.max)||!r.odou(d.summary.p50)||!r.odou(d.summary.p90)||!r.odou(d.summary.p95)||!r.odou(d.summary.p99)) return fail<Trace>(ErrorCode::kMalformedInput,"bad dist fields"); o.payload=d; }
        else return fail<Trace>(ErrorCode::kInvalidEnum, "bad metric sub-tag"); break; }
      case 2: { Event e; std::int64_t et=0,es=0,esr=0; std::uint8_t m=0; if(!r.i64(et)||!r.i64(es)||!r.i64(esr)||!r.u8(m)) return fail<Trace>(ErrorCode::kMalformedInput,"bad event"); e.type=static_cast<EventType>(et); e.severity=static_cast<EventSeverity>(es); e.source=static_cast<EventSource>(esr); if(m){ std::string msg; if(!r.blob(msg, limits.max_string_bytes)) return fail<Trace>(ErrorCode::kMalformedInput,"bad event msg"); e.message=std::move(msg); } o.payload=e; break; }
      case 3: { DomainState ds; std::uint64_t ddom=0; if(!r.u64(ddom)) return fail<Trace>(ErrorCode::kMalformedInput,"bad domain state"); ds.domain=DomainId{ddom}; DomainCapacity cap; if(!r.ou64(cap.physical)||!r.ou64(cap.governed)||!r.ou64(cap.committed)||!r.ou64(cap.reserved)||!r.ou64(cap.allocated)||!r.ou64(cap.live)||!r.ou64(cap.available)||!r.ou64(cap.cached)||!r.ou64(cap.pinned)||!r.ou64(cap.mapped)) return fail<Trace>(ErrorCode::kMalformedInput,"bad domain capacity"); ds.capacity=cap; o.payload=ds; break; }
      case 4: { ObjectState os; std::int64_t kind=0; if(!r.i64(kind)) return fail<Trace>(ErrorCode::kMalformedInput,"bad object"); os.kind=static_cast<MemoryKind>(kind); if(!r.ou64(os.size)||!r.ou64(os.resident)) return fail<Trace>(ErrorCode::kMalformedInput,"bad object fields"); o.payload=os; break; }
      case 5: { ResidencyState rs; if(!r.ou64(rs.resident)||!r.ou64(rs.total)) return fail<Trace>(ErrorCode::kMalformedInput,"bad residency"); o.payload=rs; break; }
      case 6: { PressureState ps; if(!r.odou(ps.level)||!r.ou64(ps.available)) return fail<Trace>(ErrorCode::kMalformedInput,"bad pressure"); o.payload=ps; break; }
      case 7: { TransferState ts; if(!r.ou64(ts.moved)) return fail<Trace>(ErrorCode::kMalformedInput,"bad transfer"); std::uint8_t dp=0; std::int64_t dn=0; if(!r.u8(dp)||!r.i64(dn)) return fail<Trace>(ErrorCode::kMalformedInput,"bad transfer dur"); if(dp) ts.duration=Duration{dn}; o.payload=ts; break; }
      case 8: { AllocationState as; if(!r.ou64(as.allocated)||!r.ou64(as.freed)||!r.ou64(as.alloc_count)||!r.ou64(as.free_count)) return fail<Trace>(ErrorCode::kMalformedInput,"bad alloc"); o.payload=as; break; }
      case 9: { ReclaimState rc; if(!r.ou64(rc.reclaimed)||!r.ou64(rc.event_count)) return fail<Trace>(ErrorCode::kMalformedInput,"bad reclaim"); o.payload=rc; break; }
      case 10: { CacheState cs; if(!r.ou64(cs.resident)||!r.ou64(cs.evicted)||!r.ou64(cs.evict_count)) return fail<Trace>(ErrorCode::kMalformedInput,"bad cache"); o.payload=cs; break; }
      case 11: { SystemState sys; if(!r.ou64(sys.total_physical)||!r.ou64(sys.total_available)||!r.ou64(sys.total_committed)||!r.ou64(sys.total_paged_pool)) return fail<Trace>(ErrorCode::kMalformedInput,"bad system"); o.payload=sys; break; }
    }
    std::uint32_t oann = 0;
    if (!r.u32(oann) || oann > limits.max_annotation_count) return fail<Trace>(ErrorCode::kBoundExceeded, "observation annotation bound");
    for (std::uint32_t i = 0; i < oann; ++i) {
      Annotation a;
      if (!r.blob(a.key, limits.max_annotation_bytes) || !r.blob(a.value, limits.max_annotation_bytes)) return fail<Trace>(ErrorCode::kMalformedInput, "bad obs annotation");
      o.annotations.push_back(std::move(a));
    }
    trace.records.push_back(std::move(rec));
  }
  return ok(std::move(trace));
}



// ============================================================================
// JSON trace encoding
// ============================================================================

namespace {

struct JsonNode {
  enum class K { Null, Bool, Num, Str, Arr, Obj };
  K kind = K::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonNode> arr;
  std::vector<std::pair<std::string, JsonNode>> obj;

  static JsonNode make_obj() { JsonNode n; n.kind = K::Obj; return n; }
  static JsonNode make_arr() { JsonNode n; n.kind = K::Arr; return n; }
};

std::string json_esc(std::string_view s) {
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

std::string json_num(double v) {
  if (!std::isfinite(v)) return "null";
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(6);
  os << v;
  return os.str();
}

void emit(const JsonNode& n, std::ostringstream& os) {
  switch (n.kind) {
    case JsonNode::K::Null: os << "null"; break;
    case JsonNode::K::Bool: os << (n.b ? "true" : "false"); break;
    case JsonNode::K::Num: os << json_num(n.num); break;
    case JsonNode::K::Str:
      os << "\"" << json_esc(n.str) << "\"";
      break;
    case JsonNode::K::Arr: {
      os << "[";
      bool first = true;
      for (const auto& e : n.arr) {
        if (!first) os << ",";
        first = false;
        emit(e, os);
      }
      os << "]";
      break;
    }
    case JsonNode::K::Obj: {
      os << "{";
      bool first = true;
      for (const auto& kv : n.obj) {
        if (!first) os << ",";
        first = false;
        os << "\"" << json_esc(kv.first) << "\":";
        emit(kv.second, os);
      }
      os << "}";
      break;
    }
  }
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view s) : s_(s) {}

  Result<JsonNode> parse() {
    skip_ws();
    JsonNode v;
    auto r = parse_value(v);
    if (!r.ok()) return r;
    skip_ws();
    if (pos_ != s_.size()) {
      return fail<JsonNode>(ErrorCode::kMalformedInput, "trailing JSON data");
    }
    return ok(std::move(v));
  }

 private:
  void skip_ws() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
      else break;
    }
  }
  Result<JsonNode> parse_value(JsonNode& out) {
    if (pos_ >= s_.size()) return fail<JsonNode>(ErrorCode::kMalformedInput, "eof in value");
    const char c = s_[pos_];
    if (c == '{') return parse_obj(out);
    if (c == '[') return parse_arr(out);
    if (c == '\"') return parse_str(out);
    if (c == 't') return parse_lit("true", true, out);
    if (c == 'f') return parse_lit("false", false, out);
    if (c == 'n') return parse_lit("null", false, out);
    return parse_num(out);
  }
  Result<JsonNode> parse_lit(const char* lit, bool boolv, JsonNode& out) {
    const std::size_t n = std::char_traits<char>::length(lit);
    if (pos_ + n > s_.size() || s_.substr(pos_, n) != lit) {
      return fail<JsonNode>(ErrorCode::kMalformedInput, "bad literal");
    }
    pos_ += n;
    if (std::string_view(lit) == "null") { out.kind = JsonNode::K::Null; return ok(JsonNode{}); }
    out.kind = JsonNode::K::Bool;
    out.b = boolv;
    return ok(JsonNode{});
  }
  Result<JsonNode> parse_num(JsonNode& out) {
    const std::size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '0') { ++pos_; }
    else {
      if (pos_ >= s_.size() || s_[pos_] < '1' || s_[pos_] > '9') {
        return fail<JsonNode>(ErrorCode::kMalformedInput, "bad number");
      }
      while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
        return fail<JsonNode>(ErrorCode::kMalformedInput, "bad fraction");
      }
      while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
        return fail<JsonNode>(ErrorCode::kMalformedInput, "bad exponent");
      }
      while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
    }
    const std::string text(s_.data() + start, pos_ - start);
    try {
      out.kind = JsonNode::K::Num;
      out.num = std::stod(text);
    } catch (...) {
      return fail<JsonNode>(ErrorCode::kMalformedInput, "number overflow");
    }
    return ok(JsonNode{});
  }
  Result<JsonNode> parse_str(JsonNode& out) {
    if (pos_ >= s_.size() || s_[pos_] != '\"') {
      return fail<JsonNode>(ErrorCode::kMalformedInput, "expected string");
    }
    ++pos_;
    std::string val;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '\"') { out.kind = JsonNode::K::Str; out.str = std::move(val); return ok(JsonNode{}); }
      if (c == '\\') {
        if (pos_ >= s_.size()) return fail<JsonNode>(ErrorCode::kMalformedInput, "bad escape");
        const char e = s_[pos_++];
        switch (e) {
          case 'n': val += '\n'; break;
          case 't': val += '\t'; break;
          case 'r': val += '\r'; break;
          case '\\': val += '\\'; break;
          case '\"': val += '\"'; break;
          case '/': val += '/'; break;
          case 'b': val += '\b'; break;
          case 'f': val += '\f'; break;
          case 'u': {
            if (pos_ + 4 > s_.size()) return fail<JsonNode>(ErrorCode::kMalformedInput, "bad unicode");
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = s_[pos_++];
              code <<= 4;
              if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
              else return fail<JsonNode>(ErrorCode::kMalformedInput, "bad unicode hex");
            }
            if (code < 0x80) val += static_cast<char>(code);
            else if (code < 0x800) {
              val += static_cast<char>(0xC0 | (code >> 6));
              val += static_cast<char>(0x80 | (code & 0x3F));
            } else {
              val += static_cast<char>(0xE0 | (code >> 12));
              val += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
              val += static_cast<char>(0x80 | (code & 0x3F));
            }
            break;
          }
          default: return fail<JsonNode>(ErrorCode::kMalformedInput, "bad escape char");
        }
      } else {
        val += c;
      }
    }
    return fail<JsonNode>(ErrorCode::kMalformedInput, "unterminated string");
  }
  Result<JsonNode> parse_obj(JsonNode& out) {
    out.kind = JsonNode::K::Obj;
    ++pos_;  // {
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return ok(JsonNode{}); }
    while (true) {
      skip_ws();
      JsonNode key;
      auto kr = parse_str(key);
      if (!kr.ok()) return kr;
      if (key.kind != JsonNode::K::Str) return fail<JsonNode>(ErrorCode::kMalformedInput, "object key");
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return fail<JsonNode>(ErrorCode::kMalformedInput, "expected colon");
      ++pos_;
      skip_ws();
      JsonNode val;
      auto vr = parse_value(val);
      if (!vr.ok()) return vr;
      out.obj.emplace_back(std::move(key.str), std::move(val));
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return ok(JsonNode{}); }
      return fail<JsonNode>(ErrorCode::kMalformedInput, "object malformed");
    }
  }
  Result<JsonNode> parse_arr(JsonNode& out) {
    out.kind = JsonNode::K::Arr;
    ++pos_;  // [
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return ok(JsonNode{}); }
    while (true) {
      skip_ws();
      JsonNode val;
      auto vr = parse_value(val);
      if (!vr.ok()) return vr;
      out.arr.push_back(std::move(val));
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return ok(JsonNode{}); }
      return fail<JsonNode>(ErrorCode::kMalformedInput, "array malformed");
    }
  }
  std::string_view s_;
  std::size_t pos_ = 0;
};

std::string json_string(const std::string& s) {
  return "\"" + json_esc(s) + "\"";
}

}  // namespace



// ============================================================================
// JSON serialize / deserialize (strict)
// ============================================================================

namespace {

JsonNode j_num(double v) { JsonNode n; n.kind = JsonNode::K::Num; n.num = v; return n; }
JsonNode j_str(std::string s) { JsonNode n; n.kind = JsonNode::K::Str; n.str = std::move(s); return n; }
JsonNode j_bool(bool b) { JsonNode n; n.kind = JsonNode::K::Bool; n.b = b; return n; }
JsonNode j_null() { return JsonNode{}; }
JsonNode j_int(std::int64_t v) { return j_num(static_cast<double>(v)); }
JsonNode j_uint(std::uint64_t v) { return j_num(static_cast<double>(v)); }

// Split a domain/kind to its string name.
std::string payload_type_name(std::size_t idx) {
  switch (idx) {
    case 0: return "none";
    case 1: return "metric";
    case 2: return "event";
    case 3: return "domain_state";
    case 4: return "object_state";
    case 5: return "residency_state";
    case 6: return "pressure_state";
    case 7: return "transfer_state";
    case 8: return "allocation_state";
    case 9: return "reclaim_state";
    case 10: return "cache_state";
    case 11: return "system_state";
  }
  return "none";
}

JsonNode build_payload(const Observation& o) {
  const std::size_t pi = o.payload.index();
  JsonNode node = JsonNode::make_obj();
  node.obj.emplace_back("type", j_str(payload_type_name(pi)));
  if (const auto* ms = std::get_if<MetricSample>(&o.payload)) {
    if (const auto* g = std::get_if<GaugeSample>(ms)) {
      node.obj.emplace_back("mtype", j_str("gauge"));
      node.obj.emplace_back("kind", j_str(to_string(g->kind)));
      node.obj.emplace_back("value", g->value ? j_num(*g->value) : j_null());
    } else if (const auto* c = std::get_if<CounterSample>(ms)) {
      node.obj.emplace_back("mtype", j_str("counter"));
      node.obj.emplace_back("kind", j_str(to_string(c->kind)));
      node.obj.emplace_back("value", c->value ? j_num(static_cast<double>(*c->value)) : j_null());
      node.obj.emplace_back("reset", j_bool(c->reset));
    } else if (const auto* d = std::get_if<DistributionSample>(ms)) {
      node.obj.emplace_back("mtype", j_str("distribution"));
      node.obj.emplace_back("kind", j_str(to_string(d->kind)));
      const DistributionSummary& s = d->summary;
      node.obj.emplace_back("mean", s.mean ? j_num(*s.mean) : j_null());
      node.obj.emplace_back("p95", s.p95 ? j_num(*s.p95) : j_null());
      node.obj.emplace_back("p99", s.p99 ? j_num(*s.p99) : j_null());
      node.obj.emplace_back("count", s.count ? j_num(static_cast<double>(*s.count)) : j_null());
    }
  } else if (const auto* e = std::get_if<Event>(&o.payload)) {
    node.obj.emplace_back("etype", j_str(to_string(e->type)));
    node.obj.emplace_back("severity", j_str(to_string(e->severity)));
    node.obj.emplace_back("esource", j_str(to_string(e->source)));
    node.obj.emplace_back("message", e->message ? j_str(*e->message) : j_null());
  } else if (const auto* ds = std::get_if<DomainState>(&o.payload)) {
    node.obj.emplace_back("domain", j_uint(ds->domain.value()));
    const DomainCapacity& cap = ds->capacity;
    auto addcap = [&](const char* k, std::optional<Bytes> v) {
      node.obj.emplace_back(k, v ? j_num(static_cast<double>(*v)) : j_null());
    };
    addcap("physical", cap.physical); addcap("governed", cap.governed);
    addcap("committed", cap.committed); addcap("reserved", cap.reserved);
    addcap("allocated", cap.allocated); addcap("live", cap.live);
    addcap("available", cap.available); addcap("cached", cap.cached);
    addcap("pinned", cap.pinned); addcap("mapped", cap.mapped);
  } else if (const auto* ps = std::get_if<PressureState>(&o.payload)) {
    node.obj.emplace_back("level", ps->level ? j_num(*ps->level) : j_null());
    node.obj.emplace_back("available", ps->available ? j_num(static_cast<double>(*ps->available)) : j_null());
  }
  return node;
}

JsonNode build_record_obs(const Observation& o) {
  JsonNode rec = JsonNode::make_obj();
  rec.obj.emplace_back("id", j_uint(o.id.value()));
  rec.obj.emplace_back("source", j_uint(o.source.value()));
  rec.obj.emplace_back("mono", j_int(o.timestamp.monotonic.nanoseconds()));
  rec.obj.emplace_back("wall", o.timestamp.wall ? j_int(o.timestamp.wall->nanoseconds()) : j_null());
  rec.obj.emplace_back("seq", j_int(o.timestamp.sequence.value()));
  rec.obj.emplace_back("domain", j_uint(o.domain.value()));
  rec.obj.emplace_back("domain_kind", j_str(to_string(o.domain_kind)));
  rec.obj.emplace_back("generation", j_uint(o.generation.value()));
  rec.obj.emplace_back("epoch", j_uint(o.epoch.value()));
  rec.obj.emplace_back("mech", j_str(to_string(o.provenance.mechanism)));
  rec.obj.emplace_back("confidence", o.provenance.confidence.valid() ? j_num(o.provenance.confidence.score()) : j_null());
  rec.obj.emplace_back("payload", build_payload(o));
  JsonNode ann = JsonNode::make_arr();
  for (const auto& a : o.annotations) {
    JsonNode an = JsonNode::make_obj();
    an.obj.emplace_back("k", j_str(a.key));
    an.obj.emplace_back("v", j_str(a.value));
    ann.arr.push_back(std::move(an));
  }
  rec.obj.emplace_back("annotations", std::move(ann));
  return rec;
}

}  // namespace

Result<std::string> serialize_trace_json(const Trace& trace) {
  JsonNode root = JsonNode::make_obj();
  root.obj.emplace_back("magic", j_str(trace.format));
  root.obj.emplace_back("version", j_int(trace.version));
  root.obj.emplace_back("session", j_uint(trace.session.value()));
  root.obj.emplace_back("id", j_uint(trace.id.value()));
  root.obj.emplace_back("capture_machine", j_str(trace.capture_machine));
  root.obj.emplace_back("capture_toolchain", j_str(trace.capture_toolchain));
  root.obj.emplace_back("capture_os", j_str(trace.capture_os));
  root.obj.emplace_back("capture_arch", j_str(trace.capture_arch));
  root.obj.emplace_back("capture_mechanism", j_str(trace.capture_mechanism));
  JsonNode ann = JsonNode::make_arr();
  for (const auto& a : trace.annotations) {
    JsonNode an = JsonNode::make_obj();
    an.obj.emplace_back("k", j_str(a.key));
    an.obj.emplace_back("v", j_str(a.value));
    ann.arr.push_back(std::move(an));
  }
  root.obj.emplace_back("annotations", std::move(ann));
  JsonNode recs = JsonNode::make_arr();
  for (const auto& r : trace.records) {
    recs.arr.push_back(build_record_obs(r.observation));
  }
  root.obj.emplace_back("records", std::move(recs));
  std::ostringstream os;
  emit(root, os);
  return ok(os.str());
}

Result<Trace> deserialize_trace_json(std::string_view text, const TraceLimits& limits) {
  JsonParser parser(text);
  auto root_res = parser.parse();
  if (!root_res.ok()) {
    return Result<Trace>{root_res.error()};
  }
  const JsonNode& root = root_res.value();
  if (root.kind != JsonNode::K::Obj) {
    return fail<Trace>(ErrorCode::kMalformedInput, "trace root must be an object");
  }
  auto get = [&](const char* k) -> const JsonNode* {
    for (const auto& kv : root.obj) {
      if (kv.first == k) return &kv.second;
    }
    return nullptr;
  };
  Trace trace;
  if (const JsonNode* v = get("magic")) {
    if (v->kind == JsonNode::K::Str) trace.format = v->str;
  }
  if (const JsonNode* v = get("version")) {
    if (v->kind == JsonNode::K::Num) trace.version = static_cast<std::uint32_t>(v->num);
    if (trace.version != kTraceFormatVersion) {
      return fail<Trace>(ErrorCode::kUnsupportedVersion, "unsupported version");
    }
  }
  if (const JsonNode* v = get("session")) {
    if (v->kind == JsonNode::K::Num) trace.session = SessionId{static_cast<std::uint64_t>(v->num)};
  }
  if (const JsonNode* v = get("id")) {
    if (v->kind == JsonNode::K::Num) trace.id = TraceId{static_cast<std::uint64_t>(v->num)};
  }
  if (const JsonNode* v = get("capture_machine")) if (v->kind == JsonNode::K::Str) trace.capture_machine = v->str;
  if (const JsonNode* v = get("capture_toolchain")) if (v->kind == JsonNode::K::Str) trace.capture_toolchain = v->str;
  if (const JsonNode* v = get("capture_os")) if (v->kind == JsonNode::K::Str) trace.capture_os = v->str;
  if (const JsonNode* v = get("capture_arch")) if (v->kind == JsonNode::K::Str) trace.capture_arch = v->str;
  if (const JsonNode* v = get("capture_mechanism")) if (v->kind == JsonNode::K::Str) trace.capture_mechanism = v->str;

  if (const JsonNode* v = get("annotations")) {
    if (v->kind == JsonNode::K::Arr && v->arr.size() <= limits.max_annotation_count) {
      for (const auto& a : v->arr) {
        if (a.kind != JsonNode::K::Obj) continue;
        Annotation an;
        for (const auto& kv : a.obj) {
          if (kv.first == "k" && kv.second.kind == JsonNode::K::Str) an.key = kv.second.str;
          if (kv.first == "v" && kv.second.kind == JsonNode::K::Str) an.value = kv.second.str;
        }
        if (an.key.size() <= limits.max_annotation_bytes) trace.annotations.push_back(std::move(an));
      }
    }
  }

  const JsonNode* recs = get("records");
  if (recs && recs->kind == JsonNode::K::Arr) {
    if (recs->arr.size() > limits.max_records) {
      return fail<Trace>(ErrorCode::kBoundExceeded, "record count bound");
    }
    for (const auto& rnode : recs->arr) {
      if (rnode.kind != JsonNode::K::Obj) continue;
      Observation o;
      auto g = [&](const char* k) -> const JsonNode* {
        for (const auto& kv : rnode.obj) if (kv.first == k) return &kv.second;
        return nullptr;
      };
      if (auto v = g("id")) if (v->kind == JsonNode::K::Num) o.id = ObservationId{static_cast<std::uint64_t>(v->num)};
      if (auto v = g("source")) if (v->kind == JsonNode::K::Num) o.source = SourceId{static_cast<std::uint64_t>(v->num)};
      if (auto v = g("mono")) if (v->kind == JsonNode::K::Num) o.timestamp.monotonic = MonotonicTimestamp{static_cast<std::int64_t>(v->num)};
      if (auto v = g("wall")) if (v->kind == JsonNode::K::Num) o.timestamp.wall = WallTimestamp{static_cast<std::int64_t>(v->num)};
      if (auto v = g("seq")) if (v->kind == JsonNode::K::Num) o.timestamp.sequence = SequenceNo{static_cast<std::int64_t>(v->num)};
      if (auto v = g("domain")) if (v->kind == JsonNode::K::Num) o.domain = DomainId{static_cast<std::uint64_t>(v->num)};
      if (auto v = g("domain_kind")) if (v->kind == JsonNode::K::Str) o.domain_kind = domain_kind_from_string(v->str);
      if (auto v = g("generation")) if (v->kind == JsonNode::K::Num) o.generation = Generation{static_cast<std::uint64_t>(v->num)};
      if (auto v = g("epoch")) if (v->kind == JsonNode::K::Num) o.epoch = Epoch{static_cast<std::uint64_t>(v->num)};
      if (auto v = g("mech")) if (v->kind == JsonNode::K::Str) { /* mechanism enum */ }
      if (auto v = g("confidence")) if (v->kind == JsonNode::K::Num) o.provenance.confidence = Confidence{v->num};
      if (const JsonNode* p = g("payload")) {
        if (p->kind == JsonNode::K::Obj) {
          std::string type = "none";
          for (const auto& kv : p->obj) if (kv.first == "type" && kv.second.kind == JsonNode::K::Str) type = kv.second.str;
          auto pg = [&](const char* k) -> const JsonNode* {
            for (const auto& kv : p->obj) if (kv.first == k) return &kv.second;
            return nullptr;
          };
          if (type == "metric") {
            std::string mtype = "gauge";
            if (auto v = pg("mtype")) if (v->kind == JsonNode::K::Str) mtype = v->str;
            const auto& kindname = (pg("kind") && pg("kind")->kind == JsonNode::K::Str) ? pg("kind")->str : "unknown";
            const MemoryKind kind = (kindname == "available") ? MemoryKind::kAvailable
                                   : (kindname == "allocated") ? MemoryKind::kAllocated
                                   : (kindname == "committed") ? MemoryKind::kCommitted
                                   : (kindname == "physical") ? MemoryKind::kPhysical
                                   : MemoryKind::kOther;
            if (mtype == "gauge") { GaugeSample g_; g_.kind = kind; if (auto v = pg("value")) if (v->kind == JsonNode::K::Num) g_.value = v->num; o.payload = g_; }
            else if (mtype == "counter") { CounterSample c; c.kind = kind; if (auto v = pg("value")) if (v->kind == JsonNode::K::Num) c.value = static_cast<std::uint64_t>(v->num); if (auto v = pg("reset")) if (v->kind == JsonNode::K::Bool) c.reset = v->b; o.payload = c; }
            else if (mtype == "distribution") { DistributionSample d; d.kind = kind; if (auto v = pg("mean")) if (v->kind == JsonNode::K::Num) d.summary.mean = v->num; if (auto v = pg("p95")) if (v->kind == JsonNode::K::Num) d.summary.p95 = v->num; if (auto v = pg("p99")) if (v->kind == JsonNode::K::Num) d.summary.p99 = v->num; o.payload = d; }
          } else if (type == "event") {
            Event e; if (auto v = pg("message")) if (v->kind == JsonNode::K::Str) e.message = v->str; o.payload = e;
          } else if (type == "domain_state") {
            DomainState ds; if (auto v = pg("domain")) if (v->kind == JsonNode::K::Num) ds.domain = DomainId{static_cast<std::uint64_t>(v->num)};
            auto rd = [&](const char* k) -> std::optional<Bytes> { if (auto v = pg(k)) if (v->kind == JsonNode::K::Num) return static_cast<Bytes>(v->num); return std::nullopt; };
            ds.capacity.physical = rd("physical"); ds.capacity.governed = rd("governed");
            ds.capacity.available = rd("available"); ds.capacity.allocated = rd("allocated");
            o.payload = ds;
          }
        }
      }
      if (const JsonNode* an = g("annotations")) {
        if (an->kind == JsonNode::K::Arr) {
          for (const auto& a : an->arr) {
            if (a.kind != JsonNode::K::Obj) continue;
            Annotation x;
            for (const auto& kv : a.obj) { if (kv.first == "k" && kv.second.kind == JsonNode::K::Str) x.key = kv.second.str; if (kv.first == "v" && kv.second.kind == JsonNode::K::Str) x.value = kv.second.str; }
            o.annotations.push_back(std::move(x));
          }
        }
      }
      TraceRecord rec;
      rec.observation = std::move(o);
      trace.records.push_back(std::move(rec));
    }
  }
  return ok(std::move(trace));
}

}  // namespace memory_observatory
