// ItchL2Bridge: ITCH -> L3Book -> L2 events.
//
// The property test drives the simulator's MatchingEngine (itch_sim_util.hpp) with stub quotes,
// a drifting market, C executions with Printable N and Y, replaces and partial executions. The
// ITCH messages are grouped into datagrams of random size and fed to the bridge; its output is
// applied to an L2Book the way the engine applies it. After every datagram the L2Book must equal
// the top `depth` levels of a map-based reference book rebuilt from the same messages, and the
// trades must match the reference's executions. Each run starts incomplete, is marked complete,
// loses its books (mark_incomplete) and recovers from a GLIMPSE-like spin of the reference.
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"

#include "itch_sim_util.hpp"
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/itch/itch_messages.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::itch;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::FlowDriver;
using fastmm::codecs::test::ItchFlow;
using fastmm::codecs::test::kSimInst;
using fastmm::codecs::test::kSimLocate;
using fastmm::codecs::test::RecordingSink;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}
Qty qt(std::int64_t n) {
  return Qty::from_int(n);
}

struct ExpectedTrade {
  Price price;
  Qty qty;
  std::uint64_t match;
  Side aggressor;
  bool operator==(const ExpectedTrade&) const = default;
};

// Order book rebuilt from raw ITCH bytes with std::map, independent of ItchDecoder and L3Book.
class RefBook {
 public:
  struct Order {
    Side side;
    std::int64_t price;  // Price::raw
    std::int64_t qty;    // shares
    std::uint64_t arrival;
  };

  // Applies one message of kSimLocate; returns the trade it prints, if any.
  bool apply(std::span<const std::byte> msg, ExpectedTrade& trade) {
    const auto& h = view_as<MessageHeader>(msg.data());
    if (h.stock_locate.get() != kSimLocate) return false;
    switch (h.message_type) {
      case 'A': {
        const auto& m = view_as<AddOrder>(msg.data());
        add(m.order_reference_number.get(), m.buy_sell_indicator, m.price.get(), m.shares.get());
        return false;
      }
      case 'F': {
        const auto& m = view_as<AddOrderMpid>(msg.data());
        add(m.order_reference_number.get(), m.buy_sell_indicator, m.price.get(), m.shares.get());
        return false;
      }
      case 'E': {
        const auto& m = view_as<OrderExecuted>(msg.data());
        const Order o = orders_.at(m.order_reference_number.get());
        reduce(m.order_reference_number.get(), m.executed_shares.get());
        trade = {Price::from_raw(o.price),
                 qt(m.executed_shares.get()),
                 m.match_number.get(),
                 opposite(o.side)};
        return true;
      }
      case 'C': {
        const auto& m = view_as<OrderExecutedWithPrice>(msg.data());
        const Order o = orders_.at(m.order_reference_number.get());
        reduce(m.order_reference_number.get(), m.executed_shares.get());
        if (m.printable != 'Y') return false;
        trade = {nasdaq::price4_to_price(m.execution_price.get()),
                 qt(m.executed_shares.get()),
                 m.match_number.get(),
                 opposite(o.side)};
        return true;
      }
      case 'X': {
        const auto& m = view_as<OrderCancel>(msg.data());
        reduce(m.order_reference_number.get(), m.cancelled_shares.get());
        return false;
      }
      case 'D': {
        const auto& m = view_as<OrderDelete>(msg.data());
        const std::uint64_t ref = m.order_reference_number.get();
        reduce(ref, static_cast<std::uint32_t>(orders_.at(ref).qty));
        return false;
      }
      case 'U': {
        const auto& m = view_as<OrderReplace>(msg.data());
        const std::uint64_t ref = m.original_order_reference_number.get();
        const Side side = orders_.at(ref).side;
        reduce(ref, static_cast<std::uint32_t>(orders_.at(ref).qty));
        add(m.new_order_reference_number.get(),
            side == Side::Buy ? 'B' : 'S',
            m.price.get(),
            m.shares.get());
        return false;
      }
      case 'P': {
        const auto& m = view_as<Trade>(msg.data());
        trade = {nasdaq::price4_to_price(m.price.get()),
                 qt(m.shares.get()),
                 m.match_number.get(),
                 m.buy_sell_indicator == 'B' ? Side::Buy : Side::Sell};
        return true;
      }
      default:
        return false;
    }
  }

