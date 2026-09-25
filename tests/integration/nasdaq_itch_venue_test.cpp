// The nasdaq_itch venue against fastmm-sim-itch in-process, in a user + network namespace whose lo
// carries multicast (tests/net/netns_test_util.hpp). The test thread drives the venue like
// fastmm-live's network thread (reactor iteration, then Venue::poll()) and mirrors the top of book
// from the BookSnapshot / BookDelta events, which is what the engine holds.
//
//   * joining mid-stream: GLIMPSE plus the buffered stream builds books equal to the simulator's;
//   * drops on both lines: gaps recovered through re-requests, no resync;
//   * no re-request server: every gap is unrecoverable, the venue resyncs through GLIMPSE and its
//     books end equal to the simulator's;
//   * sim_ouch: an order acknowledged, cancelled, filled and replaced, timed wire to wire by the
//     simulator, and cancel_all closing the session;
//   * resolve_venue_env does not ask for API keys for nasdaq_itch.
#include "fastmm/venues/nasdaq/nasdaq_itch_venue.hpp"

#include "../net/netns_test_util.hpp"
#include "integration_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/sim/itch/sim_itch_server.hpp"

#include <doctest/doctest.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using fastmm::sim::itch::SimItchConfig;
using fastmm::sim::itch::SimItchServer;
using fastmm::venues::FeedState;
using namespace fastmm::venues::nasdaq;

