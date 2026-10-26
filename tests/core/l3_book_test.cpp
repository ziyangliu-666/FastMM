#include "fastmm/core/book/l3_book.hpp"

#include "test_support.hpp"

#include <deque>
#include <map>
#include <random>
#include <vector>

using namespace fastmm;

namespace {
const Price kTick = Price::from_decimal("0.01").value();
Price px(std::int64_t ticks) {
  return Price::from_raw(ticks * kTick.raw);
}
Qty qt(std::int64_t n) {
  return Qty::from_int(n);
}
using Book = L3Book;
constexpr L3BookConfig kCfg{.price_window_ticks = 1U << 10, .max_orders = 1U << 12};

struct OOrder {
  std::uint64_t ref;
  std::int64_t qty;
};
struct Oracle {
  std::map<std::int64_t, std::deque<OOrder>> side[2];
  std::map<std::uint64_t, std::pair<Side, std::int64_t>> where;  // ref -> (side, price)
  bool add(std::uint64_t ref, Side s, std::int64_t p, std::int64_t q) {
    if (where.count(ref) != 0) return false;
    side[static_cast<int>(s)][p].push_back({ref, q});
    where[ref] = {s, p};
    return true;
  }
  bool reduce(std::uint64_t ref, std::int64_t by) {
    auto it = where.find(ref);
    if (it == where.end()) return false;
    auto& dq = side[static_cast<int>(it->second.first)][it->second.second];
    for (auto d = dq.begin(); d != dq.end(); ++d) {
      if (d->ref == ref) {
        if (by == 0 || by >= d->qty) {
          dq.erase(d);
          if (dq.empty()) side[static_cast<int>(it->second.first)].erase(it->second.second);
          where.erase(it);
        } else {
          d->qty -= by;
        }
        return true;
      }
    }
    return false;
  }
};

void check_equal(const Book& b, const Oracle& o) {
  for (Side s : {Side::Buy, Side::Sell}) {
    const auto& m = o.side[static_cast<int>(s)];
    REQUIRE(b.depth(s) == m.size());
    std::size_t i = 0;
    Level previous{};
    auto visit = [&](std::int64_t price, const std::deque<OOrder>& dq) {
      const Level l = b.level(s, i++);
      REQUIRE(l.price == px(price));
      std::int64_t sum = 0;
      for (auto& x : dq) sum += x.qty;
      REQUIRE(l.qty == qt(sum));
      if (previous.qty.is_positive()) REQUIRE(b.next_level(s, previous.price) == l);
      previous = l;
      // FIFO order at the level
      std::vector<std::uint64_t> refs;
      b.for_each_order_at(s, px(price), [&](const L3Order& ord) { refs.push_back(ord.ref); });
      REQUIRE(refs.size() == dq.size());
      for (std::size_t k = 0; k < refs.size(); ++k) REQUIRE(refs[k] == dq[k].ref);
    };
    if (s == Side::Buy) {
      for (auto it = m.rbegin(); it != m.rend(); ++it) visit(it->first, it->second);
    } else {
      for (auto it = m.begin(); it != m.end(); ++it) visit(it->first, it->second);
    }
    if (previous.qty.is_positive()) CHECK(b.next_level(s, previous.price).qty.is_zero());
  }
  REQUIRE(b.order_count() == o.where.size());
}

TEST_CASE("core.l3_book: deletes from the middle of a wrapped index cluster") {
  Book b(kTick, L3BookConfig{.price_window_ticks = 1024, .max_orders = 16});
  std::vector<std::uint64_t> refs;
  // A 32-slot index: force the cluster to cross its last slot, then delete from its middle.
  for (std::uint64_t ref = 1; refs.size() < 12; ++ref)
    if ((ref * 0x9E3779B97F4A7C15ULL) >> 59U == 31) refs.push_back(ref);
  for (const auto ref : refs) REQUIRE(b.add(ref, Side::Buy, px(100), qt(1)) == L3Error::None);
  while (!refs.empty()) {
    const auto index = refs.size() / 2;
    const auto ref = refs[index];
    const auto h = b.find(ref);
    REQUIRE(h.valid());
    Qty aggregate;
    REQUIRE(b.cancel(h, Qty{}, &aggregate) == L3Error::None);
    refs.erase(refs.begin() + static_cast<std::ptrdiff_t>(index));
    CHECK(aggregate == qt(static_cast<std::int64_t>(refs.size())));
    CHECK(b.order(ref) == nullptr);
    for (std::size_t i = 0; i < refs.size(); ++i) {
      REQUIRE(b.order(refs[i]) != nullptr);
      CHECK(b.qty_ahead(refs[i]) == qt(static_cast<std::int64_t>(i)));
    }
  }
  CHECK(b.order_count() == 0);
}
}  // namespace

