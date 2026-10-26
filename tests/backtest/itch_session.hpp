#pragma once
// A random TotalView-ITCH session for tests: a few stocks whose orders join, improve, cancel,
// replace and are executed against in price and time priority, now and then by a sweep through
// several levels or at a price better than the one displayed; the displayed book never crosses.
#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/core/rng.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmm::bt::testutil {

constexpr std::uint64_t kSecond = 1'000'000'000;

// A random order flow for a few stocks: orders join, improve, cancel, replace and are executed
// against in price and time priority, now and then by a sweep through several levels or at a price
// better than the one displayed; the displayed book never crosses.
class SessionWriter {
 public:
  explicit SessionWriter(std::uint64_t seed) : rng_(seed) {}

  std::string write(const std::string& name, std::uint64_t from_ns, std::uint64_t to_ns) {
    const char* const syms[] = {"AAAA", "BBBB"};
    const std::int64_t start_px[] = {10000, 2500};  // in ticks of 0.01
    for (std::uint16_t s = 0; s < 2; ++s) {
      put([&](auto out) { return e_.stock_directory(out, loc(s), from_ns - kSecond, syms[s]); });
      Stock st;
      st.name = syms[s];
      st.mid = start_px[s];
      stocks_.push_back(st);
    }
    // A book on both sides before the session starts.
    for (std::uint16_t s = 0; s < 2; ++s) {
      for (int k = 1; k <= 6; ++k) {
        add(s,
            from_ns - kSecond / 2,
            Side::Buy,
            stocks_[s].mid - k,
            100 * (1 + static_cast<std::int64_t>(rng_.uniform(5))));
        add(s,
            from_ns - kSecond / 2,
            Side::Sell,
            stocks_[s].mid + k,
            100 * (1 + static_cast<std::int64_t>(rng_.uniform(5))));
      }
    }
    std::uint64_t t = from_ns;
    while (t < to_ns) {
      t += 20'000 + rng_.uniform(3'000'000);  // 20 us to 3 ms
      const auto s = static_cast<std::uint16_t>(rng_.uniform(2));
      // A burst of messages with one timestamp, as ITCH has.
      const int burst = rng_.uniform(4) == 0 ? 2 + static_cast<int>(rng_.uniform(3)) : 1;
      for (int b = 0; b < burst; ++b) step(s, t);
    }
    const std::string path = testutil::tmp_path(name.c_str());
    std::ofstream(path, std::ios::binary)
        .write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    return path;
  }

 private:
  struct Rest {
    Side side;
    std::int64_t px;
    std::int64_t qty;
  };
  struct Stock {
    std::string name;
    std::int64_t mid = 0;
    std::map<std::uint64_t, Rest> orders;  // ref -> order; refs grow, so this is arrival order
  };

  static std::uint16_t loc(std::uint16_t s) { return static_cast<std::uint16_t>(s + 1); }
  static Price tick_px(std::int64_t ticks) { return Price::from_raw(ticks * 1'000'000); }
  static Qty shares(std::int64_t n) { return Qty::from_int(n); }

  template <class F>
  void put(F&& write) {
    std::array<std::byte, 64> buf{};
    const std::size_t n = write(std::span<std::byte>(buf));
    if (n == 0) throw std::runtime_error("itch session: message did not fit");
    bytes_.push_back(static_cast<char>((n >> 8U) & 0xFFU));
    bytes_.push_back(static_cast<char>(n & 0xFFU));
    bytes_.append(reinterpret_cast<const char*>(buf.data()), n);
  }

  std::int64_t best(const Stock& st, Side side) const {
    std::int64_t b = side == Side::Buy ? 0 : INT64_MAX;
    for (const auto& [ref, o] : st.orders) {
      if (o.side != side) continue;
      b = side == Side::Buy ? std::max(b, o.px) : std::min(b, o.px);
    }
    return b;
  }

  static std::size_t count(const Stock& st, Side side) {
    std::size_t n = 0;
    for (const auto& [ref, o] : st.orders) n += o.side == side ? 1 : 0;
    return n;
  }

  void add(std::uint16_t s, std::uint64_t t, Side side, std::int64_t px, std::int64_t qty) {
    const std::uint64_t ref = next_ref_++;
    put([&](auto out) {
      return e_.add_order(out, loc(s), t, ref, side, shares(qty), stocks_[s].name, tick_px(px));
    });
    stocks_[s].orders[ref] = Rest{side, px, qty};
  }

