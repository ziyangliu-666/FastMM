#include "fastmm/core/book/l2_book.hpp"

#include "test_support.hpp"

#include "fastmm/core/book/book_features.hpp"

#include <map>
#include <random>
#include <vector>

using namespace fastmm;

namespace {
Price px(std::int64_t ticks) {
  return Price::from_raw(ticks * 1'000'000);
}  // 0.01 tick
Qty qt(std::int64_t units) {
  return Qty::from_raw(units * 100'000);
}  // 0.001 lot

// Naive oracle: std::map per side.
struct Oracle {
  std::map<std::int64_t, std::int64_t> bids;  // price raw -> qty raw
  std::map<std::int64_t, std::int64_t> asks;
  void apply(Side s, Price p, Qty q) {
    auto& m = s == Side::Buy ? bids : asks;
    if (q.is_zero()) {
      m.erase(p.raw);
    } else {
      m[p.raw] = q.raw;
    }
  }
};

template <std::size_t D>
void check_equal(const L2Book<D>& book, const Oracle& o) {
  REQUIRE(book.depth(Side::Buy) == o.bids.size());
  REQUIRE(book.depth(Side::Sell) == o.asks.size());
  std::size_t i = 0;
  for (auto it = o.bids.rbegin(); it != o.bids.rend(); ++it, ++i) {
    const Level l = book.level(Side::Buy, i);
    REQUIRE(l.price.raw == it->first);
    REQUIRE(l.qty.raw == it->second);
  }
  i = 0;
  for (auto it = o.asks.begin(); it != o.asks.end(); ++it, ++i) {
    const Level l = book.level(Side::Sell, i);
    REQUIRE(l.price.raw == it->first);
    REQUIRE(l.qty.raw == it->second);
  }
}
}  // namespace

TEST_CASE("core.l2_book: basic top-of-book semantics") {
  L2Book<8> b;
  CHECK_FALSE(b.is_valid());
  b.apply_level(Side::Buy, px(100), qt(5));
  b.apply_level(Side::Buy, px(99), qt(3));
  b.apply_level(Side::Buy, px(101), qt(1));
  b.apply_level(Side::Sell, px(103), qt(2));
  b.apply_level(Side::Sell, px(102), qt(4));
  b.mark_snapshot();
  CHECK(b.best_bid().price == px(101));
  CHECK(b.best_ask().price == px(102));
  CHECK(b.mid() == Price::from_raw((px(101).raw + px(102).raw) / 2));
  CHECK(b.spread() == px(1));
  CHECK(b.level(Side::Buy, 1).price == px(100));
  CHECK(b.level(Side::Buy, 2).price == px(99));
  CHECK(b.level(Side::Buy, 3).qty.is_zero());
  CHECK(b.depth(Side::Buy) == 3);
  CHECK(b.qty_at_or_better(Side::Buy, px(100)) == qt(6));
  CHECK(b.qty_at_or_better(Side::Sell, px(102)) == qt(4));
  CHECK(b.price_for_qty(Side::Buy, qt(6)) == px(100));
  CHECK(b.price_for_qty(Side::Buy, qt(9)) == px(99));
  CHECK(b.price_for_qty(Side::Buy, qt(10)) == Price{});
  CHECK(b.is_valid());
  // update and delete
  b.apply_level(Side::Buy, px(101), qt(7));
  CHECK(b.best_bid().qty == qt(7));
  b.apply_level(Side::Buy, px(101), Qty{});
  CHECK(b.best_bid().price == px(100));
  b.apply_level(Side::Buy, px(555), Qty{});  // delete unknown is a no-op
  CHECK(b.depth(Side::Buy) == 2);
  const auto t = b.top<4>(Side::Sell);
  CHECK(t.count == 2);
  CHECK(t[0].price == px(102));
  CHECK(t[1].price == px(103));
  int n = 0;
  b.for_each_level(Side::Buy, [&](const Level&) { ++n; });
  CHECK(n == 2);
  n = 0;
  b.for_each_level(Side::Buy, [&](const Level&) {
    ++n;
    return false;
  });
  CHECK(n == 1);
}

TEST_CASE("core.l2_book: crossed detection and grace, truncation") {
  L2Book<4> b;
  b.apply_level(Side::Buy, px(100), qt(1));
  b.apply_level(Side::Sell, px(101), qt(1));
  b.mark_snapshot();
  CHECK_FALSE(b.crossed());
  std::vector<std::byte> buf(BookDeltaMsg::size_for(1, 0));
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf.data());
  init_header(*d,
              EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              static_cast<std::uint32_t>(buf.size()));
  d->bid_count = 1;
  d->ask_count = 0;
  d->last_update_id = 5;
  d->hdr.exch_ts = Timestamp{1000};
  d->levels()[0] = Level{px(101), qt(1)};
  b.apply_delta(*d);
  CHECK(b.crossed());
  CHECK_FALSE(b.is_valid());
  CHECK(b.seq() == 5);
  CHECK(b.last_update().ns == 1000);
  CHECK(b.crossed_for(Timestamp{1000 + 50'000'000}).ns == 50'000'000);
  d->levels()[0] = Level{px(101), Qty{}};
  d->hdr.exch_ts = Timestamp{2000};
  b.apply_delta(*d);
  CHECK_FALSE(b.crossed());
  CHECK(b.crossed_for(Timestamp{5000}).ns == 0);