TEST_CASE("core.l3_book: add/execute/cancel/replace basics") {
  Book b(kTick, kCfg);
  CHECK(b.add(1, Side::Buy, px(100), qt(5)) == L3Error::None);
  CHECK(b.add(2, Side::Buy, px(100), qt(3)) == L3Error::None);
  CHECK(b.add(3, Side::Buy, px(99), qt(1)) == L3Error::None);
  CHECK(b.add(4, Side::Sell, px(101), qt(2)) == L3Error::None);
  CHECK(b.add(4, Side::Sell, px(101), qt(2)) == L3Error::DuplicateRef);
  CHECK(b.add(5, Side::Sell, px(101), Qty{}) == L3Error::BadQty);
  CHECK(b.add(5, Side::Sell, Price::from_raw(px(101).raw + 1), qt(1)) == L3Error::OffTick);
  CHECK(b.best_bid() == Level{px(100), qt(8)});
  CHECK(b.best_ask() == Level{px(101), qt(2)});
  CHECK(b.spread() == px(1));
  CHECK(b.is_valid());
  CHECK(b.qty_ahead(2) == qt(5));
  CHECK(b.qty_ahead(1) == qt(0));
  Price fill{};
  CHECK(b.execute(1, qt(2), &fill) == L3Error::None);
  CHECK(fill == px(100));
  CHECK(b.order(1)->qty == qt(3));
  CHECK(b.execute(1, qt(10)) == L3Error::None);  // over-execute clamps and removes
  CHECK(b.order(1) == nullptr);
  CHECK(b.qty_ahead(2) == qt(0));
  CHECK(b.cancel(2, qt(1)) == L3Error::None);
  CHECK(b.order(2)->qty == qt(2));
  CHECK(b.replace(2, 20, px(98), qt(4)) == L3Error::None);
  CHECK(b.order(2) == nullptr);
  CHECK(b.best_bid() == Level{px(99), qt(1)});
  CHECK(b.level(Side::Buy, 1) == Level{px(98), qt(4)});
  CHECK(b.cancel(3) == L3Error::None);
  CHECK(b.best_bid() == Level{px(98), qt(4)});
  CHECK(b.remove(20) == L3Error::None);
  CHECK(b.best_bid().qty.is_zero());
  CHECK(b.cancel(999) == L3Error::UnknownRef);
  CHECK(b.execute(999, qt(1)) == L3Error::UnknownRef);
  CHECK_FALSE(b.is_valid());
  CHECK(b.depth(Side::Sell) == 1);
  const auto top = b.to_l2_top<4>(Side::Sell);
  CHECK(top.count == 1);
  CHECK(top[0].price == px(101));
  b.clear();
  CHECK(b.order_count() == 0);
  CHECK(b.add(7, Side::Buy, px(100), qt(1)) == L3Error::None);
}

