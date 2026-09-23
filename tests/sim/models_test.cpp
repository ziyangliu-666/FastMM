// LatencyModel, EventScheduler, QueuePositionModel, Sha256/OutboundHasher, MdAggregator.
#include "test_support.hpp"

#include "fastmm/sim/event_scheduler.hpp"
#include "fastmm/sim/latency_model.hpp"
#include "fastmm/sim/md_aggregator.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/queue_model.hpp"
#include "fastmm/sim/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
}  // namespace

TEST_CASE("sim.latency: deterministic per seed, mean near fixed + jitter, drops honour p") {
  LatencyParams out{microseconds(200), microseconds(50), 0.1};
  LatencyParams ack{microseconds(100), Duration{}, 0.0};
  LatencyParams md{microseconds(10), microseconds(5), 0.0};
  LatencyModel a(out, ack, md, 7);
  LatencyModel b(out, ack, md, 7);
  std::int64_t sum = 0;
  int drops = 0;
  const int n = 20'000;
  std::int64_t lo = INT64_MAX;
  for (int i = 0; i < n; ++i) {
    const LatencySample sa = a.order_out();
    const LatencySample sb = b.order_out();
    CHECK(sa.delay == sb.delay);
    CHECK(sa.dropped == sb.dropped);
    CHECK(sa.delay >= out.fixed);
    sum += sa.delay.ns;
    lo = sa.delay.ns < lo ? sa.delay.ns : lo;
    drops += sa.dropped ? 1 : 0;
    CHECK(a.ack_in() == microseconds(100));  // no jitter: exact
    CHECK(a.md_in() >= microseconds(10));
    static_cast<void>(b.ack_in());
    static_cast<void>(b.md_in());
  }
  const double mean_us = static_cast<double>(sum) / n / 1000.0;
  CHECK(mean_us > 240.0);
  CHECK(mean_us < 260.0);
  CHECK(lo >= 200'000);
  CHECK(drops > n / 10 - 300);
  CHECK(drops < n / 10 + 300);
  LatencyModel c(out, ack, md, 8);
  CHECK(c.order_out().delay != a.order_out().delay);
}

TEST_CASE("sim.scheduler: pops in (fire_ts, insertion) order, fixed capacity") {
  EventScheduler<int, 8> s;
  CHECK(s.empty());
  CHECK(s.peek_ts() == Timestamp::max());
  CHECK(s.push(Timestamp{30}, 1));
  CHECK(s.push(Timestamp{10}, 2));
  CHECK(s.push(Timestamp{20}, 3));
  CHECK(s.push(Timestamp{10}, 4));  // same ts as #2: after it
  CHECK(s.push(Timestamp{5}, 5));
  CHECK(s.size() == 5);
  CHECK(s.peek_ts() == Timestamp{5});
  std::vector<int> order;
  EventScheduler<int, 8>::Entry e{};
  while (s.pop(e)) order.push_back(e.payload);
  CHECK(order == std::vector<int>{5, 2, 4, 3, 1});
  for (int i = 0; i < 8; ++i) CHECK(s.push(Timestamp{i}, i));
  CHECK(s.full());
  CHECK_FALSE(s.push(Timestamp{99}, 99));
  // random stress against sorted expectation
  Xoshiro256ss rng(3);
  EventScheduler<std::uint64_t, 4096> big;
  std::vector<std::pair<std::int64_t, std::uint64_t>> want;
  for (std::uint64_t i = 0; i < 4096; ++i) {
    const auto ts = rng.between(0, 500);
    REQUIRE(big.push(Timestamp{ts}, i));
    want.emplace_back(ts, i);
  }
  std::sort(want.begin(), want.end());
  EventScheduler<std::uint64_t, 4096>::Entry be{};
  for (const auto& w : want) {
    REQUIRE(big.pop(be));
    CHECK(be.fire_ts.ns == w.first);
    CHECK(be.payload == w.second);
  }
  CHECK(big.empty());
}

TEST_CASE("sim.queue_model: ahead shrinks with trades, fills at the touch, trade-through") {
  QueuePositionModel q(10'000);  // fully conservative: cancels never help
  const InstrumentId inst{0};
  auto h = q.place(ClientOrderId{1}, 100, inst, Side::Buy, px("100"), qt("2"), qt("10"));
  REQUIRE(h.valid());
  CHECK(q.get(h).ahead == qt("10"));
  // level shrinks from 10 to 6 by cancellations: conservative model ignores it
  q.on_level_change(inst, Side::Buy, px("100"), qt("10"), qt("6"));
  CHECK(q.get(h).ahead == qt("10"));
  // a sell-aggressor trade of 4 at 100 consumes queue ahead of us
  std::vector<Qty> fills;
  std::vector<Qty> ahead_at_fill;
  auto sink = [&](QueuePositionModel::Handle32, QueuedOrder&, Qty f, Qty ahead) {
    fills.push_back(f);
    ahead_at_fill.push_back(ahead);
  };
  q.on_trade(inst, px("100"), qt("4"), Side::Sell, sink);
  CHECK(fills.empty());
  CHECK(q.get(h).ahead == qt("6"));
  // trade at a different price: nothing
  q.on_trade(inst, px("100.5"), qt("4"), Side::Buy, sink);
  CHECK(fills.empty());
  // 7 more: 6 clear the queue, 1 fills us partially
  q.on_trade(inst, px("100"), qt("7"), Side::Sell, sink);
  REQUIRE(fills.size() == 1);
  CHECK(fills[0] == qt("1"));
  CHECK(ahead_at_fill[0] == qt("6"));  // queue position at the fill
  CHECK(q.get(h).leaves() == qt("1"));
  // trade through (sell at 99.5): remainder fills entirely
  q.on_trade(inst, px("99.5"), qt("0.001"), Side::Sell, sink);
  REQUIRE(fills.size() == 2);
  CHECK(fills[1] == qt("1"));
  CHECK(q.get(h).leaves().is_zero());
  q.remove(h);
  CHECK(q.size() == 0);
  CHECK_FALSE(q.find(ClientOrderId{1}).valid());

  SUBCASE("proportional cancels with conservatism 0 and level disappearing") {
    QueuePositionModel p(0);
    auto g = p.place(ClientOrderId{2}, 101, inst, Side::Sell, px("101"), qt("1"), qt("8"));
    p.on_level_change(inst, Side::Sell, px("101"), qt("8"), qt("4"));  // half cancelled
    CHECK(p.get(g).ahead == qt("4"));
    p.on_level_change(inst, Side::Sell, px("101"), qt("4"), qt("0"));  // level gone
    CHECK(p.get(g).ahead.is_zero());
    p.on_level_change(inst, Side::Sell, px("101"), qt("0"), qt("9"));  // growth: unchanged
    CHECK(p.get(g).ahead.is_zero());
    std::vector<Qty> f2;
    p.on_trade(inst,
               px("101"),
               qt("0.4"),
               Side::Buy,
               [&](QueuePositionModel::Handle32, QueuedOrder&, Qty f, Qty) { f2.push_back(f); });
    REQUIRE(f2.size() == 1);
    CHECK(f2[0] == qt("0.4"));
  }
  SUBCASE("amend keeps priority only when qty <= leaves") {
    auto g = q.place(ClientOrderId{3}, 102, inst, Side::Buy, px("99"), qt("5"), qt("3"));
    CHECK_FALSE(q.amend_keep_priority(g, ClientOrderId{4}, 103, qt("6")));
    CHECK(q.amend_keep_priority(g, ClientOrderId{4}, 103, qt("2")));
    CHECK(q.get(g).ahead == qt("3"));
    CHECK(q.get(g).qty == qt("2"));
    CHECK(q.find(ClientOrderId{4}) == g);
    CHECK_FALSE(q.find(ClientOrderId{3}).valid());
  }
}

TEST_CASE("sim.sha256: FIPS test vectors and streaming equivalence") {
  Sha256 s;
  CHECK(s.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  s.update("abc", 3);
  CHECK(s.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Sha256 t;
  const std::string msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  t.update(msg.data(), msg.size());
  CHECK(t.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  Sha256 u;
  for (char c : msg) u.update(&c, 1);
  CHECK(u.hex() == t.hex());
  std::string million(1'000'000, 'a');
  Sha256 v;
  v.update(million.data(), million.size());
  CHECK(v.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Uneven chunks cross the block boundary at every offset.
  Sha256 w;
  for (std::size_t off = 0, step = 1; off < million.size(); off += step, step = step % 97 + 1)
    w.update(million.data() + off, std::min(step, million.size() - off));
  CHECK(w.hex() == v.hex());
}

TEST_CASE("sim.outbound_hash: normalization ignores journal seq/flags") {
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  m.cl_ord_id = ClientOrderId{5};
  m.price = px("100");
  m.qty = qt("1");
  OutboundHasher a;
  OutboundHasher b;
  a.add(m.hdr);
  OutNewOrderMsg j = m;
  j.hdr.seq = 77;
  j.hdr.flags |= EventHeader::kOutbound;
  b.add(j.hdr);
  CHECK(a.hex() == b.hex());
  CHECK(a.count() == 1);
  // latency stamps are diagnostics: a timer-triggered order carries a stale T0 in the
  // original run and T0 == 0 in the replay
  OutboundHasher c;
  OutNewOrderMsg t = m;
  t.hdr.t0_cycles = Cycles{123456};
  t.hdr.t1_delta = 7;
  c.add(t.hdr);
  CHECK(c.hex() == a.hex());
  m.price = px("101");
  a.add(m.hdr);
  CHECK(a.hex() != b.hex());
}

namespace {
struct Collected {
  std::vector<std::vector<std::byte>> msgs;
  static void emit(void* ctx, EventHeader& h, Timestamp) noexcept {
    auto* self = static_cast<Collected*>(ctx);
    const auto* b = reinterpret_cast<const std::byte*>(&h);
    self->msgs.emplace_back(b, b + h.len);
  }
  template <class M>
  const M& at(std::size_t i) const {
    return *reinterpret_cast<const M*>(msgs[i].data());
  }
};
}  // namespace

TEST_CASE("sim.md_aggregator: first flush is a snapshot, then Binance-style U/u batches") {
  struct Fwd final : MatchingSink {
    MdAggregator* agg = nullptr;
    void on_book_change(InstrumentId id, Side s, Price p, Qty q, std::uint64_t u) override {
      agg->on_book_change(id, s, p, q, u);
    }
  } fwd;
  MatchingEngine me(1, &fwd);
  MdAggregatorConfig cfg;
  cfg.interval = milliseconds(100);
  MdAggregator agg(1, me, cfg, Timestamp{0});
  fwd.agg = &agg;
  Collected out;
  auto order = [&](std::uint64_t id, Side s, const char* p, const char* q) {
    NewOrder o;
    o.account = 0;
    o.cl_ord_id = ClientOrderId{id};
    o.instrument = InstrumentId{0};
    o.side = s;
    o.price = px(p);
    o.qty = qt(q);
    return me.submit(o, Timestamp{1});
  };
  order(1, Side::Buy, "100", "1");
  order(2, Side::Sell, "101", "2");
  CHECK(agg.next_flush_ts() == Timestamp{milliseconds(100).ns});
  agg.flush(Timestamp{milliseconds(100).ns}, &Collected::emit, &out);
  REQUIRE(out.msgs.size() == 2);  // snapshot + book ticker
  const auto& snap = out.at<BookDeltaMsg>(0);
  CHECK(snap.is_snapshot());
  CHECK(snap.hdr.type == EventType::BookSnapshot);
  CHECK(snap.bid_count == 1);
  CHECK(snap.ask_count == 1);
  CHECK(snap.last_update_id == 2);
  CHECK(out.at<BookTickerMsg>(1).bid_px == px("100"));
  CHECK(agg.next_flush_ts() == Timestamp{milliseconds(200).ns});
  // two changes at 100 (net) and one new level -> one delta with U = 3, u = 5
  order(3, Side::Buy, "100", "1");    // uid 3
  order(4, Side::Buy, "99", "1");     // uid 4
  order(5, Side::Buy, "100", "0.5");  // uid 5
  agg.flush(Timestamp{milliseconds(200).ns}, &Collected::emit, &out);
  REQUIRE(out.msgs.size() == 4);
  const auto& d = out.at<BookDeltaMsg>(2);
  CHECK(d.hdr.type == EventType::BookDelta);
  CHECK(d.first_update_id == 3);
  CHECK(d.last_update_id == 5);
  REQUIRE(d.bid_count == 2);
  CHECK(d.ask_count == 0);
  CHECK(d.bids()[0] == Level{px("100"), qt("2.5")});
  CHECK(d.bids()[1] == Level{px("99"), qt("1")});
  CHECK(out.at<BookTickerMsg>(3).bid_qty == qt("2.5"));
  // quiet interval: nothing
  agg.flush(Timestamp{milliseconds(300).ns}, &Collected::emit, &out);
  CHECK(out.msgs.size() == 4);
  // a level deleted shows as qty 0
  me.cancel(0, ClientOrderId{4}, Timestamp{2});
  agg.flush(Timestamp{milliseconds(400).ns}, &Collected::emit, &out);
  REQUIRE(out.msgs.size() == 5);
  CHECK(out.at<BookDeltaMsg>(4).bids()[0] == Level{px("99"), Qty{}});
  CHECK(agg.deltas_sent() == 2);
  CHECK(agg.snapshots_sent() == 1);
}