  [[nodiscard]] std::vector<Level> top(Side side, std::size_t depth) const {
    std::vector<Level> out;
    const auto& lv = levels_[static_cast<std::size_t>(side)];
    auto push = [&](const auto& kv) {
      if (out.size() < depth) out.push_back(Level{Price::from_raw(kv.first), qt(kv.second)});
    };
    if (side == Side::Buy) {
      for (auto it = lv.rbegin(); it != lv.rend(); ++it) push(*it);
    } else {
      for (const auto& kv : lv) push(kv);
    }
    return out;
  }
  // Open orders in arrival order: what a GLIMPSE spin sends.
  [[nodiscard]] std::vector<std::pair<std::uint64_t, Order>> by_arrival() const {
    std::vector<std::pair<std::uint64_t, Order>> v(orders_.begin(), orders_.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
      return a.second.arrival < b.second.arrival;
    });
    return v;
  }
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

 private:
  void add(std::uint64_t ref, char side, std::uint32_t price4, std::uint32_t shares) {
    const Side s = side == 'B' ? Side::Buy : Side::Sell;
    const std::int64_t p = nasdaq::price4_to_price(price4).raw;
    REQUIRE(orders_.emplace(ref, Order{s, p, shares, ++arrivals_}).second);
    levels_[static_cast<std::size_t>(s)][p] += shares;
  }
  void reduce(std::uint64_t ref, std::uint32_t shares) {
    Order& o = orders_.at(ref);
    const std::int64_t by = std::min<std::int64_t>(shares, o.qty);
    auto& lv = levels_[static_cast<std::size_t>(o.side)];
    lv[o.price] -= by;
    if (lv[o.price] == 0) lv.erase(o.price);
    o.qty -= by;
    if (o.qty == 0) orders_.erase(ref);
  }

  std::map<std::uint64_t, Order> orders_;
  std::map<std::int64_t, std::int64_t> levels_[2];
  std::uint64_t arrivals_ = 0;
};

// Bridge + engine-side L2Book + reference, fed datagram by datagram.
class Harness {
 public:
  explicit Harness(const ItchL2BridgeConfig& cfg)
      : depth_(cfg.depth), bridge_(std::make_unique<ItchL2Bridge>(rec_.sink, cfg)) {
    REQUIRE(bridge_->add_instrument("FMSIM", kSimInst));
  }

  void on_itch(std::span<const std::byte> m) { pending_.emplace_back(m.begin(), m.end()); }
  [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }

  // Sends the pending messages as one datagram. Offline: the bridge misses them.
  void datagram(bool offline = false) {
    if (pending_.empty()) return;
    ++datagrams_;
    stamp_ =
        DatagramStamp{Cycles{1'000'000 + datagrams_},
                      Timestamp{1'700'000'000'000'000'000 + static_cast<std::int64_t>(datagrams_)}};
    for (const Bytes& m : pending_) {
      ++seq_;
      ExpectedTrade t{};
      const bool printed = ref_.apply({m.data(), m.size()}, t);
      if (offline) continue;
      if (printed && bridge_->complete(kSimInst)) expected_.push_back(t);
      bridge_->on_itch_message(seq_, {m.data(), m.size()}, stamp_);
    }
    pending_.clear();
    if (offline) return;
    bridge_->end_datagram();
    drain(false);
  }

  void complete() {
    REQUIRE(bridge_->mark_complete(kSimInst));
    CHECK(bridge_->complete(kSimInst));
    drain(true);
    CHECK(live_);
  }
  void incomplete(std::int32_t reason) {
    bridge_->mark_incomplete(reason);
    CHECK_FALSE(bridge_->complete(kSimInst));
    const std::vector<Bytes> out = rec_.drain();
    REQUIRE(out.size() == 1);
    const auto m = RecordingSink::as<ConnectionStateMsg>(out[0]);
    CHECK(m.hdr.type == EventType::ConnectionState);
    CHECK(m.state == ConnState::Resyncing);
    CHECK(m.channel == 0);
    CHECK(m.reason_code == reason);
    l2_.clear();  // what the engine does on a non-Live market-data state
    live_ = false;
    CHECK(bridge_->book(kSimInst)->order_count() == 0);
  }
  // GLIMPSE-like spin of the reference's open orders (same references, arrival order).
  void glimpse() {
    ItchEncoder enc;
    std::array<std::byte, 64> buf{};
    for (const auto& [ref, o] : ref_.by_arrival()) {
      const std::size_t n = enc.add_order(
          buf, kSimLocate, 1, ref, o.side, qt(o.qty), "FMSIM", Price::from_raw(o.price));
      REQUIRE(n != 0);
      bridge_->on_itch_message(seq_, {buf.data(), n}, stamp_);
    }
    CHECK(rec_.drain().empty());  // incomplete: nothing emitted
  }

