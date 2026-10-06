// The simulated venue's order intake: a cancel's own latency (latency_cancel_us) and one
// connection's messages taken in one after another (order_service_us).
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

// Orders take 50 us to the venue and back, no jitter.
sim::SimTransportConfig config() {
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  tc.order_out = sim::LatencyParams{microseconds(50), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(50), Duration{}};
  tc.own_orders_in_feed = false;
  return tc;
}

struct Venue {
  explicit Venue(const sim::SimTransportConfig& c) : v(clock, instruments, c) {
    alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)] = {};
    auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
    init_header(*d, EventType::BookSnapshot, kId, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.exch_ts = at(0);
    d->hdr.recv_ts = at(0);
    d->bid_count = 1;
    d->ask_count = 1;
    d->levels()[0] = Level{px("99.00"), qt("1")};
    d->levels()[1] = Level{px("101.00"), qt("1")};
    v.on_source_event(d->hdr);
  }

  // Sends at `us`.
  void bid(std::int64_t us, std::uint64_t id, const char* price) {
    clock.set(at(us));
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, kId, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = Side::Buy;
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
  // Processes the venue's events up to `until_us`, recording the order events' venue times.
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
      if (h->type == EventType::OrderAck) acks.push_back(h->exch_ts);
      if (h->type == EventType::OrderCancelAck) cancels.push_back(h->exch_ts);
      if (h->type == EventType::OrderFill)
        filled.push_back(reinterpret_cast<const OrderFillMsg*>(h)->cl_ord_id.value);
      feed.release();
    }
  }

  InstrumentTable instruments = table();
  SimClock clock{at(0)};
  InlineFeed feed{1 << 20};
  sim::SimTransport v;
  std::vector<Timestamp> acks;
  std::vector<Timestamp> cancels;
  std::vector<std::uint64_t> filled;
};

}  // namespace

TEST_CASE("sim.order_service: one connection's messages are taken in one after another") {
  sim::SimTransportConfig c = config();
  c.order_service = microseconds(100);
  Venue x(c);
  x.bid(0, 1, "99.50");
  x.bid(0, 2, "99.40");
  x.bid(0, 3, "99.30");
  x.bid(1000, 4, "99.20");  // long after: its own latency only
  x.run(5000);
  REQUIRE(x.acks.size() == 4);
  CHECK(x.acks[0] == at(50));
  CHECK(x.acks[1] == at(150));
  CHECK(x.acks[2] == at(250));
  CHECK(x.acks[3] == at(1050));
}

TEST_CASE("sim.order_service: zero leaves every message on its own latency") {
  Venue x(config());
  x.bid(0, 1, "99.50");
  x.bid(0, 2, "99.40");
  x.bid(0, 3, "99.30");
  x.run(5000);
  REQUIRE(x.acks.size() == 3);
  for (const Timestamp t : x.acks) CHECK(t == at(50));
}

TEST_CASE("sim.order_service: the last cancel of a burst loses the race to a trade") {
  for (const std::int64_t service_us : {0, 100}) {
    CAPTURE(service_us);
    sim::SimTransportConfig c = config();
    c.order_service = microseconds(service_us);
    Venue x(c);
    // Three bids alone at their prices, nothing ahead.
    x.bid(0, 1, "99.70");
    x.bid(0, 2, "99.60");
    x.bid(0, 3, "99.50");
    x.run(1000);
    REQUIRE(x.acks.size() == 3);
    x.cancel(2000, 1);
    x.cancel(2000, 2);
    x.cancel(2000, 3);
    x.run(2200);
    // A sell through all three at 2200 us: with a service time the third cancel is taken in at
    // 2250 us, after it.
    x.trade(2200, "99.40", "1");
    x.run(5000);
    if (service_us == 0) {
      CHECK(x.filled.empty());
      CHECK(x.cancels.size() == 3);
    } else {
      REQUIRE(x.filled.size() == 1);
      CHECK(x.filled[0] == 3);
      CHECK(x.cancels.size() == 2);  // the third finds the order filled
    }
  }
}

TEST_CASE("sim.order_service: a cancel takes its own latency when one is given") {
  sim::SimTransportConfig c = config();
  c.order_out = sim::LatencyParams{microseconds(200), Duration{}};
  c.cancel_latency = true;
  c.cancel_out = sim::LatencyParams{microseconds(40), Duration{}};
  Venue x(c);
  x.bid(0, 1, "99.50");
  x.run(1000);
  REQUIRE(x.acks.size() == 1);
  CHECK(x.acks[0] == at(200));
  x.cancel(1000, 1);
  x.run(5000);
  REQUIRE(x.cancels.size() == 1);
  CHECK(x.cancels[0] == at(1040));
}