TEST_CASE("core.l3_book: orders outside the window live in the overflow store") {
  Book b(kTick, kCfg);  // 1024-tick window around the first order
  CHECK(b.add(1, Side::Buy, px(10000), qt(1)) == L3Error::None);
  CHECK(b.add(2, Side::Sell, px(10001), qt(1)) == L3Error::None);
  CHECK(b.add(3, Side::Sell, px(12000), qt(2)) == L3Error::None);  // stub ask
  CHECK(b.add(4, Side::Buy, px(5), qt(7)) == L3Error::None);       // stub bid
  CHECK(b.add(5, Side::Sell, px(12000), qt(3)) == L3Error::None);
  CHECK(b.recentre_count() == 0);
  CHECK(b.overflow_levels(Side::Sell) == 1);
  CHECK(b.overflow_levels(Side::Buy) == 1);
  CHECK(b.best_bid() == Level{px(10000), qt(1)});
  CHECK(b.best_ask() == Level{px(10001), qt(1)});
  CHECK(b.depth(Side::Sell) == 2);
  CHECK(b.level(Side::Sell, 1) == Level{px(12000), qt(5)});
  CHECK(b.level(Side::Buy, 1) == Level{px(5), qt(7)});
  CHECK(b.qty_at_or_better(Side::Sell, px(12000)) == qt(6));
  CHECK(b.price_for_qty(Side::Sell, qt(2)) == px(12000));
  CHECK(b.price_for_qty(Side::Buy, qt(8)) == px(5));
  CHECK(b.qty_ahead(5) == qt(2));
  std::vector<std::uint64_t> fifo;
  b.for_each_order_at(Side::Sell, px(12000), [&](const L3Order& o) { fifo.push_back(o.ref); });
  CHECK(fifo == std::vector<std::uint64_t>{3, 5});
  Price at{};
  CHECK(b.execute(3, qt(1), &at) == L3Error::None);
  CHECK(at == px(12000));
  CHECK(b.level(Side::Sell, 1) == Level{px(12000), qt(4)});

  // The ask touch leaves the window, but the touches are more than a window apart: the window
  // stays on the bid.
  CHECK(b.cancel(2) == L3Error::None);
  CHECK(b.recentre_count() == 0);
  CHECK(b.best_ask() == Level{px(12000), qt(4)});
  CHECK(b.is_valid());

  // A bid above the window is the new touch. The overflow store serves it at first; once it
  // has taken a few operations the window moves to the two touches and the 10000 bid moves to
  // the overflow store.
  CHECK(b.add(6, Side::Buy, px(11990), qt(2)) == L3Error::None);
  CHECK(b.recentre_count() == 0);
  CHECK(b.best_bid() == Level{px(11990), qt(2)});
  CHECK(b.best_ask() == Level{px(12000), qt(4)});
  for (std::uint64_t r = 100; r < 104; ++r) {
    CHECK(b.add(r, Side::Buy, px(11990), qt(1)) == L3Error::None);
    CHECK(b.cancel(r) == L3Error::None);
  }
  CHECK(b.recentre_count() == 1);
  CHECK(b.best_bid() == Level{px(11990), qt(2)});
  CHECK(b.best_ask() == Level{px(12000), qt(4)});
  CHECK(b.overflow_levels(Side::Buy) == 2);
  CHECK(b.overflow_levels(Side::Sell) == 0);
  CHECK(b.level(Side::Buy, 1) == Level{px(10000), qt(1)});
  CHECK(b.level(Side::Buy, 2) == Level{px(5), qt(7)});
  fifo.clear();
  b.for_each_order_at(Side::Sell, px(12000), [&](const L3Order& o) { fifo.push_back(o.ref); });
  CHECK(fifo == std::vector<std::uint64_t>{3, 5});

  // The bid touch falls back to 10000, 2000 ticks from the ask: the window keeps the ask.
  CHECK(b.cancel(6) == L3Error::None);
  CHECK(b.recentre_count() == 1);
  CHECK(b.best_bid() == Level{px(10000), qt(1)});
  CHECK(b.spread() == px(2000));

  // Only overflow levels left on both sides: the window moves to the bid touch once it trades.
  CHECK(b.cancel(3) == L3Error::None);
  CHECK(b.cancel(5) == L3Error::None);
  CHECK(b.cancel(1) == L3Error::None);
  CHECK(b.best_bid() == Level{px(5), qt(7)});
  CHECK(b.depth(Side::Buy) == 1);
  for (std::uint64_t r = 110; r < 114; ++r) {
    CHECK(b.add(r, Side::Buy, px(5), qt(1)) == L3Error::None);
    CHECK(b.cancel(r) == L3Error::None);
  }
  CHECK(b.recentre_count() == 2);
  CHECK(b.overflow_levels(Side::Buy) == 0);
  CHECK(b.best_bid() == Level{px(5), qt(7)});
  CHECK(b.cancel(4) == L3Error::None);
  CHECK(b.order_count() == 0);
  // Empty book: any price re-anchors the window.
  CHECK(b.add(7, Side::Sell, px(50000), qt(1)) == L3Error::None);
  CHECK(b.best_ask().price == px(50000));
  CHECK(b.overflow_levels(Side::Sell) == 0);
}