  void check_books() const {
    if (!bridge_->complete(kSimInst)) return;
    REQUIRE(l2_.has_snapshot());
    for (const Side s : {Side::Buy, Side::Sell}) {
      const std::vector<Level> want = ref_.top(s, depth_);
      REQUIRE(l2_.depth(s) == want.size());
      for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(l2_.level(s, i) == want[i]);
    }
  }

  [[nodiscard]] const ItchL2Bridge& bridge() const noexcept { return *bridge_; }
  [[nodiscard]] const RefBook& ref() const noexcept { return ref_; }
  [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
  [[nodiscard]] std::uint64_t entered() const noexcept { return entered_; }

 private:
  void drain(bool snapshot_expected) {
    std::size_t deltas = 0;
    std::size_t trade_i = 0;
    for (const Bytes& raw : rec_.drain()) {
      const auto h = RecordingSink::as<EventHeader>(raw);
      CHECK(h.len == raw.size());
      CHECK(h.venue == VenueId{3});
      CHECK(h.t0_cycles == stamp_.t0_cycles);
      CHECK(h.recv_ts == stamp_.recv_ts);
      switch (h.type) {
        case EventType::BookSnapshot:
        case EventType::BookDelta: {
          REQUIRE(h.instrument == kSimInst);
          const auto* d = reinterpret_cast<const BookDeltaMsg*>(raw.data());
          CHECK(d->hdr.venue_seq == d->last_update_id);
          CHECK(d->last_update_id <= seq_);
          if (d->is_snapshot()) {
            CHECK(snapshot_expected);
            CHECK(h.type == EventType::BookSnapshot);
            CHECK(d->first_update_id == d->last_update_id);
          } else {
            ++deltas;
            CHECK_FALSE(snapshot_expected);
            CHECK(d->prev_update_id == last_u_);
            CHECK(d->first_update_id > d->prev_update_id);
            CHECK(d->first_update_id <= d->last_update_id);
            CHECK(d->bid_count + d->ask_count > 0);
            count_entering(*d);
          }
          last_u_ = d->last_update_id;
          l2_.apply_delta(*d);
          break;
        }
        case EventType::Trade: {
          const auto t = RecordingSink::as<TradeMsg>(raw);
          CHECK(h.instrument == kSimInst);
          REQUIRE(trade_i < expected_.size());
          CHECK(ExpectedTrade{t.price, t.qty, t.trade_id, t.aggressor} == expected_[trade_i]);
          ++trade_i;
          ++trades_;
          break;
        }
        case EventType::ConnectionState: {
          const auto m = RecordingSink::as<ConnectionStateMsg>(raw);
          CHECK(snapshot_expected);
          CHECK(m.state == ConnState::Live);
          live_ = true;
          break;
        }
        default:
          FAIL("unexpected event type");
      }
    }
    CHECK(deltas <= 1);  // one instrument: at most one delta per datagram
    CHECK(trade_i == expected_.size());
    expected_.clear();
    check_books();
  }
  // Levels in the delta with a price the engine's book does not have yet.
  void count_entering(const BookDeltaMsg& d) {
    for (const Level& l : d.bids())
      if (l.qty.is_positive() && !has(Side::Buy, l.price)) ++entered_;
    for (const Level& l : d.asks())
      if (l.qty.is_positive() && !has(Side::Sell, l.price)) ++entered_;
  }
  [[nodiscard]] bool has(Side s, Price p) const {
    for (std::size_t i = 0; i < l2_.depth(s); ++i)
      if (l2_.level(s, i).price == p) return true;
    return false;
  }

  std::uint32_t depth_;
  RecordingSink rec_{1U << 22};
  std::unique_ptr<ItchL2Bridge> bridge_;
  L2Book<256> l2_;
  RefBook ref_;
  std::vector<Bytes> pending_;
  std::vector<ExpectedTrade> expected_;
  DatagramStamp stamp_{};
  std::uint64_t seq_ = 0;
  std::uint64_t datagrams_ = 0;
  std::uint64_t last_u_ = 0;
  std::uint64_t trades_ = 0;
  std::uint64_t entered_ = 0;
  bool live_ = false;
};

}  // namespace

