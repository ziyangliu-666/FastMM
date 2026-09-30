// A simulated venue never fills an order before it arrived: recorded events processed after the
// order but stamped earlier (the recorded streams are not monotone in venue time).
#include "test_support.hpp"

#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/sim/venue_order.hpp"

#include <initializer_list>
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

struct Venue {
  explicit Venue(sim::FillModel model) : v(clock, instruments, config(model)) {}
  static sim::SimTransportConfig config(sim::FillModel model) {
    sim::SimTransportConfig tc;
    tc.fill_model = model;
    tc.order_out = sim::LatencyParams{microseconds(100), Duration{}};
    tc.ack_in = sim::LatencyParams{microseconds(100), Duration{}};
    tc.own_orders_in_feed = false;
    return tc;
  }

  // Processes the venue's events up to `until` and returns the order events' venue times by type.
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
      if (h->type == EventType::OrderFill) {
        fills.push_back(h->exch_ts);
        filled += reinterpret_cast<const OrderFillMsg*>(h)->qty;
      }
      feed.release();
    }
  }
  void depth(std::int64_t us,
             std::initializer_list<Level> bids,
             std::initializer_list<Level> asks,
             bool snapshot = false) {
    alignas(64) std::byte buf[BookDeltaMsg::size_for(8, 8)] = {};
    auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
    const auto nb = static_cast<std::uint32_t>(bids.size());
    const auto na = static_cast<std::uint32_t>(asks.size());
    init_header(*d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                kId,
                VenueId{0},
                BookDeltaMsg::size_for(nb, na));
    if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.exch_ts = at(us);
    d->hdr.recv_ts = at(us);
    d->bid_count = nb;
    d->ask_count = na;
    std::size_t k = 0;
    for (const Level& l : bids) d->levels()[k++] = l;
    for (const Level& l : asks) d->levels()[k++] = l;
    v.on_source_event(d->hdr);
  }
  void trade(std::int64_t us, const char* price, const char* qty, Side aggressor) {
    v.on_source_event(trade_msg(us, us, price, qty, aggressor).hdr);
  }
  static TradeMsg trade_msg(
      std::int64_t venue_us, std::int64_t recv_us, const char* price, const char* qty, Side aggr) {
    TradeMsg m{};
    init_header(m, EventType::Trade, kId, VenueId{0});
    m.hdr.exch_ts = at(venue_us);
    m.hdr.recv_ts = at(recv_us);
    m.price = px(price);
    m.qty = qt(qty);
    m.aggressor = aggr;
    return m;
  }
  void bid(std::uint64_t id, const char* price, const char* qty) {
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

  InstrumentTable instruments = table();
  SimClock clock{at(0)};
  InlineFeed feed{1 << 20};
  sim::SimTransport v;
  std::vector<Timestamp> acks;
  std::vector<Timestamp> fills;
  Qty filled;
};

}  // namespace