TEST_CASE("core.l3_book: a full overflow store refuses orders and skips recentres") {
  L3Book b(kTick, {.price_window_ticks = 64, .max_orders = 64, .max_overflow_levels = 2});
  CHECK(b.add(1, Side::Buy, px(1000), qt(1)) == L3Error::None);
  CHECK(b.add(2, Side::Sell, px(1001), qt(1)) == L3Error::None);
  CHECK(b.add(3, Side::Buy, px(900), qt(1)) == L3Error::None);
  CHECK(b.add(4, Side::Buy, px(901), qt(1)) == L3Error::None);
  CHECK(b.add(5, Side::Buy, px(902), qt(1)) == L3Error::OutOfWindow);
  CHECK(b.order(5) == nullptr);
  CHECK(b.add(5, Side::Buy, px(901), qt(2)) == L3Error::None);  // existing overflow level
  CHECK(b.depth(Side::Buy) == 3);
  // Asks 1001..1031 fill the window [968, 1032) above the bid. An ask below the window is the new
  // touch, but moving the window to it would push 20 ask levels into a store of 2: skipped.
  for (std::int64_t i = 1; i < 32; ++i)
    CHECK(b.add(static_cast<std::uint64_t>(100 + i), Side::Sell, px(1000 + i), qt(1)) ==
          L3Error::None);
  CHECK(b.add(200, Side::Sell, px(960), qt(3)) == L3Error::None);
  for (std::uint64_t r = 300; r < 320; ++r) {
    CHECK(b.add(r, Side::Sell, px(960), qt(1)) == L3Error::None);
    CHECK(b.cancel(r) == L3Error::None);
  }
  CHECK(b.recentre_failures() >= 1);
  CHECK(b.recentre_count() == 0);
  CHECK(b.best_ask() == Level{px(960), qt(3)});
  CHECK(b.level(Side::Sell, 1) == Level{px(1001), qt(2)});
  CHECK(b.depth(Side::Sell) == 32);
  CHECK(b.cancel(200) == L3Error::None);
  CHECK(b.best_ask() == Level{px(1001), qt(2)});
  CHECK(b.depth(Side::Buy) == 3);
}

TEST_CASE("core.l3_book: pool exhaustion") {
  L3Book small(kTick, {.price_window_ticks = 64, .max_orders = 4});
  for (std::uint64_t i = 0; i < 4; ++i)
    CHECK(small.add(i, Side::Buy, px(10), qt(1)) == L3Error::None);
  CHECK(small.add(9, Side::Buy, px(10), qt(1)) == L3Error::PoolExhausted);
  CHECK(small.cancel(0) == L3Error::None);
  CHECK(small.add(9, Side::Buy, px(10), qt(1)) == L3Error::None);
}

TEST_CASE("core.l3_book: clear() returns every order to the pool") {
  L3Book b(kTick, {.price_window_ticks = 64, .max_orders = 64, .max_overflow_levels = 16});
  for (int round = 0; round < 3; ++round) {
    std::uint64_t ref = 1;
    // Window levels and stub levels in the overflow store on both sides, eight orders a level.
    for (std::int64_t i = 0; i < 64; ++i) {
      CAPTURE(round);
      CAPTURE(i);
      const bool buy = i % 2 == 0;
      const std::int64_t k = (i / 2) % 8;
      const std::int64_t p = i % 8 < 6 ? (buy ? 1000 - k : 1001 + k) : (buy ? 10 + k : 90000 + k);
      REQUIRE(b.add(ref++, buy ? Side::Buy : Side::Sell, px(p), qt(1)) == L3Error::None);
    }
    CHECK(b.overflow_levels(Side::Sell) > 0);
    CHECK(b.add(ref, Side::Buy, px(1000), qt(1)) == L3Error::PoolExhausted);
    b.clear();
    CHECK(b.order_count() == 0);
    CHECK(b.depth(Side::Buy) == 0);
    CHECK(b.depth(Side::Sell) == 0);
    CHECK(b.overflow_levels(Side::Buy) == 0);
    CHECK(b.order(1) == nullptr);
    CHECK(b.best_bid().qty.is_zero());
  }
  // The pool holds max_orders again and the window re-anchors anywhere.
  for (std::uint64_t r = 1; r <= 64; ++r)
    CHECK(b.add(r, Side::Sell, px(-500 + static_cast<std::int64_t>(r % 8)), qt(1)) ==
          L3Error::None);
  CHECK(b.best_ask() == Level{px(-500), qt(8)});
}