namespace {

constexpr std::uint32_t kDepth = 10;

SimItchConfig sim_config() {
  SimItchConfig c = SimItchConfig::defaults();
  c.line_a = {"239.192.40.1", 31601, 0.0};
  c.line_b = {"239.192.40.2", 31602, 0.0};
  c.interface = "lo";
  c.rerequest_port = 0;
  c.glimpse_port = 0;
  c.ouch_port = 0;
  c.heartbeat_ms = 50;
  c.generator.limit_rate_per_s = 1500.0;
  c.generator.market_rate_per_s = 80.0;
  c.generator.cancel_rate_per_order_s = 2.0;
  c.history_messages = 1U << 18;
  c.history_bytes = 1U << 24;
  c.stamp_ring = 1U << 16;
  return c;
}

using Side2 = std::map<std::int64_t, std::int64_t>;  // price raw -> qty raw

struct Mirror {
  Side2 bids;
  Side2 asks;
  bool valid = false;
};

std::string ip_port(std::uint16_t port) {
  return "127.0.0.1:" + std::to_string(port);
}

class Harness {
 public:
  Harness(SimItchServer& sim, const NasdaqItchVenueConfig& base) : sim_(sim), cfg_(base) {
    for (const char* sym : {"FMAA", "FMBB"}) {
      Instrument inst;
      inst.venue = VenueId{0};
      inst.asset_class = AssetClass::Equity;
      inst.symbol.assign(sym);
      inst.tick = Price::from_decimal("0.01").value();
      inst.lot = Qty::from_int(1);
      inst.min_qty = Qty::from_int(1);
      REQUIRE(instruments_.add(inst));
    }
    REQUIRE(symbols_.build(instruments_));
    NasdaqItchVenueConfig& cfg = cfg_;
    cfg.lines[0].interface = "lo";
    cfg.lines[0].group = sim.config().line_a.group;
    cfg.lines[0].port = sim.config().line_a.port;
    cfg.lines[1].interface = "lo";
    cfg.lines[1].group = sim.config().line_b.group;
    cfg.lines[1].port = sim.config().line_b.port;
    cfg.glimpse_url = ip_port(sim.glimpse_port());
    cfg.depth = kDepth;
    cfg.gap_timeout_ns = 1'000'000;
    cfg.request_timeout_ns = 20'000'000;
    cfg.max_request_attempts = 50;
    cfg.reorder_packets = 4096;
    cfg.recovery_buffer_packets = 1U << 14;
    cfg.max_orders = 1U << 16;
    cfg.price_window_ticks = 1U << 16;
    if (cfg.order_entry == OrderEntry::SimOuch) cfg.ouch_url = ip_port(sim.ouch_port());
    venue_ = std::make_unique<NasdaqItchVenue>(VenueId{0}, cfg_);
    const auto r = venue_->load_reference_data(instruments_);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error()));
    venue_->attach(symbols_, instruments_, md_sink_, order_sink_, &outbound_);
    const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
    venue_->subscribe(ids);
    venue_->connect(reactor_);
  }
  ~Harness() { venue_->disconnect(); }
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  // One iteration of fastmm-live's network loop, the simulator's loop, and the engine side.
  void step() {
    sim_.poll(0);
    reactor_.run_once(0);
    venue_->poll();
    drain_md();
    drain_orders();
  }
  bool pump(int timeout_ms, const std::function<bool()>& done) {
    const std::int64_t end = net::Reactor::now_ns() + std::int64_t{timeout_ms} * 1'000'000;
    while (net::Reactor::now_ns() < end) {
      step();
      if (done()) return true;
    }
    return false;
  }
  void run_for(int ms) {
    pump(ms, [] { return false; });
  }
  [[nodiscard]] bool live() const { return venue_->feed_state() == FeedState::Live; }
  [[nodiscard]] bool caught_up() const {
    return live() && sim_.idle() && venue_->next_sequence() == sim_.published() + 1;
  }

  // The venue's top kDepth levels (as the engine sees them) equal the simulator's books.
  void check_books() const {
    for (std::size_t sym = 0; sym < 2; ++sym) {
      INFO("symbol " << sym);
      const Mirror& m = books_[sym];
      REQUIRE(m.valid);
      Side2 bids;
      Side2 asks;
      sim_.engine(sym).for_each_resting(InstrumentId{0}, Side::Buy, [&](const sim::SimOrder& o) {
        bids[o.price.raw] += o.leaves().raw;
      });
      sim_.engine(sym).for_each_resting(InstrumentId{0}, Side::Sell, [&](const sim::SimOrder& o) {
        asks[o.price.raw] += o.leaves().raw;
      });
      REQUIRE(!bids.empty());
      REQUIRE(!asks.empty());
      auto top = [](const Side2& side, bool descending) {
        std::vector<std::pair<std::int64_t, std::int64_t>> out;
        if (descending) {
          for (auto it = side.rbegin(); it != side.rend() && out.size() < kDepth; ++it)
            out.emplace_back(*it);
        } else {
          for (auto it = side.begin(); it != side.end() && out.size() < kDepth; ++it)
            out.emplace_back(*it);
        }
        return out;
      };
      CHECK(top(m.bids, true) == top(bids, true));
      CHECK(top(m.asks, false) == top(asks, false));
      CHECK(m.bids.size() <= kDepth);
      CHECK(m.asks.size() <= kDepth);
    }
  }

  // Pops the first order event of `type`; nullopt when none arrived.
  std::optional<std::vector<std::byte>> take(EventType type) {
    for (auto it = order_events_.begin(); it != order_events_.end(); ++it) {
      if (reinterpret_cast<const EventHeader*>(it->data())->type != type) continue;
      std::vector<std::byte> b = std::move(*it);
      order_events_.erase(it);
      return b;
    }
    return std::nullopt;
  }
  [[nodiscard]] bool has(EventType type) const {
    for (const auto& e : order_events_) {
      if (reinterpret_cast<const EventHeader*>(e.data())->type == type) return true;
    }
    return false;
  }
  template <class M>
  void push_outbound(M m) {
    REQUIRE(outbound_.try_push(&m, m.hdr.len));
    venue_->on_wake();
  }
  template <class M>
  void push_outbound_batch(M a, M b) {
    REQUIRE(outbound_.try_push(&a, a.hdr.len));
    REQUIRE(outbound_.try_push(&b, b.hdr.len));
    venue_->on_wake();
  }
  [[nodiscard]] std::size_t count(EventType type) const {
    std::size_t n = 0;
    for (const auto& e : order_events_) {
      if (reinterpret_cast<const EventHeader*>(e.data())->type == type) ++n;
    }
    return n;
  }

  NasdaqItchVenue& venue() { return *venue_; }
  std::uint64_t md_snapshots = 0;
  std::uint64_t md_deltas = 0;
  std::uint64_t md_resyncing = 0;
  std::uint64_t md_live = 0;
  std::uint64_t md_trades = 0;
  std::uint64_t md_stamped = 0;  // deltas with a receive stamp
  Cycles last_md_t0{};

 private:
  void drain_md() {
    while (const std::byte* p = md_.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      switch (h->type) {
        case EventType::BookSnapshot:
        case EventType::BookDelta: {
          const auto& d = *reinterpret_cast<const BookDeltaMsg*>(p);
          Mirror& m = books_[d.hdr.instrument.value];
          if (d.is_snapshot()) {
            ++md_snapshots;
            m.bids.clear();
            m.asks.clear();
            m.valid = true;
          } else {
            ++md_deltas;
            if (d.hdr.t0_cycles.v != 0 && d.hdr.recv_ts.ns != 0) {
              ++md_stamped;
              last_md_t0 = d.hdr.t0_cycles;
            }
          }
          for (const Level& l : d.bids()) {
            if (l.qty.is_zero()) {
              m.bids.erase(l.price.raw);
            } else {
              m.bids[l.price.raw] = l.qty.raw;
            }
          }
          for (const Level& l : d.asks()) {
            if (l.qty.is_zero()) {
              m.asks.erase(l.price.raw);
            } else {
              m.asks[l.price.raw] = l.qty.raw;
            }
          }
          break;
        }
        case EventType::ConnectionState: {
          const auto& c = *reinterpret_cast<const ConnectionStateMsg*>(p);
          if (c.state == ConnState::Resyncing) {
            ++md_resyncing;
            for (Mirror& m : books_) m = Mirror{};
          } else if (c.state == ConnState::Live) {
            ++md_live;
          }
          break;
        }
        case EventType::Trade:
          ++md_trades;
          break;
        default:
          break;
      }
      md_.release();
    }
  }
  void drain_orders() {
    while (const std::byte* p = orders_.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      order_events_.emplace_back(p, p + h->len);
      orders_.release();
    }
  }

  SimItchServer& sim_;
  NasdaqItchVenueConfig cfg_;
  InstrumentTable instruments_;
  venues::SymbolTable symbols_;
  MsgRing md_{1U << 22};
  MsgRing orders_{1U << 20};
  MsgRing outbound_{1U << 16};
  venues::EventSink md_sink_{&md_, venues::SinkPolicy::Drop};
  venues::EventSink order_sink_{&orders_, venues::SinkPolicy::Spin};
  net::Reactor reactor_;
  std::unique_ptr<NasdaqItchVenue> venue_;
  Mirror books_[2];
  std::vector<std::vector<std::byte>> order_events_;
};