TEST_CASE("sim.arrival: l2_queue, a trade stamped before the order's arrival does not fill it") {
  Venue x(sim::FillModel::L2Queue);
  x.depth(0, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
  x.bid(1, "99.00", "0.3");  // sent at 0, at the venue at 100 us, behind 1
  x.run(1000);
  REQUIRE(x.acks.size() == 1);
  CHECK(x.acks[0] == at(100));
  // Processed now, printed at 50 us: before the order was there. It took the 1 ahead of us.
  x.trade(50, "99.00", "2", Side::Sell);
  x.run(2000);
  CHECK(x.fills.empty());
  // Printed after the arrival: nothing is left ahead, the order fills at the trade's time.
  x.trade(1500, "99.00", "0.1", Side::Sell);
  x.run(3000);
  REQUIRE(x.fills.size() == 1);
  CHECK(x.fills[0] == at(1500));
  CHECK(x.filled == qt("0.1"));
}

TEST_CASE("sim.arrival: l2_queue, a late trade older than the order's book view keeps the queue") {
  Venue x(sim::FillModel::L2Queue);
  x.depth(0, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
  x.depth(80, {{px("99.00"), qt("1")}}, {});  // the view the order's queue comes from
  x.bid(1, "99.00", "0.3");
  x.run(1000);
  x.trade(50, "99.00", "2", Side::Sell);  // the depth at 80 already shows it
  x.trade(1100, "99.00", "1", Side::Sell);
  x.run(2000);
  CHECK(x.fills.empty());  // the 1 ahead took it
  x.trade(1200, "99.00", "0.3", Side::Sell);
  x.run(3000);
  REQUIRE(x.fills.size() == 1);
  CHECK(x.fills[0] == at(1200));
}

TEST_CASE(
    "sim.arrival: matching, a late recorded level crossing a resting order fills at arrival") {
  Venue x(sim::FillModel::Matching);
  x.depth(0, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
  x.bid(1, "100.00", "0.3");
  x.run(1000);
  REQUIRE(x.acks.size() == 1);
  CHECK(x.acks[0] == at(100));
  // An ask at our price stamped at 50 us, processed after the order rested (at 100 us).
  x.depth(50, {}, {{px("100.00"), qt("1")}});
  x.run(2000);
  REQUIRE(x.fills.size() == 1);
  CHECK(x.fills[0] == at(100));
  CHECK(x.filled == qt("0.3"));
}

namespace {
// A recording in receive order: each event's venue time and receive time.
class Recording final : public sim::MdSource {
 public:
  void add(std::int64_t venue_us, std::int64_t recv_us) {
    events_.push_back(Venue::trade_msg(venue_us, recv_us, "99.00", "0.001", Side::Sell));
  }
  const EventHeader* next() override { return i_ < events_.size() ? &events_[i_++].hdr : nullptr; }
  void reset() override { i_ = 0; }

 private:
  std::vector<TradeMsg> events_;
  std::size_t i_ = 0;
};
}  // namespace

TEST_CASE("sim.venue_order: recorded events come out in venue-time order") {
  Recording r;
  r.add(100, 110);  // 1
  r.add(90, 111);   // 2: stamped before 1, received after it
  r.add(105, 112);  // 3
  r.add(90, 113);   // 4: the venue time of 2, recorded later
  r.add(1150, 1200);
  r.add(40, 2000);  // 6: received 1.96 ms after its venue time, beyond the window
  r.add(1500, 2001);
  sim::VenueOrderSource s(r, microseconds(1000));
  const std::vector<std::uint64_t> order{2, 4, 1, 3, 6, 5, 7};
  std::vector<std::uint64_t> seqs;
  while (const EventHeader* h = s.next()) seqs.push_back(h->seq);
  CHECK(seqs == order);
  CHECK(s.late() == 1);       // 6 came after 3 (105 us)
  CHECK(s.reordered() == 3);  // 1, 3 and 5 came after a later-recorded event
  s.reset();
  seqs.clear();
  while (const EventHeader* h = s.next()) seqs.push_back(h->seq);
  CHECK(seqs == order);
}

TEST_CASE("sim.venue_order: the venue takes events in venue time, the engine as recorded") {
  sim::SimTransportConfig tc = Venue::config(sim::FillModel::L2Queue);
  tc.md_recorded_arrival = true;
  const InstrumentTable t = table();
  SimClock clock{at(0)};
  sim::SimTransport v(clock, t, tc);
  InlineFeed feed(1 << 20);
  // Recorded: a trade at 100 us received at 300, then one at 90 us received at 310. The venue
  // takes the second first.
  TradeMsg first = Venue::trade_msg(100, 300, "99.00", "0.1", Side::Sell);
  TradeMsg second = Venue::trade_msg(90, 310, "99.00", "0.2", Side::Sell);
  first.hdr.seq = 1;
  second.hdr.seq = 2;
  v.on_source_event(second.hdr);
  CHECK(v.next_inbound_ts() == Timestamp::max());  // waits for the one recorded before it
  v.on_source_event(first.hdr);
  std::vector<Qty> got;
  std::vector<Timestamp> arrival;
  while (v.next_inbound_ts() != Timestamp::max()) {
    static_cast<void>(v.deliver_next_inbound(feed));
    const EventHeader* h = feed.next();
    REQUIRE(h != nullptr);
    got.push_back(reinterpret_cast<const TradeMsg*>(h)->qty);
    arrival.push_back(h->recv_ts);
    feed.release();
  }
  REQUIRE(got.size() == 2);
  CHECK(got[0] == qt("0.1"));
  CHECK(got[1] == qt("0.2"));
  CHECK(arrival[0] == at(300));
  CHECK(arrival[1] == at(310));
}