// The window must not follow a touch that steps out and straight back: on the ITCH sample day
// each such step cost a recentre, and each recentre moved the whole window (1.5 MiB a side).
TEST_CASE("core.l3_book: an order beyond the touch and its cancel move nothing") {
  L3Book b(kTick, {.price_window_ticks = 1U << 12, .max_orders = 1U << 14});
  std::uint64_t ref = 1;
  for (std::int64_t i = 0; i < 1000; ++i) {
    REQUIRE(b.add(ref++, Side::Buy, px(10000 - i), qt(1)) == L3Error::None);
    REQUIRE(b.add(ref++, Side::Sell, px(10001 + i), qt(1)) == L3Error::None);
  }
  for (int k = 0; k < 2000; ++k) {
    // A bid and an ask across the whole window, and a one-sided touch far below.
    const Side s = k % 2 == 0 ? Side::Buy : Side::Sell;
    REQUIRE(b.add(ref, s, px(s == Side::Buy ? 20000 : 1000), qt(1)) == L3Error::None);
    CHECK((s == Side::Buy ? b.best_bid() : b.best_ask()).price ==
          px(s == Side::Buy ? 20000 : 1000));
    REQUIRE(b.cancel(ref++) == L3Error::None);
  }
  CHECK(b.recentre_count() == 0);
  CHECK(b.best_bid() == Level{px(10000), qt(1)});
  CHECK(b.best_ask() == Level{px(10001), qt(1)});
}

// Worst case for the window: a dense one-sided book whose touch alternates between two places a
// window apart, trading at each. Every move pays for itself with the operations before it, so
// the levels moved stay a constant per operation, however large the window and however often
// the touch jumps.
TEST_CASE("core.l3_book: levels moved by recentres stay O(1) per operation") {
  for (const std::size_t window : {std::size_t{1} << 10, std::size_t{1} << 16}) {
    CAPTURE(window);
    L3Book b(kTick, {.price_window_ticks = window, .max_orders = 1U << 14});
    const auto w = static_cast<std::int64_t>(window);
    std::uint64_t ref = 1;
    for (std::int64_t i = 0; i < 400; ++i)
      REQUIRE(b.add(ref++, Side::Buy, px(100000 - i), qt(1)) == L3Error::None);
    std::uint64_t ops = 0;
    for (int jump = 0; jump < 200; ++jump) {
      const std::int64_t at = 100000 + (jump % 2 == 0 ? 3 * w : 0) + 1;
      const std::uint64_t first = ref;
      for (int k = 0; k <= jump % 50; ++k) {
        REQUIRE(b.add(ref++, Side::Buy, px(at), qt(1)) == L3Error::None);
        ++ops;
      }
      for (std::uint64_t r = first; r < ref; ++r) {
        REQUIRE(b.cancel(r) == L3Error::None);
        ++ops;
      }
      CHECK(b.best_bid() == Level{px(100000), qt(1)});
    }
    CHECK(b.recentre_count() > 0);
    CHECK(b.recentre_failures() == 0);
    // A move waits for a quarter of the levels it may move in operations: at most 4 levels moved
    // per operation, whatever the window.
    CHECK(b.recentre_levels_moved() <= 4 * ops);
  }
}

