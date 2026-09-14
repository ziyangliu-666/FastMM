// Journal format v2 (ADR-0010): session settings and the embedded configuration in the header,
// the engine-time chain (kEngineTime deltas and EngineTimeMsg records), dropped outbound marks, and
// reading version 1 files.
#include "test_support.hpp"

#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

InstrumentTable one_instrument() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.flags = Instrument::kEnabled;
  REQUIRE(t.add(i));
  return t;
}

TradeMsg trade(std::uint64_t id) {
  TradeMsg t{};
  init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
  t.trade_id = id;
  return t;
}

// Every record of the ring, in order, as copies.
std::vector<std::vector<std::byte>> drain(MsgRing& ring) {
  std::vector<std::vector<std::byte>> out;
  while (const std::byte* p = ring.try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(p);
    out.emplace_back(p, p + h->len);
    ring.release();
  }
  return out;
}
const EventHeader& hdr(const std::vector<std::byte>& m) {
  return *reinterpret_cast<const EventHeader*>(m.data());
}
std::int32_t delta(const std::vector<std::byte>& m) {
  return static_cast<std::int32_t>(hdr(m).reserved0);
}

void rewrite_header(const std::string& path, const JournalFileHeader& h) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  REQUIRE(fd >= 0);
  REQUIRE(::pwrite(fd, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h));
  ::close(fd);
}

}  // namespace

TEST_CASE("core.journal v2: header carries the session settings and the effective config") {
  const auto path = (tmp_dir() / "v2_header.fmj").string();
  const InstrumentTable table = one_instrument();
  const std::string toml = "[engine]\nrng_seed = 42\n";  // 26 bytes: padded to 64
  MsgRing ring(1 << 16);
  {
    JournalSessionInfo info;
    info.session_id = 5;
    info.config_hash = 0xFEED;
    info.rng_seed = 42;
    info.strategy = "basic_mm";
    info.instruments = &table;
    info.has_session = true;
    info.session_epoch = 23;
    info.quoting_enabled = false;
    info.replace_venues = 0b101;
    info.config_toml = toml;
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    for (std::uint64_t i = 0; i < 10; ++i) REQUIRE(w.record(trade(i).hdr));
    fw.drain_once();
    fw.stop();
  }
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.version() == 2);
  CHECK(r.header().version == kJournalVersion);
  CHECK(r.has_session());
  CHECK(r.header().session_epoch == 23);
  CHECK(r.header().quoting_enabled == 0);
  CHECK(r.header().replace_venues == 0b101);
  CHECK(r.header().rng_seed == 42);
  CHECK(r.header().config_hash == 0xFEED);
  CHECK(r.config_text() == toml);
  CHECK(r.header().header_bytes % 64 == 0);
  CHECK(r.header().header_bytes == sizeof(JournalFileHeader) + sizeof(Instrument) + 64);
  CHECK(r.message_count() == 10);
  std::uint64_t n = 0;
  r.for_each([&](const EventHeader* h) { CHECK(msg_cast<TradeMsg>(h).trade_id == n++); });
  CHECK(n == 10);

  // A damaged config byte fails the config checksum.
  {
    const int fd = ::open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    const char x = '#';
    const auto off = static_cast<off_t>(sizeof(JournalFileHeader) + sizeof(Instrument) + 3);
    REQUIRE(::pwrite(fd, &x, 1, off) == 1);
    ::close(fd);
  }
  JournalReader bad;
  CHECK(bad.open(path).error() == JournalError::HeaderCorrupt);
}

TEST_CASE("core.journal v2: a journal without session settings or config") {
  const auto path = (tmp_dir() / "v2_plain.fmj").string();
  const InstrumentTable table = one_instrument();
  MsgRing ring(1 << 16);
  {
    JournalSessionInfo info;
    info.instruments = &table;
    JournalFileWriter fw(ring, path, info);
    JournalWriter w(&ring);
    REQUIRE(w.record(trade(1).hdr));
    fw.drain_once();
    fw.stop();
  }
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.version() == 2);
  CHECK_FALSE(r.has_session());
  CHECK(r.config_text().empty());
  CHECK(r.header().header_bytes == sizeof(JournalFileHeader) + sizeof(Instrument));
  CHECK(r.message_count() == 1);
}

