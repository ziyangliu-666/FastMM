// JournalSource with remap: the events come back with the ids and venues of the instruments of
// the same symbol in the configuration's table (the recording's venue first, else any), so
// recordings of different instrument sets merge; a symbol the configuration lacks drops its
// events, and note() counts them.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

Instrument instrument(const char* symbol, VenueId venue) {
  Instrument i{};
  i.symbol = symbol;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.flags = Instrument::kEnabled;
  i.venue = venue;
  return i;
}

// The recording's table: X then Y, both on venue 0, so ids 0 and 1.
InstrumentTable recorded() {
  InstrumentTable t;
  REQUIRE(t.add(instrument("X", VenueId{0})));
  REQUIRE(t.add(instrument("Y", VenueId{0})));
  return t;
}

// The configuration's table: Y is id 0 on venue 0, Z id 1, X id 2 on venue 1 (the recording had
// it on venue 0: found by symbol alone), and a second Y on venue 1 (id 3) that the recording's Y
// must not land on.
InstrumentTable configured() {
  InstrumentTable t;
  REQUIRE(t.add(instrument("Y", VenueId{0})));
  REQUIRE(t.add(instrument("Z", VenueId{1})));
  REQUIRE(t.add(instrument("X", VenueId{1})));
  REQUIRE(t.add(instrument("Y", VenueId{1})));
  return t;
}

constexpr std::uint32_t kDeep = 300;  // a side: deeper than EventBuf's 256
constexpr std::uint32_t kDeepBytes = BookDeltaMsg::size_for(kDeep, kDeep);
static_assert(kDeepBytes > sim::kMaxSourceEventBytes);
static_assert(kDeepBytes <= kMaxMsgBytes);