NasdaqItchVenueConfig venue_config(const SimItchServer& sim, bool rerequest) {
  NasdaqItchVenueConfig c;
  c.name = "itch";
  if (rerequest) c.rerequest = ip_port(sim.rerequest_port());
  return c;
}

}  // namespace

TEST_CASE("nasdaq_itch venue: joins mid-stream from GLIMPSE and its books equal the simulator's") {
  if (!net::test::in_multicast_netns()) return;
  SimItchServer sim(sim_config());
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  const std::int64_t until = net::Reactor::now_ns() + 400'000'000;
  while (net::Reactor::now_ns() < until) sim.poll(0);  // the stream is well past sequence 1
  const std::uint64_t published_before = sim.published();
  REQUIRE(published_before > 100);

  Harness h(sim, venue_config(sim, true));
  REQUIRE(h.pump(5000, [&] { return h.live(); }));
  CHECK(sim.stats().snapshots == 1);
  CHECK(h.md_snapshots == 2);
  CHECK(h.md_live == 1);
  h.run_for(500);
  sim.set_generator_enabled(false);
  REQUIRE(h.pump(5000, [&] { return h.caught_up(); }));
  h.run_for(50);
  h.check_books();
  CHECK(h.md_deltas > 50);
  CHECK(h.md_stamped == h.md_deltas);
  CHECK(h.md_trades > 0);
  h.run_for(200);  // the venue publishes its status every 100 ms
  const venues::VenueStatus st = h.venue().status();
  CHECK(st.md == venues::ChannelState::Live);
  CHECK(st.books_synced == 2);
  CHECK(st.books_total == 2);
  CHECK(st.resyncs == 0);
  CHECK(st.feed.state == FeedState::Live);
  CHECK(st.feed.line_packets[0] > 0);
  CHECK(st.feed.line_packets[1] > 0);
  CHECK(st.feed.line_duplicates[1] + st.feed.line_duplicates[0] > 0);
  CHECK(st.feed.book_errors == 0);
  CHECK(st.feed.snapshot_recoveries == 0);
  CHECK(st.feed.kernel_to_t0.count > 0);
}

TEST_CASE("nasdaq_itch venue: drops on both lines are recovered through re-requests") {
  if (!net::test::in_multicast_netns()) return;
  SimItchConfig cfg = sim_config();
  cfg.line_a.drop_rate = 0.05;
  cfg.line_b.drop_rate = 0.05;
  cfg.drop_seed = 21;
  SimItchServer sim(cfg);
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  Harness h(sim, venue_config(sim, true));
  REQUIRE(h.pump(5000, [&] { return h.live(); }));
  h.run_for(1000);
  sim.set_generator_enabled(false);
  REQUIRE(h.pump(5000, [&] { return h.caught_up(); }));
  h.run_for(200);
  h.check_books();
  const venues::VenueStatus st = h.venue().status();
  CHECK(st.feed.gaps > 0);
  CHECK(st.feed.requests > 0);
  CHECK(st.feed.recovered > 0);
  CHECK(st.feed.unrecovered == 0);
  CHECK(st.resyncs == 0);
  CHECK(h.md_resyncing == 1);  // only the one before the first snapshot
  CHECK(sim.stats().requests_answered > 0);
}

