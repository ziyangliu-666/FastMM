// SimTransport's recorded feed with our resting orders in it (own_orders_in_feed), and the
// engine's book and tickers with them shown.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/sim_driver.hpp"
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

TEST_CASE("sim.own_feed: our resting quantity follows the instrument's next message") {
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

    // A bid inside the spread rests at 110 us; nothing goes out until the instrument's next
    // message, which goes out as recorded, right behind our quantity at its arrival.
    REQUIRE(v.send(bid(1, "99.50", "0.3").hdr));
    std::vector<Msg> w = drain(v, clock, feed, at(1000));
    CHECK(find(w, EventType::OwnLevels) == nullptr);
    ticker(v, 1100, "99.00", "101.00");
    w = drain(v, clock, feed, at(2000));
    REQUIRE(w.size() == 2);
    REQUIRE(w[0].hdr().type == EventType::OwnLevels);
    const auto& o = w[0].as<OwnLevelsMsg>();
    CHECK(o.bid_count == 1);
    CHECK(o.ask_count == 0);
    CHECK(level(o, Side::Buy, px("99.50")) == qt("0.3"));
    CHECK(w[0].hdr().exch_ts == at(1100));
    CHECK(w[0].hdr().recv_ts == w[1].hdr().recv_ts);
    CHECK((w[0].hdr().flags & EventHeader::kSynthetic) != 0);
    CHECK(w[1].hdr().type == EventType::BookTicker);
    CHECK(w[1].as<BookTickerMsg>().bid_px == px("99.00"));
    CHECK(w[1].hdr().venue_seq == 7);
    CHECK(v.stats().own_levels == 1);

    // A second bid at the recorded 99.00: only what changed goes out.
    REQUIRE(v.send(bid(2, "99.00", "0.2").hdr));
    static_cast<void>(drain(v, clock, feed, at(3000)));
    depth(v, 3100, {{px("98.00"), qt("3")}}, {});
    w = drain(v, clock, feed, at(4000));
    REQUIRE(w.size() == 2);
    CHECK(level(w[1].as<BookDeltaMsg>(), Side::Buy, px("98.00")) == qt("3"));
    CHECK(level(w[1].as<BookDeltaMsg>(), Side::Buy, px("99.00")) == Qty::from_raw(-1));
    const auto& o2 = w[0].as<OwnLevelsMsg>();
    CHECK(o2.bid_count == 1);
    CHECK(level(o2, Side::Buy, px("99.00")) == qt("0.2"));

    // Cancelled: both levels go to zero with the next message; then nothing more.
    REQUIRE(v.send(cancel(1).hdr));
    REQUIRE(v.send(cancel(2).hdr));
    static_cast<void>(drain(v, clock, feed, at(5000)));
    depth(v, 5100, {}, {{px("101.00"), qt("2")}});
    w = drain(v, clock, feed, at(6000));
    REQUIRE(w.size() == 2);
    const auto& o3 = w[0].as<OwnLevelsMsg>();
    CHECK(o3.bid_count == 2);
    CHECK(level(o3, Side::Buy, px("99.50")).is_zero());
    CHECK(level(o3, Side::Buy, px("99.00")).is_zero());
    depth(v, 6100, {}, {{px("101.00"), qt("3")}});
    w = drain(v, clock, feed, at(7000));
    CHECK(w.size() == 1);
    CHECK(v.stats().own_levels == 3);
  }
}

TEST_CASE("sim.own_feed: without it, nothing is added") {
  const InstrumentTable t = table();
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
  CHECK(v.stats().own_levels == 0);
}

namespace {

// Market data from a list, in venue time.
class ListSource final : public sim::MdSource {
 public:
  void add(const EventHeader& h) {
    const auto* b = reinterpret_cast<const std::byte*>(&h);
    msgs_.emplace_back(b, b + h.len);
  }
  const EventHeader* next() override {
    if (i_ >= msgs_.size()) return nullptr;
    std::memcpy(buf_.bytes, msgs_[i_].data(), msgs_[i_].size());
    ++i_;
    return &buf_.hdr();
  }
  void reset() override { i_ = 0; }