TEST_CASE("codecs.itch_l2_bridge: random ITCH through the bridge matches a reference top of book") {
  std::uint64_t trades = 0;
  std::uint64_t entered = 0;
  std::uint64_t skipped = 0;
  std::uint32_t recentres = 0;
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    CAPTURE(seed);
    std::mt19937_64 rng(seed * 7919);
    ItchL2BridgeConfig cfg;
    cfg.venue = VenueId{3};
    cfg.depth = seed % 3 == 0 ? 20 : 5;
    // Window of 1024 ticks of 0.0001 (about 10 price levels of 0.01): most levels, and every
    // stub quote, live in the overflow store, and the touches keep leaving the window.
    cfg.book = seed % 2 == 0 ? L3BookConfig{.price_window_ticks = 1024, .max_orders = 1U << 14}
                             : L3BookConfig{.price_window_ticks = 1U << 16, .max_orders = 1U << 14};
    auto h = std::make_unique<Harness>(cfg);
    auto flow =
        std::make_unique<ItchFlow>([&](std::span<const std::byte> m) { h->on_itch(m); },
                                   fastmm::codecs::test::ItchFlowOptions{.non_printable_every = 2});
    auto eng = std::make_unique<sim::MatchingEngine>(1, flow.get());
    FlowDriver driver(seed, *eng, *flow, {.stub_pct = 4, .drift_every = seed % 2 == 0 ? 10 : 0});
    flow->start();
    std::size_t target = 1;
    for (int step = 0; step < 2000; ++step) {
      CAPTURE(step);
      driver.step(step);
      const bool offline = step >= 1200 && step < 1300;  // books lost: the bridge misses these
      if (step == 150) {
        h->datagram();
        h->complete();  // sequence-1 start: the book was built from the first message
      } else if (step == 1200) {
        h->datagram();
        h->incomplete(7);
      } else if (step == 1300) {
        h->datagram(true);
        h->glimpse();
        h->complete();
      } else if (h->pending() >= target) {
        h->datagram(offline);
        target = 1 + rng() % 12;
      }
    }
    h->datagram();
    const ItchL2BridgeStats& st = h->bridge().stats();
    CHECK(st.book_errors == 0);
    CHECK(st.overflow == 0);
    CHECK(st.snapshots == 2);
    CHECK(h->bridge().book(kSimInst)->order_count() == h->ref().order_count());
    trades += h->trades();
    entered += h->entered();
    skipped += st.skipped;
    recentres += h->bridge().book(kSimInst)->recentre_count();
  }
  CHECK(trades > 0);
  CHECK(entered > 0);
  CHECK(skipped > 0);
  CHECK(recentres > 0);
}