TEST_CASE("nasdaq_itch venue: unrecoverable gaps without re-requests resync through GLIMPSE") {
  if (!net::test::in_multicast_netns()) return;
  SimItchConfig cfg = sim_config();
  cfg.line_a.drop_rate = 0.05;
  cfg.line_b.drop_rate = 0.05;
  cfg.drop_seed = 3;
  SimItchServer sim(cfg);
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  Harness h(sim, venue_config(sim, false));
  REQUIRE(h.pump(5000, [&] { return h.live(); }));
  REQUIRE(h.pump(10000, [&] { return h.venue().status().resyncs >= 2 && h.live(); }));
  sim.set_generator_enabled(false);
  // The last datagrams may be lost on both lines too; the heartbeats announce them and the venue
  // resyncs once more before it is caught up.
  REQUIRE(h.pump(10000, [&] { return h.caught_up(); }));
  h.run_for(200);
  h.check_books();
  const venues::VenueStatus st = h.venue().status();
  CHECK(st.feed.unrecovered > 0);
  CHECK(st.feed.requests == 0);
  CHECK(st.feed.snapshot_recoveries >= 2);
  CHECK(st.feed.state == FeedState::Live);
  CHECK(h.md_resyncing >= 3);
  CHECK(sim.stats().snapshots >= 3);
}

