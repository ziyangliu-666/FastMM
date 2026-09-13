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
using Book = L3Book<1U << 10, 1U << 12>;

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
    auto visit = [&](std::int64_t price, const std::deque<OOrder>& dq) {
      const Level l = b.level(s, i++);
      REQUIRE(l.price == px(price));
      std::int64_t sum = 0;
      for (auto& x : dq) sum += x.qty;
      REQUIRE(l.qty == qt(sum));
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
  }
  REQUIRE(b.order_count() == o.where.size());
}
}  // namespace

TEST_CASE("core.l3_book: add/execute/cancel/replace basics") {
  Book b(kTick);
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

TEST_CASE("core.l3_book: window recentring keeps orders and best levels intact") {
  Book b(kTick);  // 1024 tick window
  CHECK(b.add(1, Side::Buy, px(10000), qt(1)) == L3Error::None);
  CHECK(b.add(2, Side::Sell, px(10001), qt(1)) == L3Error::None);
  // drift far above: forces recentre
  CHECK(b.add(3, Side::Sell, px(10400), qt(2)) == L3Error::None);
  CHECK(b.recentre_count() >= 1);
  CHECK(b.best_bid() == Level{px(10000), qt(1)});
  CHECK(b.best_ask() == Level{px(10001), qt(1)});
  CHECK(b.level(Side::Sell, 1) == Level{px(10400), qt(2)});
  // span wider than the window cannot be represented
  CHECK(b.add(4, Side::Sell, px(12000), qt(1)) == L3Error::OutOfWindow);
  CHECK(b.cancel(1) == L3Error::None);
  CHECK(b.cancel(2) == L3Error::None);
  CHECK(b.cancel(3) == L3Error::None);
  // empty book: any price re-anchors the window
  CHECK(b.add(4, Side::Sell, px(12000), qt(1)) == L3Error::None);
  CHECK(b.best_ask().price == px(12000));
  CHECK(b.qty_at_or_better(Side::Sell, px(12000)) == qt(1));
  CHECK(b.price_for_qty(Side::Sell, qt(1)) == px(12000));
  CHECK(b.price_for_qty(Side::Sell, qt(2)) == Price{});
}

TEST_CASE("core.l3_book: pool exhaustion") {
  L3Book<64, 4> small(kTick);
  for (std::uint64_t i = 0; i < 4; ++i)
    CHECK(small.add(i, Side::Buy, px(10), qt(1)) == L3Error::None);
  CHECK(small.add(9, Side::Buy, px(10), qt(1)) == L3Error::PoolExhausted);
  CHECK(small.cancel(0) == L3Error::None);
  CHECK(small.add(9, Side::Buy, px(10), qt(1)) == L3Error::None);
}

TEST_CASE("core.l3_book: property test vs map<price, deque> oracle") {
  for (std::uint32_t seed = 1; seed <= 60; ++seed) {
    CAPTURE(seed);
    std::mt19937 rng(seed);
    Book b(kTick);
    Oracle o;
    std::uint64_t next_ref = 1;
    std::vector<std::uint64_t> live;
    std::uniform_int_distribution<int> op_dist(0, 9);
    std::uniform_int_distribution<int> px_dist(950, 1050);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    for (int op = 0; op < 4000; ++op) {
      const int o_ = op_dist(rng);
      if (o_ < 4 || live.empty()) {
        const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
        const std::int64_t p = px_dist(rng);
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
          const std::int64_t p = px_dist(rng);
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
  }
}
