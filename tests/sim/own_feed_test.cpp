// SimTransport's recorded feed with our resting orders in it (own_orders_in_feed).
#include "test_support.hpp"

#include "fastmm/sim/sim_transport.hpp"

#include <cstring>
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

struct Msg {
  std::vector<std::byte> bytes;
  [[nodiscard]] const EventHeader& hdr() const {
    return *reinterpret_cast<const EventHeader*>(bytes.data());
  }
  template <class M>
  [[nodiscard]] const M& as() const {
    return *reinterpret_cast<const M*>(bytes.data());
  }
};

// Every message the venue sends up to `until`, venue events processed in time order.
std::vector<Msg> drain(sim::SimTransport& v, SimClock& clock, InlineFeed& feed, Timestamp until) {
  std::vector<Msg> out;
  for (;;) {
    const Timestamp a = v.next_order_arrival();
    const Timestamp b = v.next_inbound_ts();
    const Timestamp t = a < b ? a : b;
    if (t > until) break;
    if (t > clock.now()) clock.set(t);
    if (t == a) {
      v.process_order_arrival();
      continue;
    }
    static_cast<void>(v.deliver_next_inbound(feed));
    const EventHeader* h = feed.next();
    REQUIRE(h != nullptr);
    const auto* p = reinterpret_cast<const std::byte*>(h);
    out.push_back(Msg{std::vector<std::byte>(p, p + h->len)});
    feed.release();
  }
  return out;
}

