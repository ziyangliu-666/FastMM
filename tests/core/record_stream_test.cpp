// The engine's record stream: what a storage backend receives, and that nothing about it can
// stall or fail the engine (core/record_stream.hpp).
#include "fastmm/core/record_stream.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  i.min_notional = Notional::from_decimal("0.5").value();
  REQUIRE(t.add(i));
  return t;
}

struct FakeTransport {
  std::vector<std::vector<std::byte>> out;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
};

using TestEngine = Engine<BasicMM, SimClock, FakeTransport, InlineFeed>;

// Drains the record ring into a list of (type, copy) pairs.
struct Records {
  std::vector<std::pair<RecordType, std::vector<std::byte>>> all;

  void drain(MsgRing& ring) {
    while (const std::byte* p = ring.try_peek()) {
      const auto* h = reinterpret_cast<const RecordHeader*>(p);
      all.emplace_back(h->type, std::vector<std::byte>(p, p + h->len));
      ring.release();
    }
  }
  [[nodiscard]] std::size_t count(RecordType t) const {
    std::size_t n = 0;
    for (const auto& [type, bytes] : all) n += type == t ? 1U : 0U;
    return n;
  }
  template <class R>
  [[nodiscard]] const R& first(RecordType t) const {
    for (const auto& [type, bytes] : all) {
      if (type == t) return *reinterpret_cast<const R*>(bytes.data());
    }
    REQUIRE_MESSAGE(false, "no record of that type");
    return *reinterpret_cast<const R*>(all.front().second.data());
  }
};

struct Fixture {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  FakeTransport transport;
  InlineFeed feed{1 << 20};
  MsgRing record_ring{1 << 18};
  BasicMM strategy;
  std::unique_ptr<TestEngine> engine;

  Fixture() {
    REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "10"},
                                      {"quote_qty", "0.01"},
                                      {"max_inventory", "0.05"},
                                      {"skew_bps_per_unit", "2"},
                                      {"requote_threshold_ticks", "1"},
                                      {"pull_on_stale_ms", "500"}}));
    EngineConfig cfg;
    cfg.session_id = 4242;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_position = qt("1");
    cfg.risk.max_open_orders = 8;
    cfg.risk.stale_md = seconds(5);
    cfg.risk.price_collar_bps = 500;
    cfg.quotes.min_requote_interval = Duration{};
    engine = std::make_unique<TestEngine>(
        cfg, table, clock, transport, feed, strategy, nullptr, &record_ring);
    engine->warm_up();
    engine->start();
  }

  void push_book(const char* bid, const char* ask, std::uint64_t seq) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(
        *d, EventType::BookSnapshot, InstrumentId{0}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->last_update_id = seq;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
  }
  void drain() {
    while (engine->step() != 0) {
    }
  }
  ClientOrderId first_order() {
    for (const auto& bytes : transport.out) {
      const auto* h = reinterpret_cast<const EventHeader*>(bytes.data());
      if (h->type == EventType::OutNewOrder)
        return reinterpret_cast<const OutNewOrderMsg*>(bytes.data())->cl_ord_id;
    }
    return ClientOrderId{};
  }
};

}  // namespace

TEST_CASE("core.records: a fill produces a fill record and the position it left behind") {
  Fixture f;
  f.push_book("100.00", "100.02", 1);
  f.drain();
  const ClientOrderId id = f.first_order();
  REQUIRE(id.valid());

  OrderAckMsg a{};
  init_header(a, EventType::OrderAck, InstrumentId{0}, VenueId{0});
  a.cl_ord_id = id;
  a.venue_order_id = "V42";
  f.push(a);
  f.drain();

  OrderFillMsg fill{};
  init_header(fill, EventType::OrderFill, InstrumentId{0}, VenueId{0});
  fill.cl_ord_id = id;
  fill.side = Side::Buy;
  fill.price = px("100.00");
  fill.qty = qt("0.01");
  fill.cum_qty = qt("0.01");
  fill.fee = Notional::from_decimal("0.001").value();
  fill.fee_asset = FeeAsset::Quote;
  fill.liquidity = Liquidity::Maker;
  fill.exec_id = "E1";
  fill.venue_order_id = "V42";
  f.push(fill);
  f.drain();

  Records r;
  r.drain(f.record_ring);
  REQUIRE(r.count(RecordType::Fill) == 1);
  const FillRecord& fr = r.first<FillRecord>(RecordType::Fill);
  CHECK(fr.hdr.session_id == 4242);
  CHECK(fr.hdr.instrument.value == 0);
  CHECK(fr.hdr.seq != 0);
  CHECK(fr.side == Side::Buy);
  CHECK(fr.price == px("100.00"));
  CHECK(fr.qty == qt("0.01"));
  CHECK(fr.booked_qty == qt("0.01"));
  CHECK(fr.fee == Notional::from_decimal("0.001").value());
  CHECK(fr.fee_asset == FeeAsset::Quote);
  CHECK(fr.liquidity == Liquidity::Maker);
  CHECK(fr.exec_id.view() == "E1");
  CHECK(fr.venue_order_id.view() == "V42");
  CHECK(fr.position_qty == qt("0.01"));
  // Every fill is followed by the position it left behind.
  REQUIRE(r.count(RecordType::Position) >= 1);
  const PositionRecord& pr = r.first<PositionRecord>(RecordType::Position);
  CHECK(pr.pos.qty == qt("0.01"));
  CHECK(pr.pos.fills == 1);
  CHECK(pr.total_fees == Notional::from_decimal("0.001").value());
  // And an order record for each state change of that order.
  CHECK(r.count(RecordType::Order) >= 2);
  const OrderRecord& orr = r.first<OrderRecord>(RecordType::Order);
  CHECK(orr.order.cl_ord_id == id);
  CHECK(orr.hdr.aux[0] == static_cast<std::uint8_t>(OrderState::PendingNew));
}