  // truncation: capacity 4 per side
  b.clear();
  for (int i = 0; i < 4; ++i) b.apply_level(Side::Buy, px(90 + i), qt(1));
  CHECK(b.depth(Side::Buy) == 4);
  CHECK_FALSE(b.truncated());
  CHECK_FALSE(b.apply_level(Side::Buy, px(50), qt(1)));  // worse than all: dropped
  CHECK(b.truncated(Side::Buy));
  CHECK(b.depth(Side::Buy) == 4);
  CHECK(b.apply_level(Side::Buy, px(200), qt(1)));  // better: evicts the worst (90)
  CHECK(b.best_bid().price == px(200));
  CHECK(b.level(Side::Buy, 3).price == px(91));
  CHECK_FALSE(b.truncated(Side::Sell));
  // snapshot resets truncation
  const Level bids[] = {{px(10), qt(1)}};
  const Level asks[] = {{px(11), qt(1)}};
  b.apply_snapshot(bids, asks, 9, Timestamp{3});
  CHECK_FALSE(b.truncated());
  CHECK(b.depth() == 2);
  CHECK(b.seq() == 9);
  CHECK(b.is_valid());
  b.clear();
  CHECK_FALSE(b.has_snapshot());
}

TEST_CASE("core.l2_book: property test vs std::map oracle, 200 seeds x 5000 ops") {
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    CAPTURE(seed);
    std::mt19937 rng(seed);
    L2Book<64> book;
    Oracle o;
    // keep prices in a band so the depth cap (64) is hit only sometimes; when the cap is
    // hit the book truncates and we resync the oracle from a snapshot
    std::uniform_int_distribution<int> price_dist(1, 120);
    std::uniform_int_distribution<int> qty_dist(0, 5);
    std::uniform_int_distribution<int> side_dist(0, 1);
    for (int op = 0; op < 5000; ++op) {
      const Side s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
      const Price p = px(price_dist(rng));
      const Qty q = qt(qty_dist(rng));
      const bool kept = book.apply_level(s, p, q);
      if (kept && !book.truncated(s)) {
        o.apply(s, p, q);
      } else {
        // truncated: rebuild both from a snapshot (the venue would resend one)
        std::map<std::int64_t, std::int64_t>& m = s == Side::Buy ? o.bids : o.asks;
        if (kept) {
          if (q.is_zero()) {
            m.erase(p.raw);
          } else {
            m[p.raw] = q.raw;
          }
        }
        std::vector<Level> bids;
        std::vector<Level> asks;
        // keep only the best 64 in the oracle (what a depth-limited snapshot would carry)
        for (auto it = o.bids.rbegin(); it != o.bids.rend() && bids.size() < 64; ++it) {
          bids.push_back({Price::from_raw(it->first), Qty::from_raw(it->second)});
        }
        for (auto it = o.asks.begin(); it != o.asks.end() && asks.size() < 64; ++it) {
          asks.push_back({Price::from_raw(it->first), Qty::from_raw(it->second)});
        }
        o.bids.clear();
        o.asks.clear();
        for (auto& l : bids) o.bids[l.price.raw] = l.qty.raw;
        for (auto& l : asks) o.asks[l.price.raw] = l.qty.raw;
        book.apply_snapshot(bids, asks, static_cast<std::uint64_t>(op), Timestamp{op});
      }
      if (op % 50 == 0) check_equal(book, o);
    }
    check_equal(book, o);
    // cross-check aggregate queries against the oracle
    for (Side s : {Side::Buy, Side::Sell}) {
      const auto& m = s == Side::Buy ? o.bids : o.asks;
      if (m.empty()) continue;
      const Price limit = px(60);
      std::int64_t sum = 0;
      for (const auto& [pr, qr] : m) {
        if (at_or_better(s, Price::from_raw(pr), limit)) sum += qr;
      }
      CHECK(book.qty_at_or_better(s, limit).raw == sum);
    }
  }
}

TEST_CASE("core.book_features: imbalance and microprice") {
  L2Book<8> b;
  b.apply_level(Side::Buy, px(100), qt(3));
  b.apply_level(Side::Sell, px(102), qt(1));
  b.mark_snapshot();
  // (3-1)/(3+1) = 0.5
  CHECK(imbalance(b, 1) == Ratio::from_decimal("0.5").value());
  // microprice = (102*3 + 100*1)/4 = 101.5
  CHECK(microprice(b) == Price::from_decimal("1.015").value());  // px(101.5) with 0.01 ticks
  CHECK(weighted_mid(b, 1) == b.mid());
  L2Book<8> empty;
  CHECK(imbalance(empty, 3) == Ratio{});
  CHECK(microprice(empty) == Price{});
  b.apply_level(Side::Sell, px(102), qt(3));
  CHECK(imbalance(b, 1) == Ratio{});
  CHECK(microprice(b) == b.mid());
}