class Writer {
 public:
  Writer(const std::string& path, const InstrumentTable& table) : ring_(1U << 22) {
    JournalSessionInfo info;
    info.session_id = 1;
    info.strategy = "journal_remap_test";
    info.instruments = &table;
    fw_ = std::make_unique<JournalFileWriter>(ring_, path, info);
    REQUIRE(fw_->ok());
  }
  ~Writer() {
    fw_->drain_once();
    fw_->stop();
  }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  void trade(std::uint32_t inst, std::uint64_t id) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{inst}, VenueId{0});
    t.price = px("100");
    t.qty = qt("1");
    t.trade_id = id;
    stamp(t.hdr);
  }
  void ticker(std::uint32_t inst) {
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, InstrumentId{inst}, VenueId{0});
    m.bid_px = px("99");
    m.bid_qty = qt("1");
    m.ask_px = px("101");
    m.ask_qty = qt("1");
    stamp(m.hdr);
  }
  // A snapshot of kDeep levels a side: larger than an EventBuf.
  void deep_snapshot(std::uint32_t inst) {
    std::vector<std::uint64_t> buf(kDeepBytes / 8);
    auto& d = *reinterpret_cast<BookDeltaMsg*>(buf.data());
    init_header(d, EventType::BookSnapshot, InstrumentId{inst}, VenueId{0}, kDeepBytes);
    d.hdr.flags |= EventHeader::kSnapshot;
    d.bid_count = kDeep;
    d.ask_count = kDeep;
    for (std::uint32_t k = 0; k < kDeep; ++k) {
      d.levels()[k] = Level{Price::from_raw(10'000 - static_cast<std::int64_t>(k)), qt("1")};
      d.levels()[kDeep + k] =
          Level{Price::from_raw(10'001 + static_cast<std::int64_t>(k)), qt("1")};
    }
    stamp(d.hdr);
  }

 private:
  void stamp(EventHeader& h) {
    h.recv_ts = h.exch_ts = Timestamp{now_};
    now_ += 1000;
    REQUIRE(w_.record(h));
  }

  std::int64_t now_ = 1'000'000'000;
  MsgRing ring_;
  std::unique_ptr<JournalFileWriter> fw_;
  JournalWriter w_{&ring_};
};

struct Seen {
  EventType type;
  std::uint32_t instrument;
  std::uint32_t venue;
  std::uint32_t len;
  bool operator==(const Seen&) const = default;
};

std::vector<Seen> drain(MdSource& s) {
  std::vector<Seen> out;
  while (const EventHeader* h = s.next()) {
    out.push_back(Seen{h->type, h->instrument.value, h->venue.value, h->len});
  }
  return out;
}

std::string fresh(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

}  // namespace

TEST_CASE("backtest.journal_remap: the events carry the configuration's ids and venues") {
  const std::string path = fresh("journal_remap.fmj");
  const InstrumentTable rec = recorded();
  const InstrumentTable cfg = configured();
  {
    Writer w(path, rec);
    w.trade(0, 1);       // X
    w.deep_snapshot(1);  // Y
    w.ticker(0);         // X
    w.trade(1, 2);       // Y
  }

  // As recorded: the recording's ids, venue 0.
  CHECK(drain(*std::make_unique<JournalSource>(path)) ==
        std::vector<Seen>{{EventType::Trade, 0, 0, sizeof(TradeMsg)},
                          {EventType::BookSnapshot, 1, 0, kDeepBytes},
                          {EventType::BookTicker, 0, 0, sizeof(BookTickerMsg)},
                          {EventType::Trade, 1, 0, sizeof(TradeMsg)}});

  // Remapped: X is the configuration's id 2 on venue 1, Y its id 0 on venue 0 (not the Y on
  // venue 1); the deep snapshot comes through whole.
  JournalSource src(path, false, true, &cfg);
  const std::vector<Seen> want{{EventType::Trade, 2, 1, sizeof(TradeMsg)},
                               {EventType::BookSnapshot, 0, 0, kDeepBytes},
                               {EventType::BookTicker, 2, 1, sizeof(BookTickerMsg)},
                               {EventType::Trade, 0, 0, sizeof(TradeMsg)}};
  src.next();
  const EventHeader* snap = src.next();
  REQUIRE(snap != nullptr);
  const auto& d = msg_cast<BookDeltaMsg>(snap);
  CHECK(d.bid_count == kDeep);
  CHECK(d.ask_count == kDeep);
  CHECK(d.bids()[kDeep - 1].price == Price::from_raw(10'000 - (kDeep - 1)));
  CHECK(d.asks()[kDeep - 1].price == Price::from_raw(10'001 + (kDeep - 1)));
  src.reset();
  CHECK(drain(src) == want);
  CHECK(src.dropped() == 0);
  CHECK(src.note() == "journal: remap: 2 of 2 instruments in the configuration, 0 events dropped");
  CHECK(src.md_events() == 4);

  // The same through the spec; without a configuration the option is refused.
  CHECK(drain(*open_data("journal:" + path + ",remap=1", &cfg)) == want);
  CHECK(drain(*open_data("journal:" + path + ",remap=0", &cfg)).front().instrument == 0);
  CHECK_THROWS_WITH_AS(static_cast<void>(open_data("journal:" + path + ",remap=1")),
                       doctest::Contains("remap needs the configuration's instruments"),
                       std::runtime_error);
}

TEST_CASE("backtest.journal_remap: a symbol the configuration lacks drops its events") {
  const std::string path = fresh("journal_remap_drop.fmj");
  InstrumentTable rec;
  REQUIRE(rec.add(instrument("X", VenueId{0})));
  REQUIRE(rec.add(instrument("Q", VenueId{0})));
  const InstrumentTable cfg = configured();
  {
    Writer w(path, rec);
    w.trade(0, 1);  // X
    w.trade(1, 2);  // Q: nowhere in the configuration
    w.ticker(0);    // X
  }
  JournalSource src(path, false, true, &cfg);
  CHECK(drain(src) == std::vector<Seen>{{EventType::Trade, 2, 1, sizeof(TradeMsg)},
                                        {EventType::BookTicker, 2, 1, sizeof(BookTickerMsg)}});
  CHECK(src.dropped() == 1);
  CHECK(src.note() == "journal: remap: 1 of 2 instruments in the configuration, 1 event dropped");
  // reset() starts the count over: a second pass does not count the drop twice.
  src.reset();
  CHECK(drain(src).size() == 2);
  CHECK(src.dropped() == 1);
}
