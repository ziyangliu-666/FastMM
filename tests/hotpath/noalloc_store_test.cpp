// The store costs the engine thread a memcpy into a ring and nothing else: no allocation, no
// syscall, no wait (core/record_stream.hpp).
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>
#include <span>
#include <vector>

using namespace fastmm;
using fastmm::test::NoAllocScope;

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
  i.id = InstrumentId{0};
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

struct NullTransport {
  std::size_t sent = 0;
  bool send(const EventHeader&) noexcept {
    ++sent;
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> b) noexcept {
    sent += b.size();
    return b.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
};

}  // namespace

TEST_CASE("hotpath.noalloc: RecordWriter writes records without allocating") {
  auto ring = std::make_unique<MsgRing>(1 << 20);
  RecordWriter w(ring.get(), 1);
  FillRecord fill{};
  PositionRecord pos{};
  OrderRecord order{};
  KillRecord kill{};
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 500; ++i) {
      w.init(fill, RecordType::Fill, InstrumentId{0}, VenueId{0}, Timestamp{i}, Timestamp{i});
      fill.price = px("100");
      fill.qty = qt("1");
      fill.exec_id = "E";
      static_cast<void>(w.put(fill.hdr));
      w.init(pos, RecordType::Position, InstrumentId{0}, VenueId{0}, Timestamp{i}, Timestamp{i});
      static_cast<void>(w.put(pos.hdr));
      w.init(order, RecordType::Order, InstrumentId{0}, VenueId{0}, Timestamp{i}, Timestamp{i});
      static_cast<void>(w.put(order.hdr));
      w.init(kill, RecordType::Kill, InstrumentId{}, VenueId{0}, Timestamp{i}, Timestamp{i});
      static_cast<void>(w.put(kill.hdr));
      // Keep the ring from filling: a drop is counted, never allocated for.
      while (ring->try_peek() != nullptr) ring->release();
    }
  }
  CHECK(w.written() == 2000);
  CHECK(w.dropped() == 0);
}

TEST_CASE("hotpath.noalloc: a full record ring drops without allocating") {
  auto ring = std::make_unique<MsgRing>(1 << 12);
  RecordWriter w(ring.get(), 1);
  FillRecord fill{};
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 1000; ++i) {
      w.init(fill, RecordType::Fill, InstrumentId{0}, VenueId{0}, Timestamp{i}, Timestamp{i});
      static_cast<void>(w.put(fill.hdr));
    }
  }
  CHECK(w.dropped() > 0);
}

TEST_CASE("hotpath.noalloc: an engine with a record ring does not allocate on a fill") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  NullTransport transport;
  InlineFeed feed(1 << 20);
  auto records = std::make_unique<MsgRing>(1 << 20);
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "10"},
                                    {"quote_qty", "0.01"},
                                    {"max_inventory", "0.5"},
                                    {"skew_bps_per_unit", "2"},
                                    {"requote_threshold_ticks", "1"},
                                    {"pull_on_stale_ms", "500"}}));
  EngineConfig cfg;
  cfg.session_id = 1;
  cfg.risk.max_order_qty = qt("1");
  cfg.risk.max_position = qt("5");
  cfg.risk.max_open_orders = 8;
  cfg.risk.stale_md = seconds(5);
  cfg.quotes.min_requote_interval = Duration{};
  using E = Engine<BasicMM, SimClock, NullTransport, InlineFeed>;
  auto engine =
      std::make_unique<E>(cfg, table, clock, transport, feed, strategy, nullptr, records.get());
  engine->warm_up();
  engine->start();
  REQUIRE(engine->records().enabled());

  // One book update and an ack, so there is a live order to fill.
  std::vector<std::byte> book(BookDeltaMsg::size_for(1, 1));
  auto* d = reinterpret_cast<BookDeltaMsg*>(book.data());
  init_header(*d, EventType::BookDelta, InstrumentId{0}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
  d->hdr.recv_ts = clock.now();
  d->bid_count = d->ask_count = 1;
  d->last_update_id = 1;
  d->levels()[0] = Level{px("100.00"), qt("5")};
  d->levels()[1] = Level{px("100.02"), qt("5")};
  REQUIRE(feed.push(d->hdr));
  while (engine->step() != 0) {
  }

  OrderFillMsg fill{};
  init_header(fill, EventType::OrderFill, InstrumentId{0}, VenueId{0});
  fill.side = Side::Buy;
  fill.price = px("100.00");
  fill.qty = qt("0.001");
  fill.cum_qty = qt("0.001");
  fill.liquidity = Liquidity::Maker;
  fill.exec_id = "E";
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 200; ++i) {
      fill.hdr.recv_ts = clock.now();
      if (!feed.push(fill.hdr)) break;
      while (engine->step() != 0) {
      }
      while (records->try_peek() != nullptr) records->release();
    }
  }
  CHECK(engine->stats().records_written > 0);
  CHECK(engine->stats().records_dropped == 0);
}