void depth(sim::SimTransport& v,
           std::int64_t us,
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

void ticker(sim::SimTransport& v, std::int64_t us, const char* bid, const char* ask) {
  BookTickerMsg m{};
  init_header(m, EventType::BookTicker, kId, VenueId{0});
  m.hdr.exch_ts = at(us);
  m.hdr.recv_ts = at(us);
  m.hdr.venue_seq = 7;
  m.bid_px = px(bid);
  m.bid_qty = qt("1");
  m.ask_px = px(ask);
  m.ask_qty = qt("1");
  v.on_source_event(m.hdr);
}

OutNewOrderMsg bid(std::uint64_t id, const char* p, const char* q) {
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  m.side = Side::Buy;
  m.type = OrderType::PostOnly;
  m.tif = TimeInForce::Gtc;
  m.price = px(p);
  m.qty = qt(q);
  return m;
}
OutCancelMsg cancel(std::uint64_t id) {
  OutCancelMsg m{};
  init_header(m, EventType::OutCancel, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  return m;
}

// The last message of type `t`.
const Msg* find(const std::vector<Msg>& v, EventType t) {
  const Msg* last = nullptr;
  for (const Msg& m : v) {
    if (m.hdr().type == t) last = &m;
  }
  return last;
}

Qty level(const BookDeltaMsg& d, Side side, Price p) {
  for (const Level& l : side == Side::Buy ? d.bids() : d.asks()) {
    if (l.price == p) return l.qty;
  }
  return Qty::from_raw(-1);  // not listed
}

sim::SimTransportConfig config(sim::FillModel model, bool own) {
  sim::SimTransportConfig tc;
  tc.fill_model = model;
  tc.order_out = sim::LatencyParams{microseconds(100), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(100), Duration{}};
  tc.own_orders_in_feed = own;
  return tc;
}

}  // namespace

TEST_CASE("sim.own_feed: recorded depth and tickers show our resting orders") {
  for (const sim::FillModel model : {sim::FillModel::L2Queue, sim::FillModel::Matching}) {
    CAPTURE(static_cast<int>(model));
    const InstrumentTable t = table();
    SimClock clock{at(0)};
    sim::SimTransport v(clock, t, config(model, true));
    CHECK(v.own_in_feed(VenueId{0}));
    InlineFeed feed(1 << 20);
    depth(v, 0, {{px("99.00"), qt("1")}, {px("98.00"), qt("2")}}, {{px("101.00"), qt("1")}}, true);
    ticker(v, 10, "99.00", "101.00");
    static_cast<void>(drain(v, clock, feed, at(20)));

    // A bid inside the spread: the venue's real-time top of book is now ours, so a ticker goes
    // out (no update id of its own); the recorded depth has not shown it yet.
    REQUIRE(v.send(bid(1, "99.50", "0.3").hdr));
    std::vector<Msg> w = drain(v, clock, feed, at(1000));
    const Msg* tk = find(w, EventType::BookTicker);
    REQUIRE(tk != nullptr);
    CHECK(tk->as<BookTickerMsg>().bid_px == px("99.50"));
    CHECK(tk->as<BookTickerMsg>().bid_qty == qt("0.3"));
    CHECK(tk->as<BookTickerMsg>().ask_px == px("101.00"));
    CHECK(tk->hdr().venue_seq == 0);
    CHECK((tk->hdr().flags & EventHeader::kSynthetic) != 0);
    CHECK(tk->hdr().exch_ts == at(110));  // sent at 10 us, 100 us to the venue
    CHECK(v.stats().own_tickers == 1);

    // A second bid at the recorded 99.00: a recorded ticker shows our 99.50 on top; the next
    // depth update carries both our levels, 99.00 with ours added to the recorded quantity.
    REQUIRE(v.send(bid(2, "99.00", "0.2").hdr));
    static_cast<void>(drain(v, clock, feed, at(2000)));
    CHECK(v.stats().own_tickers == 1);  // not the top: no ticker
    ticker(v, 2100, "99.00", "101.00");
    depth(v, 2200, {{px("98.00"), qt("3")}}, {});
    w = drain(v, clock, feed, at(3000));
    REQUIRE(w.size() == 2);
    CHECK(w[0].as<BookTickerMsg>().bid_px == px("99.50"));
    CHECK(w[0].as<BookTickerMsg>().bid_qty == qt("0.3"));
    CHECK(w[0].hdr().venue_seq == 7);
    const auto& d = w[1].as<BookDeltaMsg>();
    CHECK(level(d, Side::Buy, px("98.00")) == qt("3"));
    CHECK(level(d, Side::Buy, px("99.50")) == qt("0.3"));
    CHECK(level(d, Side::Buy, px("99.00")) == qt("1.2"));

    // Cancelled: the top goes back to the recorded one (the last ticker), the next depth update
    // deletes the level that was only ours and gives 99.00 its recorded quantity back.
    REQUIRE(v.send(cancel(1).hdr));
    REQUIRE(v.send(cancel(2).hdr));
    w = drain(v, clock, feed, at(4000));
    tk = find(w, EventType::BookTicker);
    REQUIRE(tk != nullptr);
    CHECK(tk->as<BookTickerMsg>().bid_px == px("99.00"));
    CHECK(tk->as<BookTickerMsg>().bid_qty == qt("1"));
    depth(v, 4100, {}, {{px("101.00"), qt("2")}});
    w = drain(v, clock, feed, at(5000));
    REQUIRE(w.size() == 1);
    const auto& d2 = w[0].as<BookDeltaMsg>();
    CHECK(level(d2, Side::Buy, px("99.50")).is_zero());
    CHECK(level(d2, Side::Buy, px("99.00")) == qt("1"));
    CHECK(level(d2, Side::Sell, px("101.00")) == qt("2"));
  }
}

TEST_CASE("sim.own_feed: without it, or without recorded tickers, nothing is added") {
  const InstrumentTable t = table();
  SUBCASE("own_orders_in_feed = false") {
    SimClock clock{at(0)};
    sim::SimTransport v(clock, t, config(sim::FillModel::L2Queue, false));
    CHECK_FALSE(v.own_in_feed(VenueId{0}));
    InlineFeed feed(1 << 20);
    depth(v, 0, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
    ticker(v, 10, "99.00", "101.00");
    REQUIRE(v.send(bid(1, "99.50", "0.3").hdr));
    static_cast<void>(drain(v, clock, feed, at(1000)));
    depth(v, 1100, {{px("98.00"), qt("3")}}, {});
    const std::vector<Msg> w = drain(v, clock, feed, at(2000));
    REQUIRE(w.size() == 1);
    CHECK(w[0].as<BookDeltaMsg>().bid_count == 1);
    CHECK(v.stats().own_tickers == 0);
    CHECK(v.stats().own_levels == 0);
  }
  SUBCASE("depth only: no ticker is made up, the depth still shows us") {
    SimClock clock{at(0)};
    sim::SimTransport v(clock, t, config(sim::FillModel::L2Queue, true));
    InlineFeed feed(1 << 20);
    depth(v, 0, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
    REQUIRE(v.send(bid(1, "99.50", "0.3").hdr));
    std::vector<Msg> w = drain(v, clock, feed, at(1000));
    CHECK(find(w, EventType::BookTicker) == nullptr);
    depth(v, 1100, {{px("99.00"), qt("1")}}, {{px("101.00"), qt("1")}}, true);
    w = drain(v, clock, feed, at(2000));
    REQUIRE(w.size() == 1);
    // A snapshot lists every level of ours.
    CHECK(level(w[0].as<BookDeltaMsg>(), Side::Buy, px("99.50")) == qt("0.3"));
    CHECK(v.stats().own_tickers == 0);
  }
}
