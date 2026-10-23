// The simulated venue over recorded L2 data: an order is checked against a book ticker newer than
// the throttled depth, whose levels on the near side of the ticker are stale.
#include "test_support.hpp"

#include "fastmm/sim/sim_transport.hpp"

#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

constexpr std::int64_t kT0 = 1'700'000'000'000'000'000;
constexpr InstrumentId kId{0};
Timestamp at(std::int64_t us) {
  return Timestamp{kT0 + us * 1000};
}

InstrumentTable table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  REQUIRE(t.add(i));
  return t;
}

// Orders take 50 us to the venue, acks 50 us back.
sim::SimTransportConfig config(std::int64_t = 0) {
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  tc.queue_conservatism_bps = 0;
  tc.order_out = sim::LatencyParams{microseconds(50), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(50), Duration{}};
  tc.own_orders_in_feed = false;
  return tc;
}

struct Inbound {
  EventType type;
  std::uint64_t id;
  Timestamp venue;
  Timestamp arrival;
};

struct Venue {
  explicit Venue(const sim::SimTransportConfig& c) : v(clock, instruments, c) {
    book(0, true, "99.00", "101.00");
  }

  void book(std::int64_t us, bool snapshot, const char* bid, const char* ask) {
    alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)] = {};
    auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
    init_header(*d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                kId,
                VenueId{0},
                BookDeltaMsg::size_for(1, 1));
    if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.exch_ts = at(us);
    d->hdr.recv_ts = at(us);
    d->last_update_id = ++update_id;
    d->bid_count = 1;
    d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("1")};
    d->levels()[1] = Level{px(ask), qt("1")};
    v.on_source_event(d->hdr);
  }
  void ticker(std::int64_t us, const char* bid, const char* ask) {
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, kId, VenueId{0});
    m.hdr.exch_ts = at(us);
    m.hdr.recv_ts = at(us);
    m.hdr.venue_seq = ++update_id;
    m.bid_px = px(bid);
    m.bid_qty = qt("1");
    m.ask_px = px(ask);
    m.ask_qty = qt("1");
    v.on_source_event(m.hdr);
  }
  void order(std::int64_t us, std::uint64_t id, Side side, const char* price) {
    clock.set(at(us));
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, kId, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = side;
    m.type = OrderType::PostOnly;
    m.tif = TimeInForce::Gtc;
    m.price = px(price);
    m.qty = qt("0.1");
    REQUIRE(v.send(m.hdr));
  }
  void cancel(std::int64_t us, std::uint64_t id) {
    clock.set(at(us));
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, kId, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    REQUIRE(v.send(m.hdr));
  }
  void trade(std::int64_t us, const char* price, const char* qty) {
    TradeMsg m{};
    init_header(m, EventType::Trade, kId, VenueId{0});
    m.hdr.exch_ts = at(us);
    m.hdr.recv_ts = at(us);
    m.price = px(price);
    m.qty = qt(qty);
    m.aggressor = Side::Sell;
    v.on_source_event(m.hdr);
  }
  // Everything the venue sends the engine up to `until_us`, in delivery order.
  void run(std::int64_t until_us) {
    for (;;) {
      const Timestamp a = v.next_order_arrival();
      const Timestamp b = v.next_inbound_ts();
      const Timestamp t = a < b ? a : b;
      if (t > at(until_us)) break;
      if (t > clock.now()) clock.set(t);
      if (t == a) {
        v.process_order_arrival();
        continue;
      }
      static_cast<void>(v.deliver_next_inbound(feed));
      const EventHeader* h = feed.next();
      REQUIRE(h != nullptr);
      std::uint64_t id = 0;
      if (h->type == EventType::OrderAck) id = msg_cast<OrderAckMsg>(h).cl_ord_id.value;
      if (h->type == EventType::OrderFill) id = msg_cast<OrderFillMsg>(h).cl_ord_id.value;
      if (h->type == EventType::OrderCancelAck) id = msg_cast<OrderCancelAckMsg>(h).cl_ord_id.value;
      if (h->type == EventType::OrderReject) id = msg_cast<OrderRejectMsg>(h).cl_ord_id.value;
      if (h->type != EventType::BookDelta && h->type != EventType::BookSnapshot &&
          h->type != EventType::BookTicker && h->type != EventType::Trade)
        got.push_back(Inbound{h->type, id, h->exch_ts, h->recv_ts});
      feed.release();
    }
  }
  [[nodiscard]] const Inbound* find(EventType t, std::uint64_t id) const {
    for (const Inbound& i : got) {
      if (i.type == t && i.id == id) return &i;
    }
    return nullptr;
  }

  InstrumentTable instruments = table();
  SimClock clock{at(0)};
  InlineFeed feed{1 << 20};
  sim::SimTransport v;
  std::uint64_t update_id = 0;
  std::vector<Inbound> got;
};

}  // namespace

TEST_CASE("sim.l2_queue: a post-only order is checked against a ticker newer than the depth") {
  Venue x(config(0));
  // The price ran up: the depth (throttled) still shows the ask at 101.00, the ticker 101.50.
  x.ticker(100, "101.20", "101.50");
  x.order(200, 1, Side::Buy, "101.30");  // inside the ticker's spread: rests
  x.order(210, 2, Side::Buy, "101.50");  // at the ticker's ask: would take
  x.run(1000);
  CHECK(x.find(EventType::OrderAck, 1) != nullptr);
  CHECK(x.find(EventType::OrderReject, 1) == nullptr);
  CHECK(x.find(EventType::OrderReject, 2) != nullptr);
}

TEST_CASE("sim.l2_queue: a depth level on the near side of the ticker is stale") {
  Venue x(config(0));
  x.ticker(100, "99.80", "100.20");
  // A later depth update that does not remove the ask the price has left (a missed removal): its
  // best ask 99.90 is below the ticker's best bid.
  x.book(150, false, "99.00", "99.90");
  x.order(200, 1, Side::Buy, "100.00");  // below the ticker's ask: rests
  x.run(1000);
  CHECK(x.find(EventType::OrderAck, 1) != nullptr);
  CHECK(x.find(EventType::OrderReject, 1) == nullptr);
}