TEST_CASE("codecs.itch_l2_bridge: snapshot, deltas and trades of a hand-written stream") {
  RecordingSink rec(1U << 20);
  ItchL2BridgeConfig cfg;
  cfg.venue = VenueId{2};
  cfg.depth = 2;
  cfg.book = L3BookConfig{.price_window_ticks = 1U << 12, .max_orders = 1024};
  ItchL2Bridge bridge(rec.sink, cfg);
  REQUIRE(bridge.add_instrument("FMA", InstrumentId{5}));
  REQUIRE(bridge.add_instrument("", InstrumentId{6}));
  CHECK_FALSE(bridge.add_instrument("FMA2", InstrumentId{5}));
  REQUIRE(bridge.map_locate(22, InstrumentId{6}));
  CHECK_FALSE(bridge.map_locate(23, InstrumentId{9}));

  ItchEncoder enc;
  std::array<std::byte, 64> buf{};
  std::uint64_t seq = 0;
  const DatagramStamp stamp{Cycles{0}, Timestamp{42}};
  auto send = [&](std::size_t n) {
    REQUIRE(n != 0);
    return bridge.on_itch_message(++seq, {buf.data(), n}, stamp);
  };
  send(enc.system_event(buf, 1, 'O'));
  CHECK(bridge.last_system_event() == 'O');
  send(enc.stock_directory(buf, 21, 1, "FMA"));
  send(enc.stock_directory(buf, 30, 1, "NOTME"));
  send(enc.add_order(buf, 21, 2, 1, Side::Buy, qt(100), "FMA", px("10.00")));
  send(enc.add_order(buf, 21, 2, 2, Side::Buy, qt(200), "FMA", px("9.99")));
  send(enc.add_order(buf, 21, 2, 3, Side::Buy, qt(300), "FMA", px("9.98")));
  send(enc.add_order(buf, 21, 2, 4, Side::Sell, qt(50), "FMA", px("10.01")));
  send(enc.add_order(buf, 22, 2, 5, Side::Sell, qt(10), "FMB", px("20.00")));
  CHECK(send(enc.add_order(buf, 30, 2, 6, Side::Sell, qt(10), "NOTME", px("1.00"))) ==
        venues::ParseStatus::Ignored);
  CHECK(bridge.stats().skipped == 1);
  bridge.end_datagram();
  CHECK(rec.drain().empty());  // incomplete

  REQUIRE(bridge.mark_complete(InstrumentId{5}));
  {
    const std::vector<Bytes> out = rec.drain();
    REQUIRE(out.size() == 1);  // instrument 6 is still incomplete: no Live yet
    const auto* d = reinterpret_cast<const BookDeltaMsg*>(out[0].data());
    CHECK(d->hdr.type == EventType::BookSnapshot);
    CHECK(d->is_snapshot());
    CHECK(d->hdr.instrument == InstrumentId{5});
    CHECK(d->last_update_id == 7);  // the last message applied to instrument 5
    REQUIRE(d->bid_count == 2);
    REQUIRE(d->ask_count == 1);
    CHECK(d->bids()[0] == Level{px("10.00"), qt(100)});
    CHECK(d->bids()[1] == Level{px("9.99"), qt(200)});
    CHECK(d->asks()[0] == Level{px("10.01"), qt(50)});
  }
  REQUIRE(bridge.mark_all_complete());
  {
    const std::vector<Bytes> out = rec.drain();
    REQUIRE(out.size() == 2);
    CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
    const auto st = RecordingSink::as<ConnectionStateMsg>(out[1]);
    CHECK(st.hdr.type == EventType::ConnectionState);
    CHECK(st.state == ConnState::Live);
  }

  // One datagram: the best bid goes (9.98 enters the top 2), the ask trades twice.
  send(enc.order_delete(buf, 21, 3, 1));
  send(enc.order_executed(buf, 21, 3, 4, qt(10), 900));
  send(enc.order_executed_with_price(buf, 21, 3, 4, qt(5), 901, px("10.02"), false));
  send(enc.order_executed_with_price(buf, 21, 3, 4, qt(5), 902, px("10.03"), true));
  send(enc.add_order(buf, 21, 3, 7, Side::Buy, qt(1), "FMA", px("9.00")));  // below the top
  bridge.end_datagram();
  {
    const std::vector<Bytes> out = rec.drain();
    REQUIRE(out.size() == 3);
    const auto t1 = RecordingSink::as<TradeMsg>(out[0]);
    CHECK(t1.price == px("10.01"));
    CHECK(t1.qty == qt(10));
    CHECK(t1.trade_id == 900);
    CHECK(t1.aggressor == Side::Buy);
    CHECK(t1.hdr.venue_seq == 11);
    CHECK(t1.hdr.recv_ts == Timestamp{42});
    const auto t2 = RecordingSink::as<TradeMsg>(out[1]);  // C with Printable N printed nothing
    CHECK(t2.price == px("10.03"));
    CHECK(t2.trade_id == 902);
    const auto* d = reinterpret_cast<const BookDeltaMsg*>(out[2].data());
    CHECK(d->hdr.type == EventType::BookDelta);
    CHECK_FALSE(d->is_snapshot());
    CHECK(d->first_update_id == 10);
    CHECK(d->last_update_id == 14);
    CHECK(d->prev_update_id == 7);
    CHECK(d->hdr.venue_seq == 14);
    REQUIRE(d->bid_count == 2);
    CHECK(d->bids()[0] == Level{px("10.00"), Qty{}});
    CHECK(d->bids()[1] == Level{px("9.98"), qt(300)});
    REQUIRE(d->ask_count == 1);
    CHECK(d->asks()[0] == Level{px("10.01"), qt(30)});
  }

  // Changes below a full top emit nothing.
  send(enc.order_cancel(buf, 21, 4, 7, qt(1)));
  bridge.end_datagram();
  CHECK(rec.drain().empty());
  // A better bid pushes 9.98 out of the top.
  send(enc.order_replace(buf, 21, 4, 3, 8, qt(300), px("10.00")));
  bridge.end_datagram();
  {
    const std::vector<Bytes> out = rec.drain();
    REQUIRE(out.size() == 1);
    const auto* d = reinterpret_cast<const BookDeltaMsg*>(out[0].data());
    CHECK(d->prev_update_id == 14);
    CHECK(d->first_update_id == 15);  // the cancel below the top counts as applied
    CHECK(d->last_update_id == 16);
    REQUIRE(d->bid_count == 2);
    CHECK(d->bids()[0] == Level{px("10.00"), qt(300)});
    CHECK(d->bids()[1] == Level{px("9.98"), Qty{}});
    CHECK(d->ask_count == 0);
  }
  CHECK(bridge.stats().book_errors == 0);
  send(enc.order_delete(buf, 21, 5, 999));  // unknown reference
  CHECK(bridge.stats().book_errors == 1);

  bridge.mark_incomplete(3);
  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 1);
  const auto st = RecordingSink::as<ConnectionStateMsg>(out[0]);
  CHECK(st.state == ConnState::Resyncing);
  CHECK(st.reason_code == 3);
  CHECK_FALSE(st.hdr.instrument.valid());
  CHECK(bridge.book(InstrumentId{5})->order_count() == 0);
  CHECK(bridge.book(InstrumentId{6})->order_count() == 0);
}