TEST_CASE("core.records: a kill switch trip is recorded with the reason and the PnL") {
  Fixture f;
  f.push_book("100.00", "100.02", 1);
  f.drain();
  ControlMsg c{};
  init_header(c, EventType::Control, InstrumentId{}, VenueId{0});
  c.command = ControlCommand::TripKill;
  f.push(c);
  f.drain();

  Records r;
  r.drain(f.record_ring);
  REQUIRE(r.count(RecordType::Kill) == 1);
  const KillRecord& kr = r.first<KillRecord>(RecordType::Kill);
  CHECK(kr.reason == KillReason::Requested);
  CHECK((kr.hdr.flags & RecordHeader::kVenue) == 0);
  CHECK(kr.kills == 1);
}

TEST_CASE("core.records: finish writes the last position of every instrument that traded") {
  Fixture f;
  f.push_book("100.00", "100.02", 1);
  f.drain();
  const ClientOrderId id = f.first_order();
  REQUIRE(id.valid());
  OrderFillMsg fill{};
  init_header(fill, EventType::OrderFill, InstrumentId{0}, VenueId{0});
  fill.cl_ord_id = id;
  fill.side = Side::Buy;
  fill.price = px("100.00");
  fill.qty = qt("0.01");
  fill.cum_qty = qt("0.01");
  fill.exec_id = "E1";
  f.push(fill);
  f.drain();
  Records before;
  before.drain(f.record_ring);
  const std::size_t positions_before = before.count(RecordType::Position);

  f.engine->finish();
  Records after;
  after.drain(f.record_ring);
  CHECK(after.count(RecordType::Position) == 1);
  CHECK(positions_before >= 1);
}

TEST_CASE("core.records: a session without a record ring emits nothing and counts nothing") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  FakeTransport transport;
  InlineFeed feed{1 << 20};
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "10"},
                                    {"quote_qty", "0.01"},
                                    {"max_inventory", "0.05"},
                                    {"skew_bps_per_unit", "2"},
                                    {"requote_threshold_ticks", "1"},
                                    {"pull_on_stale_ms", "500"}}));
  EngineConfig cfg;
  cfg.risk.max_order_qty = qt("1");
  cfg.risk.max_position = qt("1");
  cfg.risk.max_open_orders = 8;
  TestEngine engine(cfg, table, clock, transport, feed, strategy, nullptr, nullptr);
  engine.warm_up();
  engine.start();
  CHECK_FALSE(engine.records().enabled());
  CHECK(engine.stats().records_written == 0);
  CHECK(engine.stats().records_dropped == 0);
}

TEST_CASE("core.records: record layouts are a multiple of the ring granule") {
  CHECK(sizeof(RecordHeader) % kRingMsgGranule == 0);
  CHECK(sizeof(FillRecord) % kRingMsgGranule == 0);
  CHECK(sizeof(OrderRecord) % kRingMsgGranule == 0);
  CHECK(sizeof(PositionRecord) % kRingMsgGranule == 0);
  CHECK(sizeof(KillRecord) % kRingMsgGranule == 0);
  // Type 0 is the ring's padding marker, so no record may use it.
  CHECK(static_cast<std::uint8_t>(RecordType::Fill) != kRingPaddingType);
  CHECK(to_string(RecordType::Position) == "Position");
}
