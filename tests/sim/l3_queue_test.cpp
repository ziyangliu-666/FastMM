// fill_model = "l3_queue": the simulated venue keeps the recorded orders of an order-by-order feed
// and puts ours in its level's FIFO between the ones that came before and after it.
#include "test_support.hpp"

#include "fastmm/core/queue_model.hpp"
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
  i.symbol = "AAPL";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("1");
  i.min_qty = qt("1");
  REQUIRE(t.add(i));
  return t;
}

sim::SimTransportConfig config(sim::FillModel fm = sim::FillModel::L3Queue) {
  sim::SimTransportConfig tc;
  tc.fill_model = fm;
  tc.queue_conservatism_bps = 0;  // l3_queue ignores it
  tc.order_out = sim::LatencyParams{microseconds(50), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(50), Duration{}};
  tc.own_orders_in_feed = false;
  return tc;
}

struct Venue {
  explicit Venue(const sim::SimTransportConfig& c) : v(clock, instruments, c) {}

  void add(std::int64_t us, std::uint64_t ref, Side side, const char* price, const char* qty) {
    OrderAddL3Msg m{};
    init_header(m, EventType::OrderAddL3, kId, VenueId{0});
    stamp(m.hdr, us);
    m.order_ref = ref;
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
    v.on_source_event(m.hdr);
  }
  void exec(std::int64_t us, std::uint64_t ref, const char* qty, const char* exec_price = "0") {
    OrderExecL3Msg m{};
    init_header(m, EventType::OrderExecL3, kId, VenueId{0});
    stamp(m.hdr, us);
    m.order_ref = ref;
    m.exec_qty = qt(qty);
    m.exec_price = px(exec_price);
    v.on_source_event(m.hdr);
  }
  void cancel(std::int64_t us, std::uint64_t ref, const char* qty = "0") {
    OrderCancelL3Msg m{};
    init_header(m, EventType::OrderCancelL3, kId, VenueId{0});
    stamp(m.hdr, us);
    m.order_ref = ref;
    m.canceled_qty = qt(qty);
    v.on_source_event(m.hdr);
  }
  void bid(std::int64_t us, std::uint64_t id, const char* price, const char* qty) {
    clock.set(at(us));
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, kId, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = Side::Buy;
    m.type = OrderType::PostOnly;
    m.tif = TimeInForce::Gtc;
    m.price = px(price);
    m.qty = qt(qty);
    REQUIRE(v.send(m.hdr));
  }
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
      if (h->type == EventType::OrderAck) ++acks;
      if (h->type == EventType::OrderFill) filled += reinterpret_cast<const OrderFillMsg*>(h)->qty;
      feed.release();
    }
    if (clock.now() < at(until_us)) clock.set(at(until_us));
  }

  InstrumentTable instruments = table();
  SimClock clock{at(0)};
  InlineFeed feed{1 << 20};
  sim::SimTransport v;
  int acks = 0;
  Qty filled{};

 private:
  static void stamp(EventHeader& h, std::int64_t us) {
    h.exch_ts = at(us);
    h.recv_ts = at(us);
  }
};

}  // namespace

TEST_CASE("queue.l3: what an execution of a recorded order means for ours") {
  using enum L3Reach;
  const Price p = px("100.00");
  // Ours is a bid at 100.00, fifth in the arrival sequence.
  CHECK(l3_execution_reaches(Side::Buy, p, 5, p, 3, p) == Ahead);  // one in front of us executes
  CHECK(l3_execution_reaches(Side::Buy, p, 5, p, 7, p) == Fills);  // one behind us: we came first
  CHECK(l3_execution_reaches(Side::Buy, p, 5, px("99.99"), 9, px("99.99")) == Fills);  // through us
  CHECK(l3_execution_reaches(Side::Buy, p, 5, px("100.01"), 1, px("100.01")) == None);  // above us
  // Displayed at our price, ranked and executed at a better one: the aggressor never came down to
  // us, and the order leaves the queue only if it was in front of us.
  CHECK(l3_execution_reaches(Side::Buy, p, 5, p, 7, px("100.01")) == None);
  CHECK(l3_execution_reaches(Side::Buy, p, 5, p, 3, px("100.01")) == Ahead);
  // Asks mirror bids.
  CHECK(l3_execution_reaches(Side::Sell, p, 5, px("100.01"), 9, px("100.01")) == Fills);
  CHECK(l3_execution_reaches(Side::Sell, p, 5, px("99.99"), 1, px("99.99")) == None);
}

TEST_CASE("sim.l3_queue: our order fills only once the orders in front of it are gone") {
  Venue x(config());
  x.add(0, 11, Side::Buy, "100.00", "10");
  x.add(0, 12, Side::Buy, "100.00", "10");
  x.bid(10, 1, "100.00", "5");  // at the venue at 60 us, behind 20
  x.run(150);                   // acknowledged at 110 us
  REQUIRE(x.acks == 1);
  x.add(200, 13, Side::Buy, "100.00", "10");  // behind us
  x.exec(300, 11, "10");
  x.cancel(310, 13);  // behind us: changes nothing
  x.exec(320, 12, "4");
  x.run(400);
  CHECK(x.filled.is_zero());
  x.cancel(500, 12);  // the last 6 in front of us go
  x.add(510, 14, Side::Buy, "100.00", "10");
  x.exec(520, 14, "3");  // an order behind us executes: the aggressor met us first
  x.run(700);
  CHECK(x.filled == qt("3"));
  x.exec(800, 14, "7");
  x.run(900);
  CHECK(x.filled == qt("5"));
}

TEST_CASE("sim.l3_queue: an execution ranked at a better undisplayed price does not reach us") {
  Venue x(config());
  x.add(0, 11, Side::Buy, "100.00", "10");
  x.bid(10, 1, "100.00", "5");
  x.run(150);                                 // acknowledged at 110 us
  x.add(200, 12, Side::Buy, "100.00", "10");  // behind us, displayed at our price
  x.exec(300, 12, "10", "100.01");            // executed at the price it was ranked at
  x.run(400);
  CHECK(x.filled.is_zero());
  x.exec(500, 11, "10");  // the one in front of us
  x.add(510, 13, Side::Buy, "99.99", "10");
  x.exec(520, 13, "10");  // a sell that went through us to 99.99
  x.run(700);
  CHECK(x.filled == qt("5"));
}

TEST_CASE("sim.l3_queue: a cancel in front of us moves us up, one behind us does not") {
  Venue x(config());
  x.add(0, 11, Side::Buy, "100.00", "10");
  x.add(0, 12, Side::Buy, "100.00", "10");
  x.bid(10, 1, "100.00", "5");
  x.run(150);  // acknowledged at 110 us
  x.add(150, 13, Side::Buy, "100.00", "10");
  x.cancel(200, 13);       // behind us
  x.cancel(210, 11, "4");  // part of one in front
  x.cancel(220, 12);       // all of the other
  x.exec(230, 11, "6");    // the 6 left in front of us
  x.run(300);
  CHECK(x.filled.is_zero());
  x.add(400, 14, Side::Buy, "100.00", "10");
  x.exec(410, 14, "1");
  x.run(500);
  CHECK(x.filled == qt("1"));
}