TEST_CASE("codecs.itch_l2_bridge: a delta the sink has no room for is sent with the next one") {
  MsgRing ring(4096);
  venues::EventSink sink(&ring, venues::SinkPolicy::Drop);
  ItchL2BridgeConfig cfg;
  cfg.depth = 4;
  cfg.book = L3BookConfig{.price_window_ticks = 1U << 12, .max_orders = 1024};
  ItchL2Bridge bridge(sink, cfg);
  REQUIRE(bridge.add_instrument("FMA", InstrumentId{1}));
  REQUIRE(bridge.map_locate(1, InstrumentId{1}));
  REQUIRE(bridge.mark_complete(InstrumentId{1}));
  L2Book<64> l2;
  auto drain = [&] {
    while (const std::byte* p = ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot)
        l2.apply_delta(*reinterpret_cast<const BookDeltaMsg*>(p));
      ring.release();
    }
  };
  drain();
  ItchEncoder enc;
  std::array<std::byte, 64> buf{};
  std::uint64_t seq = 0;
  auto add = [&](std::uint64_t ref, Side s, const char* price) {
    const std::size_t n = enc.add_order(buf, 1, 1, ref, s, qt(1), "FMA", px(price));
    bridge.on_itch_message(++seq, {buf.data(), n}, DatagramStamp{});
  };
  add(1, Side::Buy, "5.00");
  bridge.end_datagram();
  drain();
  CHECK(l2.best_bid() == Level{px("5.00"), qt(1)});
  // Fill the ring so the next delta does not fit.
  while (std::byte* p = ring.try_reserve(128)) {
    std::memset(p, 0, 128);
    reinterpret_cast<EventHeader*>(p)->len = 128;
    reinterpret_cast<EventHeader*>(p)->type = EventType::Timer;
    ring.commit();
  }
  add(2, Side::Buy, "5.01");
  add(3, Side::Sell, "5.05");
  bridge.end_datagram();
  CHECK(bridge.stats().overflow == 1);
  drain();
  CHECK(l2.best_bid() == Level{px("5.00"), qt(1)});
  add(4, Side::Sell, "5.06");
  bridge.end_datagram();
  drain();
  CHECK(l2.best_bid() == Level{px("5.01"), qt(1)});
  CHECK(l2.best_ask() == Level{px("5.05"), qt(1)});
  CHECK(l2.depth(Side::Sell) == 2);
  CHECK(l2.depth(Side::Buy) == 2);
}