  // Executes up to `qty` against the best `side` orders, best price first, then time; `levels`
  // bounds how many prices the sweep may take. `better`: the first execution is printed one tick
  // better for the aggressor than the order's displayed price (an order ranked at a hidden price).
  void execute(
      std::uint16_t s, std::uint64_t t, Side side, std::int64_t qty, int levels, bool better) {
    Stock& st = stocks_[s];
    std::int64_t last_px = 0;
    int seen = 0;
    while (qty > 0) {
      std::uint64_t ref = 0;
      std::int64_t b = side == Side::Buy ? 0 : INT64_MAX;
      for (const auto& [r, o] : st.orders) {
        if (o.side != side) continue;
        if (side == Side::Buy ? o.px > b : o.px < b) {
          b = o.px;
          ref = r;
        }
      }
      if (ref == 0 || count(st, side) <= 2) return;  // the book keeps both sides
      if (b != last_px) {
        if (++seen > levels) return;
        last_px = b;
      }
      Rest& o = st.orders[ref];
      const std::int64_t q = std::min(qty, o.qty);
      const std::uint64_t match = next_match_++;
      if (better) {
        const std::int64_t at = side == Side::Buy ? o.px - 1 : o.px + 1;
        put([&](auto out) {
          return e_.order_executed_with_price(
              out, loc(s), t, ref, shares(q), match, tick_px(at), true);
        });
        better = false;
      } else {
        put([&](auto out) { return e_.order_executed(out, loc(s), t, ref, shares(q), match); });
      }
      o.qty -= q;
      qty -= q;
      if (o.qty == 0) st.orders.erase(ref);
    }
  }

  void step(std::uint16_t s, std::uint64_t t) {
    Stock& st = stocks_[s];
    const std::int64_t bb = best(st, Side::Buy);
    const std::int64_t ba = best(st, Side::Sell);
    const std::uint64_t r = rng_.uniform(100);
    if (r < 40 || st.orders.size() < 6) {
      // Join or improve a side, or rest a level or two behind; never cross.
      const Side side = rng_.uniform(2) == 0 ? Side::Buy : Side::Sell;
      std::int64_t px;
      const std::int64_t off = static_cast<std::int64_t>(rng_.uniform(4)) - 1;  // -1 improves
      if (side == Side::Buy) {
        px = (bb > 0 ? bb : st.mid - 1) - off;
        if (ba != INT64_MAX && px >= ba) px = ba - 1;
      } else {
        px = (ba != INT64_MAX ? ba : st.mid + 1) + off;
        if (bb > 0 && px <= bb) px = bb + 1;
      }
      if (px <= 0) return;
      add(s, t, side, px, 100 * (1 + static_cast<std::int64_t>(rng_.uniform(5))));
      return;
    }
    if (r < 62) {
      // Cancel part or all of an order.
      auto it = st.orders.begin();
      std::advance(it, static_cast<long>(rng_.uniform(st.orders.size())));
      const std::uint64_t ref = it->first;
      if (rng_.uniform(3) == 0 && it->second.qty > 100) {
        put([&](auto out) { return e_.order_cancel(out, loc(s), t, ref, shares(100)); });
        it->second.qty -= 100;
      } else if (count(st, it->second.side) > 2) {
        put([&](auto out) { return e_.order_delete(out, loc(s), t, ref); });
        st.orders.erase(it);
      }
      return;
    }
    if (r < 72) {
      // Replace an order: a new reference at a new price and size, behind its new level.
      auto it = st.orders.begin();
      std::advance(it, static_cast<long>(rng_.uniform(st.orders.size())));
      const std::uint64_t old_ref = it->first;
      const Side side = it->second.side;
      std::int64_t px = it->second.px + static_cast<std::int64_t>(rng_.uniform(3)) - 1;
      st.orders.erase(it);
      const std::int64_t nb = best(st, Side::Buy);
      const std::int64_t na = best(st, Side::Sell);
      if (side == Side::Buy && na != INT64_MAX && px >= na) px = na - 1;
      if (side == Side::Sell && nb > 0 && px <= nb) px = nb + 1;
      if (px <= 0) px = 1;
      const std::uint64_t new_ref = next_ref_++;
      const std::int64_t qty = 100 * (1 + static_cast<std::int64_t>(rng_.uniform(5)));
      put([&](auto out) {
        return e_.order_replace(out, loc(s), t, old_ref, new_ref, shares(qty), tick_px(px));
      });
      st.orders[new_ref] = Rest{side, px, qty};
      return;
    }
    if (r < 97) {
      // An aggressor takes the best orders of one side: mostly part of the touch, sometimes a
      // sweep through two or three levels.
      const Side side = rng_.uniform(2) == 0 ? Side::Buy : Side::Sell;
      const int levels = rng_.uniform(6) == 0 ? 2 + static_cast<int>(rng_.uniform(2)) : 1;
      const std::int64_t qty =
          100 *
          (1 + static_cast<std::int64_t>(rng_.uniform(static_cast<std::uint64_t>(levels) * 6)));
      execute(s, t, side, qty, levels, rng_.uniform(15) == 0);
      return;
    }
    // A print against an undisplayed order.
    const std::int64_t px = bb > 0 ? bb : st.mid;
    put([&](auto out) {
      return e_.trade(out, loc(s), t, Side::Buy, shares(100), st.name, tick_px(px), next_match_++);
    });
  }

  codecs::itch::ItchEncoder e_;
  Xoshiro256ss rng_;
  std::string bytes_;
  std::vector<Stock> stocks_;
  std::uint64_t next_ref_ = 1;
  std::uint64_t next_match_ = 1;
};

}  // namespace fastmm::bt::testutil