TEST_CASE("nasdaq_itch venue: sim_ouch orders round trip and are timed wire to wire") {
  if (!net::test::in_multicast_netns()) return;
  SimItchServer sim(sim_config());
  REQUIRE_MESSAGE(sim.open(), sim.last_error());
  NasdaqItchVenueConfig vc = venue_config(sim, true);
  vc.order_entry = OrderEntry::SimOuch;
  Harness h(sim, vc);
  CHECK(h.venue().caps().supports_replace);
  REQUIRE(h.pump(5000, [&] { return h.live() && h.venue().ouch_up(); }));
  REQUIRE(h.pump(2000, [&] { return h.last_md_t0.v != 0; }));
  {
    const auto up = h.take(EventType::ConnectionState);
    REQUIRE(up.has_value());
    const auto& c = *reinterpret_cast<const ConnectionStateMsg*>(up->data());
    CHECK(c.channel == 1);
    CHECK(c.state == ConnState::Live);
  }

  // 1. A resting buy far below the market, triggered by the last market-data event.
  OutNewOrderMsg o{};
  init_header(o, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  o.hdr.t0_cycles = h.last_md_t0;
  o.cl_ord_id = ClientOrderId{101};
  o.price = Price::from_int(50);
  o.qty = Qty::from_int(100);
  o.side = Side::Buy;
  o.type = OrderType::Limit;
  o.tif = TimeInForce::Gtc;
  h.push_outbound(o);
  REQUIRE(h.pump(2000, [&] { return h.has(EventType::OrderAck); }));
  const auto ack = h.take(EventType::OrderAck);
  CHECK(reinterpret_cast<const OrderAckMsg*>(ack->data())->cl_ord_id == ClientOrderId{101});
  CHECK(sim.wire_to_wire().count() == 1);
  CHECK(sim.stats().w2w_misses == 0);

  // 2. Replace it to 51.00, triggered by market data as well: timed too.
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  r.hdr.t0_cycles = h.last_md_t0;
  r.cl_ord_id = ClientOrderId{102};
  r.orig_cl_ord_id = ClientOrderId{101};
  r.price = Price::from_int(51);
  r.qty = Qty::from_int(80);
  h.push_outbound(r);
  REQUIRE(h.pump(2000, [&] { return sim.stats().replaces == 1 && h.has(EventType::OrderAck); }));
  CHECK(reinterpret_cast<const OrderAckMsg*>(h.take(EventType::OrderAck)->data())->cl_ord_id ==
        ClientOrderId{102});
  CHECK(sim.wire_to_wire().count() == 2);

  // 3. Cancel it.
  OutCancelMsg x{};
  init_header(x, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  x.cl_ord_id = ClientOrderId{102};
  h.push_outbound(x);
  REQUIRE(h.pump(2000, [&] { return h.has(EventType::OrderCancelAck); }));
  CHECK(reinterpret_cast<const OrderCancelAckMsg*>(h.take(EventType::OrderCancelAck)->data())
            ->cl_ord_id == ClientOrderId{102});

  // 4. An aggressive IOC buy fills against the book.
  OutNewOrderMsg ioc = o;
  ioc.hdr.t0_cycles = Cycles{};  // not triggered by market data: not timed
  ioc.cl_ord_id = ClientOrderId{103};
  ioc.price = Price::from_int(150);
  ioc.qty = Qty::from_int(10);
  ioc.tif = TimeInForce::Ioc;
  h.push_outbound(ioc);
  REQUIRE(h.pump(2000, [&] { return h.has(EventType::OrderFill); }));
  const auto fill = h.take(EventType::OrderFill);
  const auto& f = *reinterpret_cast<const OrderFillMsg*>(fill->data());
  CHECK(f.cl_ord_id == ClientOrderId{103});
  CHECK(f.side == Side::Buy);
  CHECK(f.qty.is_positive());
  CHECK(sim.wire_to_wire().count() == 2);

  // 4b. Two orders drained by one wake go out in one write; both are acknowledged.
  {
    h.run_for(50);
    while (h.take(EventType::OrderAck)) {
    }
    OutNewOrderMsg a = o;
    a.hdr.t0_cycles = Cycles{};
    a.cl_ord_id = ClientOrderId{110};
    a.price = Price::from_int(48);
    OutNewOrderMsg b = a;
    b.cl_ord_id = ClientOrderId{111};
    b.price = Price::from_int(47);
    h.push_outbound_batch(a, b);
    REQUIRE(h.pump(2000, [&] { return h.count(EventType::OrderAck) == 2; }));
    OutCancelMsg ca{};
    init_header(ca, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    ca.cl_ord_id = ClientOrderId{110};
    OutCancelMsg cb = ca;
    cb.cl_ord_id = ClientOrderId{111};
    h.push_outbound_batch(ca, cb);
    REQUIRE(h.pump(2000, [&] { return h.count(EventType::OrderCancelAck) == 2; }));
    while (h.take(EventType::OrderAck)) {
    }
    while (h.take(EventType::OrderCancelAck)) {
    }
  }

  // 5. A resting order, then cancel_all from another thread: the simulator cancels on disconnect
  //    and the venue reports the session down with an empty reconciliation.
  o.cl_ord_id = ClientOrderId{104};
  // Stamped from recent market data: the simulator keeps the send stamps of the last datagrams
  // only, and under load step 1's has left the ring by now.
  o.hdr.t0_cycles = h.last_md_t0;
  h.push_outbound(o);
  REQUIRE(h.pump(2000, [&] { return h.has(EventType::OrderAck); }));
  std::thread killer([&] { CHECK(h.venue().cancel_all()); });
  killer.join();
  REQUIRE(h.pump(2000, [&] { return h.has(EventType::Reconcile); }));
  REQUIRE(h.pump(2000, [&] {
    bool resting_at_50 = false;
    sim.engine(0).for_each_resting(InstrumentId{0}, Side::Buy, [&](const sim::SimOrder& so) {
      if (so.price == Price::from_int(50)) resting_at_50 = true;
    });
    return !resting_at_50;
  }));
  CHECK_FALSE(h.venue().ouch_up());
  // New orders are refused once the session is down.
  o.cl_ord_id = ClientOrderId{105};
  h.push_outbound(o);
  REQUIRE(h.pump(1000, [&] { return h.has(EventType::OrderReject); }));
  h.run_for(200);
  const venues::VenueStatus st = h.venue().status();
  CHECK(st.orders_sent == 5);
  CHECK(st.replaces_sent == 1);
  CHECK(st.cancels_sent == 3);
  CHECK(st.wire_tick_to_trade.count == 3);  // the orders with a receive stamp: 101, 102, 104
  CHECK(sim.wire_to_wire().count() == 3);
  CHECK(st.order == venues::ChannelState::Down);
}

TEST_CASE("nasdaq_itch venue: resolve_venue_env needs no API keys for nasdaq_itch") {
  Config cfg;
  VenueSection v;
  v.name = "itch";
  v.kind = "nasdaq_itch";
  v.extra["line_a"] = "239.1.1.1:1";
  cfg.venues.push_back(v);
  CHECK(live::resolve_venue_env(cfg, false, "test"));
  VenueSection b;
  b.name = "binance";
  b.kind = "binance_spot";
  cfg.venues.push_back(b);
  CHECK_FALSE(live::resolve_venue_env(cfg, false, "test"));
}
