// A hazard group on the hot path: quote fills and a print on its two members take the score to the
// threshold (the two fills of a round together, or the first with what is left of the round
// before), the engine pulls the side, requotes meet the hold, and the hold's end puts the quotes
// back, all without an allocation.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

using namespace fastmm;
using fastmm::test::NoAllocScope;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// Keeps the last new order of each instrument and side, in fixed storage.
struct LastOrders {
  struct Sent {
    ClientOrderId id{};
    Price price{};
  };
  std::array<std::array<Sent, 2>, 2> last{};
  std::array<ClientOrderId, 16> cancelled{};  // not yet acknowledged
  std::size_t open_cancels = 0;
  std::size_t news = 0;
  std::size_t cancels = 0;
  bool send(const EventHeader& m) noexcept {
    if (m.type == EventType::OutNewOrder) {
      const auto& n = msg_cast<OutNewOrderMsg>(&m);
      last[m.instrument.value][static_cast<std::size_t>(n.side)] = {n.cl_ord_id, n.price};
      ++news;
    } else if (m.type == EventType::OutCancel) {
      if (open_cancels < cancelled.size())
        cancelled[open_cancels++] = msg_cast<OutCancelMsg>(&m).cl_ord_id;
      ++cancels;
    }
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> b) noexcept {
    for (const EventHeader* m : b) static_cast<void>(send(*m));
    return b.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
};

}  // namespace

TEST_CASE("hotpath.noalloc: engine step with a hazard group engaging and lifting") {
  InstrumentTable table;
  for (const char* symbol : {"BTCUSDT", "ETHUSDT"}) {
    Instrument i{};
    i.symbol = symbol;
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.min_qty = qt("0.001");
    REQUIRE(table.add(i));
  }
  SimClock clock{Timestamp{seconds(1000).ns}};
  LastOrders transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure(
      {{"half_spread_bps", "10"}, {"quote_qty", "0.01"}, {"max_inventory", "1"}}));
  EngineConfig cfg;
  cfg.risk.max_order_qty = qt("1");
  cfg.risk.max_open_orders = 8;
  cfg.quotes.min_requote_interval = Duration{};
  // A fill of 0.001 is about 0.1 (instrument 0) or 0.05 (instrument 1) of notional.
  HazardGroupConfig group;
  group.name = "pair";
  group.members = {InstrumentId{0}, InstrumentId{1}};
  group.threshold = 0.105;
  group.half_life = milliseconds(20);
  group.hold = milliseconds(50);
  cfg.hazard.groups.push_back(group);
  using E = Engine<BasicMM, SimClock, LastOrders, InlineFeed>;
  auto engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, &journal_ring);
  engine->warm_up();
  engine->start();
  std::uint64_t seq = 0;
  auto push_book = [&](std::uint16_t inst, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(
        *d, EventType::BookSnapshot, InstrumentId{inst}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->hdr.t0_cycles = clock.cycles();
    d->bid_count = d->ask_count = 1;
    d->last_update_id = ++seq;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
  };
  auto ack = [&](std::uint16_t inst, Side side) {
    OrderAckMsg a{};
    init_header(a, EventType::OrderAck, InstrumentId{inst}, VenueId{0});
    a.cl_ord_id = transport.last[inst][static_cast<std::size_t>(side)].id;
    a.venue_order_id = "V";
    a.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(a.hdr));
  };
  std::uint64_t exec = 0;
  auto fill = [&](std::uint16_t inst, Side side) {
    const LastOrders::Sent& s = transport.last[inst][static_cast<std::size_t>(side)];
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, InstrumentId{inst}, VenueId{0});
    f.cl_ord_id = s.id;
    f.exec_id.assign("e");
    f.exec_id.push_back(static_cast<char>('a' + exec++ % 26));
    f.side = side;
    f.price = s.price;
    f.qty = qt("0.001");
    f.cum_qty = qt("0.001");
    f.liquidity = Liquidity::Maker;
    f.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(f.hdr));
  };
  auto print = [&](std::uint16_t inst, const char* price) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{inst}, VenueId{0});
    t.price = px(price);
    t.qty = qt("0.0001");
    t.aggressor = Side::Sell;
    t.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(t.hdr));
  };
  auto cancel_acks = [&] {
    for (std::size_t i = 0; i < transport.open_cancels; ++i) {
      OrderCancelAckMsg a{};
      init_header(a, EventType::OrderCancelAck, InstrumentId{0}, VenueId{0});
      a.cl_ord_id = transport.cancelled[i];
      a.hdr.recv_ts = clock.now();
      REQUIRE(feed.push(a.hdr));
    }
    transport.open_cancels = 0;
  };
  auto drain = [&] {
    while (engine->step() > 0) {
    }
  };
  push_book(0, "100.00", "100.02");
  push_book(1, "50.00", "50.02");
  drain();
  for (std::uint16_t k = 0; k < 2; ++k) {
    ack(k, Side::Buy);
    ack(k, Side::Sell);
  }
  drain();
  REQUIRE(transport.news == 4);
  {
    NoAllocScope guard(true);
    for (int round = 0; round < 3; ++round) {
      fill(0, Side::Buy);
      drain();
      clock.advance(milliseconds(10));
      print(1, "1.00");    // a print through our bid, ahead of its fill
      fill(1, Side::Buy);  // the second member: the bids are pulled by now
      drain();
      cancel_acks();
      drain();
      push_book(0, "100.01", "100.03");  // a requote during the hold places no bid
      drain();
      clock.advance(milliseconds(60));
      push_book(1, "50.01", "50.03");  // the hold has run out: the bids come back
      drain();
      for (std::uint16_t k = 0; k < 2; ++k) {
        ack(k, Side::Buy);
        ack(k, Side::Sell);
      }
      drain();
    }
  }
  const HazardView v = engine->hazard(0);
  CHECK(v.engagements[0] == 3);
  CHECK_FALSE(v.held[0]);
  CHECK(transport.cancels >= 6);
  CHECK(transport.news >= 4 + 6);  // each round puts both bids back
}
