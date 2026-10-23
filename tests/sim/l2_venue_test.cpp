// The simulated venue over recorded L2 data: an order is checked against a book ticker newer than
// the throttled depth, whose levels on the near side of the ticker are stale; its replies on two
// connections (SimVenueConfig::fill_latency): fills and balances on the private stream's latency,
// acks and cancel acks on the order connection's, each in order on its own; and the latencies a
// live session's messages had (sim::RecordedLatency), taken where they are longer.
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

// Orders take 50 us to the venue, acks 50 us back; with `fill_us`, fills take that long back.
sim::SimTransportConfig config(std::int64_t fill_us) {
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  tc.queue_conservatism_bps = 0;
  tc.order_out = sim::LatencyParams{microseconds(50), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(50), Duration{}};
  tc.own_orders_in_feed = false;
  if (fill_us > 0) {
    tc.fill_latency = true;
    tc.fill_in = sim::LatencyParams{microseconds(fill_us), Duration{}};
  }
  return tc;
}

struct Inbound {
  EventType type;
  std::uint64_t id;
  Timestamp venue;
  Timestamp arrival;
  Price price{};  // a fill's
  Qty qty{};
  Liquidity liquidity = Liquidity::Unknown;
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
  // One level of one side, as a depth update at `us` (venue time).
  void level(std::int64_t us, Side side, const char* price, const char* qty) {
    alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 0)] = {};
    auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
    init_header(*d, EventType::BookDelta, kId, VenueId{0}, BookDeltaMsg::size_for(1, 0));
    d->hdr.exch_ts = at(us);
    d->hdr.recv_ts = at(us);
    d->last_update_id = ++update_id;
    d->bid_count = side == Side::Buy ? 1 : 0;
    d->ask_count = side == Side::Sell ? 1 : 0;
    d->levels()[0] = Level{px(price), qt(qty)};
    v.on_source_event(d->hdr);
  }
  void order(std::int64_t us,
             std::uint64_t id,
             Side side,
             const char* price,
             OrderType type = OrderType::PostOnly) {
    clock.set(at(us));
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, kId, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = side;
    m.type = type;
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
      Inbound in{h->type, 0, h->exch_ts, h->recv_ts};
      if (h->type == EventType::OrderFill) {
        const auto& f = msg_cast<OrderFillMsg>(h);
        id = f.cl_ord_id.value;
        in.price = f.price;
        in.qty = f.qty;
        in.liquidity = f.liquidity;
      }
      if (h->type == EventType::OrderCancelAck) id = msg_cast<OrderCancelAckMsg>(h).cl_ord_id.value;
      if (h->type == EventType::OrderReject) id = msg_cast<OrderRejectMsg>(h).cl_ord_id.value;
      if (h->type != EventType::BookDelta && h->type != EventType::BookSnapshot &&
          h->type != EventType::BookTicker && h->type != EventType::Trade)
        got.push_back(Inbound{h->type, id, in.venue, in.arrival, in.price, in.qty, in.liquidity});
      feed.release();
    }
  }
  // The fills of order `id`, in delivery order.
  [[nodiscard]] std::vector<Inbound> fills(std::uint64_t id) const {
    std::vector<Inbound> out;
    for (const Inbound& i : got) {
      if (i.type == EventType::OrderFill && i.id == id) out.push_back(i);
    }
    return out;
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

TEST_CASE("sim.l2_queue: liquidity that reaches a resting order trades with it, without a print") {
  Venue x(config(0));
  x.order(0, 1, Side::Buy, "100.00");  // rests alone at 100.00 from 50 us
  x.run(100);
  // Asks come down to the bid: with the order on the book they would have taken it. The recording
  // has no print for them, since without the order they rested.
  x.level(200, Side::Sell, "100.00", "0.04");
  x.run(300);
  auto f = x.fills(1);
  REQUIRE(f.size() == 1);
  CHECK(f[0].price == px("100.00"));
  CHECK(f[0].qty == qt("0.04"));
  CHECK(f[0].liquidity == Liquidity::Maker);
  CHECK(f[0].venue == at(200));
  // The same quantity shown again is the liquidity it took already.
  x.level(400, Side::Sell, "100.00", "0.04");
  x.run(500);
  CHECK(x.fills(1).size() == 1);
  // What joins it later trades too; a shrinking level gives nothing back.
  x.level(600, Side::Sell, "100.00", "0.02");
  x.level(700, Side::Sell, "100.00", "0.05");
  x.run(800);
  f = x.fills(1);
  REQUIRE(f.size() == 2);
  CHECK(f[1].qty == qt("0.03"));
  // A level through the price fills the rest, still at the order's price.
  x.level(900, Side::Sell, "99.50", "1");
  x.run(1000);
  f = x.fills(1);
  REQUIRE(f.size() == 3);
  CHECK(f[2].price == px("100.00"));
  CHECK(f[2].qty == qt("0.03"));
  CHECK(x.v.stats().cross_fills == 3);
  CHECK(x.v.queue().size() == 0);
}

TEST_CASE("sim.l2_queue: liquidity the order found on the book does not cross it later") {
  Venue x(config(0));
  x.order(0, 1, Side::Buy, "100.00");  // at the venue at 50 us
  x.run(100);
  // Recorded at 40 us, before the order arrived, and applied after it: the order did not see it
  // come in. Shown again later, it is still not new.
  x.level(40, Side::Sell, "100.00", "0.04");
  x.level(200, Side::Sell, "100.00", "0.04");
  x.run(300);
  CHECK(x.fills(1).empty());

  // A limit order through the ask takes it, and the rest rests: the ask it took is still in the
  // recording and is not taken twice; what joins it later is new.
  Venue y(config(0));
  y.level(10, Side::Sell, "101.00", "0.04");
  y.order(100, 2, Side::Buy, "101.00", OrderType::Limit);
  y.run(200);
  auto f = y.fills(2);
  REQUIRE(f.size() == 1);
  CHECK(f[0].qty == qt("0.04"));
  CHECK(f[0].liquidity == Liquidity::Taker);
  y.level(300, Side::Sell, "101.00", "0.04");
  y.run(400);
  CHECK(y.fills(2).size() == 1);
  y.level(500, Side::Sell, "101.00", "0.05");
  y.run(600);
  f = y.fills(2);
  REQUIRE(f.size() == 2);
  CHECK(f[1].qty == qt("0.01"));
  CHECK(f[1].liquidity == Liquidity::Maker);
  CHECK(f[1].price == px("101.00"));
}

TEST_CASE("sim.fill_path: fills take the private stream's latency, and hold up no ack") {
  for (const std::int64_t fill_us : {std::int64_t{0}, std::int64_t{5000}}) {
    CAPTURE(fill_us);
    Venue x(config(fill_us));
    x.order(0, 1, Side::Buy, "100.00");  // at the venue at 50 us
    x.run(100);
    x.trade(200, "100.00", "0.1");        // fills it at 200 us
    x.order(300, 2, Side::Buy, "99.50");  // acked at 350 us, back at 400 us
    x.run(10'000);
    const Inbound* fill = x.find(EventType::OrderFill, 1);
    const Inbound* ack2 = x.find(EventType::OrderAck, 2);
    REQUIRE(fill != nullptr);
    REQUIRE(ack2 != nullptr);
    CHECK(fill->venue == at(200));
    CHECK(ack2->arrival == at(400));
    if (fill_us == 0) {
      CHECK(fill->arrival == at(250));  // on the ack path, in order with the acks
    } else {
      CHECK(fill->arrival == at(5200));
      CHECK(ack2->arrival < fill->arrival);  // the ack does not wait behind the late fill
    }
  }
}

TEST_CASE("sim.fill_path: a cancel that reaches the venue after the fill is rejected first") {
  // The fill is late on the private stream: the cancel's reject (the order is gone) arrives first,
  // as a live venue's order API answers before its user stream reports the trade.
  Venue x(config(3000));
  x.order(0, 1, Side::Buy, "100.00");
  x.run(100);
  x.trade(200, "100.00", "0.1");
  x.cancel(210, 1);  // at the venue at 260 us: the order filled at 200 us
  x.run(10'000);
  REQUIRE(x.got.size() >= 3);
  const Inbound* fill = x.find(EventType::OrderFill, 1);
  REQUIRE(fill != nullptr);
  CHECK(fill->arrival == at(3200));
  CHECK(x.got.back().type == EventType::OrderFill);
}

TEST_CASE("sim.recorded_latency: the live message nearest in time, the same decision first") {
  using Path = sim::RecordedLatency::Path;
  using S = sim::RecordedLatency::Sample;
  const sim::RecordedLatency r({S{at(1000).ns, 600'000, 0, VenueId{0}, Path::Cancel},
                                S{at(1010).ns, 40'000'000, 7, VenueId{0}, Path::Cancel},
                                S{at(5000).ns, 2'000'000, 0, VenueId{1}, Path::Cancel},
                                S{at(9000).ns, 90'000'000, 0, VenueId{0}, Path::Ack}});
  CHECK(r.size() == 4);
  // The same instrument within 2 ms wins over a nearer one of another.
  CHECK(r.out(Path::Cancel, VenueId{0}, 7, at(1001)) == milliseconds(40));
  CHECK(r.out(Path::Cancel, VenueId{0}, 3, at(1001)) == microseconds(600));
  // Only another account's, or none within 20 ms: nothing.
  CHECK(r.out(Path::Cancel, VenueId{2}, 0, at(5000)) == Duration{});
  CHECK(r.out(Path::Cancel, VenueId{1}, 0, at(5000)) == milliseconds(2));
  CHECK(r.out(Path::Cancel, VenueId{0}, 3, at(40'000)) == Duration{});
  CHECK(r.out(Path::New, VenueId{0}, 0, at(1000)) == Duration{});
  // A fill takes an ack's when no fill was recorded near it.
  CHECK(r.in(Path::Fill, VenueId{0}, at(9500)) == milliseconds(90));
  CHECK(r.in(Path::Ack, VenueId{0}, at(40'000)) == Duration{});
}

TEST_CASE("sim.recorded_latency: a cancel as slow as live's loses the race live lost") {
  using Path = sim::RecordedLatency::Path;
  using S = sim::RecordedLatency::Sample;
  // Live, the cancel sent at 210 us took 30 ms to reach the book (a burst).
  const sim::RecordedLatency slow({S{at(210).ns, 30'000'000, 0, VenueId{0}, Path::Cancel}});
  for (const bool recorded : {false, true}) {
    CAPTURE(recorded);
    Venue x(config(0));
    if (recorded) x.v.set_recorded_latency(&slow);
    x.order(0, 1, Side::Buy, "100.00");
    x.run(100);
    x.cancel(210, 1);  // the model: at the venue at 260 us
    x.run(4000);
    x.trade(5000, "100.00", "0.1");  // hits the bid at 5 ms
    x.run(50'000);
    CHECK((x.find(EventType::OrderFill, 1) != nullptr) == recorded);
    CHECK((x.find(EventType::OrderCancelAck, 1) != nullptr) == !recorded);
  }
}