TEST_CASE("core.journal v2: version 1 files open and unknown versions are rejected") {
  const auto path = (tmp_dir() / "v1_file.fmj").string();
  const InstrumentTable table = one_instrument();
  MsgRing ring(1 << 16);
  {
    JournalSessionInfo info;
    info.rng_seed = 9;
    info.instruments = &table;
    JournalFileWriter fw(ring, path, info);
    JournalWriter w(&ring);
    for (std::uint64_t i = 0; i < 3; ++i) REQUIRE(w.record(trade(i).hdr));
    fw.drain_once();
    fw.stop();
  }
  JournalReader r2;
  REQUIRE(r2.open(path));
  JournalFileHeader h = r2.header();
  // A v1 writer left the v2 bytes zero; the reader must ignore whatever is there anyway.
  h.version = 1;
  h.header_flags = kHeaderSession;
  h.session_epoch = 77;
  h.config_bytes = 0;
  h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
  rewrite_header(path, h);
  JournalReader r1;
  REQUIRE(r1.open(path));
  CHECK(r1.version() == 1);
  CHECK_FALSE(r1.has_session());
  CHECK(r1.config_text().empty());
  CHECK(r1.header().rng_seed == 9);
  CHECK(r1.message_count() == 3);

  h.version = kJournalVersion + 1;
  h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
  rewrite_header(path, h);
  JournalReader r3;
  CHECK(r3.open(path).error() == JournalError::BadVersion);
}

TEST_CASE("core.journal v2: engine time is a delta chain with absolute syncs") {
  MsgRing ring(1 << 16);
  JournalWriter w(&ring);
  const Timestamp t0{1'789'000'000'000'000'000LL};
  REQUIRE(w.record_clock(EngineTimeMsg::Kind::Start, t0));
  // No absolute record needed after the start record.
  REQUIRE(w.record_at(trade(1).hdr, t0 + Duration{5}));
  // A backward step fits in int32 ns.
  REQUIRE(w.record_at(trade(2).hdr, t0 - milliseconds(300)));
  // A gap of 10 s does not: an EngineTimeMsg first, then a zero delta.
  REQUIRE(w.record_at(trade(3).hdr, t0 + seconds(10)));
  // The outbound copy carries no engine time, and the dropped mark only when asked.
  OutCancelMsg oc{};
  init_header(oc, EventType::OutCancel);
  oc.hdr.flags = EventHeader::kEngineTime;  // must not leak into the record
  oc.hdr.reserved0 = 1234;
  REQUIRE(w.record_outbound(oc.hdr));
  REQUIRE(w.record_outbound(oc.hdr, /*dropped=*/true));
  REQUIRE(w.record_at(trade(4).hdr, t0 + seconds(10) + milliseconds(1)));
  // A plain record() of a message that was journaled with engine time clears it.
  TradeMsg copied = trade(5);
  copied.hdr.flags = EventHeader::kEngineTime;
  copied.hdr.reserved0 = 99;
  REQUIRE(w.record(copied.hdr));
  REQUIRE(w.record_clock(EngineTimeMsg::Kind::Finish, t0 + seconds(11)));

  const auto recs = drain(ring);
  REQUIRE(recs.size() == 10);
  CHECK(hdr(recs[0]).type == EventType::EngineTime);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[0])).kind == EngineTimeMsg::Kind::Start);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[0])).engine_ts == t0);
  CHECK((hdr(recs[1]).flags & EventHeader::kEngineTime) != 0);
  CHECK(delta(recs[1]) == 5);
  CHECK(delta(recs[2]) == static_cast<std::int32_t>(-milliseconds(300).ns - 5));
  CHECK(hdr(recs[3]).type == EventType::EngineTime);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[3])).kind == EngineTimeMsg::Kind::Sync);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[3])).engine_ts == t0 + seconds(10));
  CHECK(delta(recs[4]) == 0);
  CHECK(hdr(recs[5]).flags == EventHeader::kOutbound);
  CHECK(hdr(recs[5]).reserved0 == 0);
  CHECK(hdr(recs[6]).flags == (EventHeader::kOutbound | EventHeader::kDropped));
  CHECK(delta(recs[7]) == static_cast<std::int32_t>(milliseconds(1).ns));
  CHECK(hdr(recs[8]).flags == 0);
  CHECK(hdr(recs[8]).reserved0 == 0);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[9])).kind == EngineTimeMsg::Kind::Finish);
  std::uint64_t seq = 1;
  for (const auto& m : recs) CHECK(hdr(m).seq == seq++);
}

TEST_CASE("core.journal v2: a full ring breaks the chain and the next event re-syncs") {
  MsgRing ring(1 << 12);
  JournalWriter w(&ring);
  Timestamp t{1'000'000'000};
  REQUIRE(w.record_clock(EngineTimeMsg::Kind::Start, t));
  int accepted = 0;
  while (w.record_at(trade(1).hdr, t = t + Duration{10})) ++accepted;
  CHECK(accepted > 0);
  CHECK(w.overflows() == 1);
  static_cast<void>(drain(ring));
  // The lost record's time never reached the file: the chain restarts from an absolute value.
  REQUIRE(w.record_at(trade(2).hdr, t + Duration{10}));
  const auto recs = drain(ring);
  REQUIRE(recs.size() == 2);
  CHECK(hdr(recs[0]).type == EventType::EngineTime);
  CHECK(msg_cast<EngineTimeMsg>(&hdr(recs[0])).engine_ts == t + Duration{10});
  CHECK(delta(recs[1]) == 0);
}