namespace {
// wide: prices 950..1050 in a 1024-tick window. Otherwise a 64-tick window, a reference price
// drifting by up to 3 ticks every 20 operations, and 5 % stub quotes far from it. jumpy: the
// reference price jumps by up to three windows every 50 operations, through zero and below, and
// the book is cleared now and then.
void run_oracle(std::uint32_t seed, bool wide, bool jumpy = false) {
  CAPTURE(seed);
  CAPTURE(wide);
  CAPTURE(jumpy);
  std::mt19937 rng(seed);
  Book b(kTick,
         wide ? kCfg
              : L3BookConfig{
                    .price_window_ticks = 64, .max_orders = 1U << 12, .max_overflow_levels = 1024});
  Oracle o;
  std::uint64_t next_ref = 1;
  std::vector<std::uint64_t> live;
  std::uniform_int_distribution<int> op_dist(0, 9);
  std::uniform_int_distribution<int> px_dist(950, 1050);
  std::uniform_int_distribution<int> qty_dist(1, 20);
  std::int64_t centre = 0;
  auto draw_price = [&]() -> std::int64_t {
    const std::int64_t p = px_dist(rng);
    if (wide) return p;
    if (rng() % 20 == 0) return (rng() & 1) ? 1 + static_cast<std::int64_t>(rng() % 20) : 5000;
    return p - (jumpy ? 1000 : 0) + centre;
  };
  for (int op = 0; op < 4000; ++op) {
    if (!wide && !jumpy && op % 20 == 0) centre += static_cast<std::int64_t>(rng() % 7) - 3;
    if (jumpy && op % 50 == 0) centre += static_cast<std::int64_t>(rng() % 385) - 192;
    if (jumpy && op % 1500 == 1499) {
      b.clear();
      o = Oracle{};
      live.clear();
    }
    const int o_ = op_dist(rng);
    if (o_ < 4 || live.empty()) {
      const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
      const std::int64_t p = draw_price();
      const std::int64_t q = qty_dist(rng);
      const std::uint64_t ref = next_ref++;
      REQUIRE(b.add(ref, s, px(p), qt(q)) == L3Error::None);
      REQUIRE(o.add(ref, s, p, q));
      live.push_back(ref);
    } else {
      const std::size_t k = rng() % live.size();
      const std::uint64_t ref = live[k];
      const L3Order* ord = b.order(ref);
      REQUIRE(ord != nullptr);
      const std::int64_t rem = ord->qty.units();
      if (o_ < 6) {  // execute partial or full
        const std::int64_t by =
            1 + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rem));
        REQUIRE(b.execute(ref, qt(by)) == L3Error::None);
        REQUIRE(o.reduce(ref, by));
        if (by >= rem) live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      } else if (o_ < 8) {  // cancel (partial or delete)
        const std::int64_t by =
            static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(rem + 1));
        REQUIRE(b.cancel(ref, qt(by)) == L3Error::None);
        REQUIRE(o.reduce(ref, by));
        if (by == 0 || by >= rem) live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      } else {  // replace
        const Side s = ord->side;
        const std::int64_t p = draw_price();
        const std::int64_t q = qty_dist(rng);
        const std::uint64_t nref = next_ref++;
        REQUIRE(b.replace(ref, nref, px(p), qt(q)) == L3Error::None);
        REQUIRE(o.reduce(ref, 0));
        REQUIRE(o.add(nref, s, p, q));
        live[k] = nref;
      }
    }
    if (op % 100 == 0) check_equal(b, o);
  }
  check_equal(b, o);
  if (!wide && !jumpy) CHECK(b.recentre_failures() == 0);
}
}  // namespace

TEST_CASE("core.l3_book: property test vs map<price, deque> oracle") {
  for (std::uint32_t seed = 1; seed <= 60; ++seed) run_oracle(seed, true);
}

TEST_CASE("core.l3_book: property test with stub quotes and a drifting market in a small window") {
  for (std::uint32_t seed = 1; seed <= 60; ++seed) run_oracle(seed, false);
}

TEST_CASE("core.l3_book: property test with a jumping market through negative prices") {
  for (std::uint32_t seed = 1; seed <= 60; ++seed) run_oracle(seed, false, true);
}

TEST_CASE("core.l3_book: mutations return the price aggregate across window and overflow moves") {
  Book b(kTick, kCfg);
  REQUIRE(b.add(1, Side::Buy, px(10000), qt(1)) == L3Error::None);
  REQUIRE(b.add(2, Side::Sell, px(10001), qt(1)) == L3Error::None);
  Qty aggregate;
  std::uint64_t ref = 3;
  for (const auto price : {10001, 12000, 10, 16000}) {
    CAPTURE(price);
    for (int i = 0; i < 8; ++i) {
      const auto first = ref++;
      const auto second = ref++;
      const Qty base = price == 10001 ? qt(1) : Qty{};
      REQUIRE(b.add(first, Side::Sell, px(price), qt(3), &aggregate) == L3Error::None);
      CHECK(aggregate == base + qt(3));
      REQUIRE(b.add(second, Side::Sell, px(price), qt(5), &aggregate) == L3Error::None);
      CHECK(aggregate == base + qt(8));
      REQUIRE(b.execute(b.find(first), qt(1), &aggregate) == L3Error::None);
      CHECK(aggregate == base + qt(7));
      REQUIRE(b.cancel(b.find(first), Qty{}, &aggregate) == L3Error::None);
      CHECK(aggregate == base + qt(5));
      REQUIRE(b.cancel(b.find(second), Qty{}, &aggregate) == L3Error::None);
      CHECK(aggregate == base);
    }
  }
  aggregate = qt(99);
  CHECK(b.add(1, Side::Buy, px(10000), qt(1), &aggregate) == L3Error::DuplicateRef);
  CHECK(aggregate == qt(99));
  CHECK(b.add(ref, Side::Buy, px(10000), Qty{}, &aggregate) == L3Error::BadQty);
  CHECK(aggregate == qt(99));
}
