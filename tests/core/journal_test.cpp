#include "fastmm/core/journal.hpp"

#include "test_support.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {
InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.flags = Instrument::kEnabled;
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  REQUIRE(t.add(i));
  return t;
}

JournalSessionInfo make_info(const InstrumentTable& t) {
  JournalSessionInfo info;
  info.session_id = 0xABCDEF;
  info.start_ts = Timestamp{1'700'000'000'000'000'000LL};
  info.tsc.tsc0 = 1;
  info.tsc.ns0 = 2;
  info.tsc.ns_per_cycle_q32 = 3;
  info.config_hash = 0x1234;
  info.rng_seed = 99;
  info.strategy = "basic_mm";
  info.instruments = &t;
  return info;
}

// Writes n trade messages (and one large book delta every 100) through a JournalWriter.
std::uint64_t write_messages(JournalWriter& w, int n) {
  std::uint64_t last = 0;
  std::vector<std::byte> big(BookDeltaMsg::size_for(200, 200));
  for (int i = 0; i < n; ++i) {
    if (i % 100 == 99) {
      auto* d = reinterpret_cast<BookDeltaMsg*>(big.data());
      init_header(*d,
                  EventType::BookDelta,
                  InstrumentId{1},
                  VenueId{0},
                  static_cast<std::uint32_t>(big.size()));
      d->bid_count = d->ask_count = 200;
      d->last_update_id = static_cast<std::uint64_t>(i);
      for (std::uint32_t k = 0; k < 400; ++k)
        d->levels()[k] = Level{Price::from_int(k), Qty::from_int(1)};
      auto r = w.record(d->hdr);
      REQUIRE(r);
      last = *r;
    } else {
      TradeMsg t{};
      init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
      t.trade_id = static_cast<std::uint64_t>(i);
      t.price = Price::from_int(i);
      auto r = w.record(t.hdr);
      REQUIRE(r);
      last = *r;
    }
  }
  return last;
}
}  // namespace

TEST_CASE("core.journal: write / read round trip with header, instruments, blocks, trailer") {
  const auto path = (tmp_dir() / "roundtrip.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 22);
  {
    JournalFileWriter fw(ring, path, make_info(table));
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    CHECK(w.next_seq() == 1);
    const int n = 8000;  // ~1.5 MiB of payload -> multiple blocks
    const std::uint64_t last = write_messages(w, n);
    CHECK(last == static_cast<std::uint64_t>(n));
    CHECK(w.recorded() == static_cast<std::uint64_t>(n));
    // sim-style single-threaded drain
    CHECK(fw.drain_once() == static_cast<std::size_t>(n));
    fw.stop();
    CHECK(fw.blocks_written() >= 2);
    CHECK(fw.messages_written() == static_cast<std::uint64_t>(n));
  }
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.header().session_id == 0xABCDEF);
  CHECK(r.header().config_hash == 0x1234);
  CHECK(r.header().rng_seed == 99);
  CHECK(r.header().tsc_ns_per_cycle_q32 == 3);
  CHECK(std::string(r.header().strategy) == "basic_mm");
  REQUIRE(r.instruments().size() == 2);
  CHECK(r.instruments()[1].symbol == "ETHUSDT");
  CHECK(r.has_trailer());
  CHECK_FALSE(r.truncated_tail());
  CHECK(r.message_count() == 8000);
  CHECK(r.last_seq() == 8000);
  std::uint64_t expect = 1;
  int deltas = 0;
  r.for_each([&](const EventHeader* h) {
    CHECK(h->seq == expect);
    if (h->type == EventType::BookDelta) {
      ++deltas;
      const auto& d = msg_cast<BookDeltaMsg>(h);
      CHECK(d.bid_count == 200);
      CHECK(d.levels()[399].price == Price::from_int(399));
    } else {
      CHECK(msg_cast<TradeMsg>(h).trade_id == expect - 1);
    }
    ++expect;
  });
  CHECK(expect == 8001);
  CHECK(deltas == 80);
}

TEST_CASE("core.journal: background thread writer") {
  const auto path = (tmp_dir() / "threaded.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 20);
  JournalFileWriter fw(ring, path, make_info(table));
  REQUIRE(fw.ok());
  fw.start();
  JournalWriter w(&ring);
  for (int i = 0; i < 20'000; ++i) {
    TradeMsg t{};
    init_header(t, EventType::Trade);
    t.trade_id = static_cast<std::uint64_t>(i);
    while (!w.record(t.hdr)) {
      // ring full: the file thread is draining; a real engine would treat this as fatal
      w = JournalWriter(&ring);  // not reached in practice; keep the seq monotonic anyway
    }
  }
  fw.stop();
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.message_count() == 20'000);
  CHECK(r.has_trailer());
}

TEST_CASE("core.journal: truncated tail block is discarded, earlier blocks survive") {
  const auto path = (tmp_dir() / "truncated.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 22);
  std::uint64_t blocks = 0;
  {
    JournalFileWriter fw(ring, path, make_info(table));
    JournalWriter w(&ring);
    write_messages(w, 8000);
    fw.drain_once();
    fw.stop();
    blocks = fw.blocks_written();
  }
  REQUIRE(blocks >= 2);
  // Chop the file in the middle of the last block (drops trailer + part of the last block).
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 100'000);
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.truncated_tail());
  CHECK_FALSE(r.has_trailer());
  CHECK(r.block_count() == blocks - 1);
  CHECK(r.message_count() < 8000);
  CHECK(r.message_count() > 0);
  std::uint64_t expect = 1;
  r.for_each([&](const EventHeader* h) { CHECK(h->seq == expect++); });
  CHECK(expect - 1 == r.message_count());

  // Corrupt a byte inside the first block payload -> CRC mismatch -> nothing readable.
  {
    const int fd = ::open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    const auto off = static_cast<off_t>(r.header().header_bytes + sizeof(JournalBlockHeader) + 100);
    const char x = 'X';
    REQUIRE(::pwrite(fd, &x, 1, off) == 1);
    ::close(fd);
  }
  JournalReader r2;
  REQUIRE(r2.open(path));
  CHECK(r2.truncated_tail());
  CHECK(r2.message_count() == 0);
  CHECK(r2.next() == nullptr);
}

TEST_CASE("core.journal: bad files are rejected") {
  JournalReader r;
  CHECK(r.open((tmp_dir() / "does-not-exist.fmj").string()).error() == JournalError::OpenFailed);
  const auto path = (tmp_dir() / "garbage.fmj").string();
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::vector<char> junk(1024, 'j');
    std::fwrite(junk.data(), 1, junk.size(), f);
    std::fclose(f);
  }
  CHECK(r.open(path).error() == JournalError::BadMagic);
  std::filesystem::resize_file(path, 10);
  CHECK(r.open(path).error() == JournalError::TooShort);
  JournalWriter disabled;
  CHECK_FALSE(disabled.enabled());
  CHECK(disabled.record(EventHeader{}).value() == 1);  // still assigns seq when disabled
}