 private:
  std::vector<std::vector<std::byte>> msgs_;
  std::size_t i_ = 0;
  sim::EventBuf buf_;
};

void depth_at(ListSource& src,
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
  src.add(d->hdr);
}

// Bids 0.3 at 99.50 at the first book, and writes down what it sees.
struct Watcher {
  struct Seen {
    std::int64_t us;
    bool ticker;
    Level bid;
    Level ask;
    std::size_t bids;
  };
  std::vector<Seen> seen;
  bool sent = false;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    seen.push_back(
        Seen{(ctx.now().ns - kT0) / 1000, false, b.best_bid(), b.best_ask(), b.depth(Side::Buy)});
    if (!sent) {
      sent = true;
      static_cast<void>(ctx.send(NewOrderRequest::limit(id, Side::Buy, px("99.50"), qt("0.3"))));
    }
  }
  template <class Ctx>
  void on_book_ticker(Ctx& ctx, InstrumentId, const BookTickerMsg& m) noexcept {
    seen.push_back(Seen{(ctx.now().ns - kT0) / 1000,
                        true,
                        Level{m.bid_px, m.bid_qty},
                        Level{m.ask_px, m.ask_qty},
                        0});
  }
};

}  // namespace

TEST_CASE("sim.own_feed: the engine's book and tickers show our quantity the feed sent") {
  const InstrumentTable t = table();
  SimClock clock{at(0)};
  sim::SimTransport venue(clock, t, config(sim::FillModel::L2Queue, true));
  InlineFeed feed(1 << 20);
  EngineConfig cfg;
  cfg.risk.max_order_qty = qt("10");
  cfg.risk.max_position = qt("10");
  cfg.risk.max_open_orders = 8;
  cfg.risk.price_collar_bps = 5000;
  cfg.risk.stale_md = seconds(60);
  Watcher w;
  Engine<Watcher, SimClock, sim::SimTransport, InlineFeed> engine(cfg, t, clock, venue, feed, w);
  ListSource src;
  depth_at(
      src, 0, {{px("99.00"), qt("1")}, {px("98.00"), qt("2")}}, {{px("101.00"), qt("1")}}, true);
  // Our bid rests at 100 us. The ticker shows it on top; so does the book.
  BookTickerMsg tk{};
  init_header(tk, EventType::BookTicker, kId, VenueId{0});
  tk.hdr.exch_ts = tk.hdr.recv_ts = at(1000);
  tk.bid_px = px("99.00");
  tk.bid_qty = qt("1");
  tk.ask_px = px("101.00");
  tk.ask_qty = qt("1");
  src.add(tk.hdr);
  depth_at(src, 2000, {}, {{px("101.00"), qt("2")}});
  // An ask at our bid's price: there ours is not shown (the venue would have matched it).
  depth_at(src, 3000, {}, {{px("99.50"), qt("0.1")}});
  sim::SimDriver driver(clock, venue, feed, sim::EngineHooks::for_engine(engine));
  driver.set_source(&src);
  driver.run_all();
  driver.finish();
  REQUIRE(w.seen.size() == 4);
  CHECK(w.seen[0].bid.price == px("99.00"));
  CHECK(w.seen[1].ticker);
  CHECK(w.seen[1].bid.price == px("99.50"));
  CHECK(w.seen[1].bid.qty == qt("0.3"));
  CHECK(w.seen[1].ask.price == px("101.00"));
  CHECK(w.seen[2].bid.price == px("99.50"));
  CHECK(w.seen[2].bid.qty == qt("0.3"));
  CHECK(w.seen[2].bids == 3);
  CHECK(w.seen[3].bid.price == px("99.00"));
  CHECK(w.seen[3].ask.price == px("99.50"));
  CHECK(w.seen[3].bids == 2);
  // Between steps the book is the market's alone.
  CHECK(engine.book(kId).best_bid().price == px("99.00"));
  CHECK(engine.book(kId).depth(Side::Buy) == 2);
  CHECK(venue.stats().own_levels >= 1);
}
