// The population (backtest/population.hpp) against the engine: every lane of a population over a
// randomly generated TotalView-ITCH session gets the fills, positions, cash and fees that
// run_backtest() gives its configuration alone.
#include "fastmm/backtest/population.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/rng.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;

namespace {

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
    REQUIRE(n > 0);
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

constexpr const char* kConfig = R"(
[engine]
name = "population-test"
journal = false
min_requote_ticks = 1
min_requote_interval_ms = 0
min_qty_bps = 8000
post_only = true
supports_replace = true

[venues.nq]
kind = "sim"
supports_replace = true

[venues.nq.fees]
maker_bps = -0.2
taker_bps = 0.3

[[instruments]]
venue = "nq"
symbol = "AAAA"
asset_class = "equity"
tick = "0.01"
lot = "1"
min_qty = "1"

[[instruments]]
venue = "nq"
symbol = "BBBB"
asset_class = "equity"
tick = "0.01"
lot = "1"
min_qty = "1"

[strategy]
name = "basic_mm"

[strategy.params]
half_spread_bps = 1
skew_bps_per_unit = 0.5
quote_qty = 100
max_inventory = 300
requote_threshold_ticks = 1
pull_on_stale_ms = 0

[risk]
stp = true

[backtest]
fill_model = "l3_queue"
latency_fixed_us = 50
latency_jitter_us = 0
own_orders_in_feed = false
markout_horizons_s = ""
)";

struct Point {
  const char* half;
  const char* skew;
  const char* qty;
  const char* inv;
  const char* thr;
  std::int64_t lat_us;
};

BacktestConfig with(const BacktestConfig& base, const Point& p) {
  BacktestConfig c = base;
  c.params["half_spread_bps"] = p.half;
  c.params["skew_bps_per_unit"] = p.skew;
  c.params["quote_qty"] = p.qty;
  c.params["max_inventory"] = p.inv;
  c.params["requote_threshold_ticks"] = p.thr;
  c.transport.order_out.fixed = microseconds(p.lat_us);
  c.transport.ack_in.fixed = microseconds(p.lat_us);
  c.output_dir.clear();
  return c;
}

}  // namespace

TEST_CASE("population: every lane trades as its configuration alone") {
  register_builtin_data_sources();
  const std::uint64_t from = (9 * 3600 + 30 * 60) * kSecond;
  // FASTMM_POPULATION_SEED: another session (a local fuzz loop); CI runs seed 7.
  const char* seed_env = std::getenv("FASTMM_POPULATION_SEED");
  const std::uint64_t seed = seed_env != nullptr ? std::strtoull(seed_env, nullptr, 10) : 7;
  const std::string path =
      SessionWriter(seed).write("population_session.itch", from, from + 120 * kSecond);
  const std::string cfg_path = testutil::tmp_path("population_test.toml");
  std::ofstream(cfg_path) << kConfig;
  const BacktestConfig base = BacktestConfig::from_config(Config::load(cfg_path));
  REQUIRE(check_population(base).empty());
  const std::string spec = "itch:" + path + ",date=2019-07-30,start=09:30";
  const auto factory = [&]() { return open_data(spec, &base.instruments); };

  std::vector<Point> points;
  for (const char* half : {"0.25", "2", "6"})
    for (const char* skew : {"0", "3"})
      for (const char* qty : {"100", "300"})
        for (const char* inv : {"300", "0"})
          for (const char* thr : {"1", "2"})
            for (const std::int64_t lat : {20, 400})
              points.push_back({half, skew, qty, inv, thr, lat});
  std::vector<LaneSpec> lanes;
  for (const Point& p : points) lanes.push_back(lane_from_config(with(base, p)));

  std::vector<std::vector<LaneFill>> fills;
  PopulationStats st;
  const std::vector<LaneResult> res = run_population(base, lanes, factory, 2, &st, &fills);
  // The same lanes as lane_kernel.hpp runs them, each (lane, instrument) on its own over the tape.
  std::unique_ptr<MdSource> tape_src = factory();
  const Tape tape = compile_tape(base, *tape_src);
  const std::size_t n_inst = base.instruments.size();
  const std::vector<lk::Out> kout = run_tape(tape, lanes, 2);
  REQUIRE(kout.size() == points.size() * n_inst);
  REQUIRE(res.size() == points.size());

  std::uint64_t total = 0;
  std::uint64_t taker = 0;
  std::uint64_t rejects = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    CAPTURE(i);
    std::unique_ptr<MdSource> src = factory();
    const BacktestResult r = run_backtest(with(base, points[i]), "basic_mm", src.get());
    const FillRows& f = r.fills;
    const std::vector<LaneFill>& g = fills[i];
    REQUIRE(f.size() == g.size());
    Notional cash{};
    Notional fee{};
    std::vector<Qty> pos(base.instruments.size());
    for (std::size_t j = 0; j < f.size(); ++j) {
      CAPTURE(j);
      CHECK(f.ts[j] == g[j].ts.ns);
      CHECK(f.instrument[j] == g[j].instrument);
      CHECK(f.side[j] == static_cast<std::int8_t>(g[j].side));
      CHECK(f.price[j] == g[j].price.raw);
      CHECK(f.qty[j] == g[j].qty.raw);
      CHECK(f.liquidity[j] == static_cast<std::uint8_t>(g[j].liquidity));
      const Price p = Price::from_raw(f.price[j]);
      const Qty q = Qty::from_raw(f.qty[j]);
      const Notional n = base.instruments.get(InstrumentId{f.instrument[j]}).notional(p, q);
      cash = f.side[j] == 0 ? cash - n : cash + n;
      fee = fee + Notional::from_raw(f.fee[j]);
      pos[f.instrument[j]] += f.side[j] == 0 ? q : -q;
    }
    CHECK(cash == res[i].cash);
    CHECK(fee == res[i].fees);
    CHECK(pos == res[i].positions);
    // The run's own decomposition of its fills, marked at the final mid (the engine's net PnL goes
    // through an average cost rounded at every fill; the decomposition shows that as a residual).
    CHECK(r.metrics.decomposition.net == doctest::Approx(res[i].net_pnl.to_double()).epsilon(1e-9));
    CHECK(r.metrics.replaces == res[i].replaces);
    // The kernel: each instrument's fills in order (hashed), cash, fees and position.
    std::vector<std::uint64_t> hash(n_inst);
    for (std::size_t j = 0; j < f.size(); ++j) {
      std::uint64_t& h = hash[f.instrument[j]];
      h = lk::mix(lk::mix(lk::mix(lk::mix(h, static_cast<std::uint64_t>(f.ts[j])),
                                  static_cast<std::uint64_t>(f.side[j])),
                          static_cast<std::uint64_t>(f.price[j])),
                  static_cast<std::uint64_t>(f.qty[j]));
    }
    Notional kcash{};
    Notional kfee{};
    std::uint64_t kreplaces = 0;
    for (std::size_t k = 0; k < n_inst; ++k) {
      const lk::Out& o = kout[i * n_inst + k];
      CAPTURE(k);
      CHECK(o.error == 0);
      CHECK(o.fill_hash == hash[k]);
      CHECK(o.position == pos[k].raw);
      kcash = kcash + Notional::from_raw(o.cash);
      kfee = kfee + Notional::from_raw(o.fees);
      kreplaces += o.replaces;
    }
    CHECK(kcash == cash);
    CHECK(kfee == fee);
    CHECK(kreplaces == res[i].replaces);
    total += f.size();
    taker += res[i].taker_fills;
    rejects += res[i].venue_rejects + res[i].risk_rejects;
  }
  // Seed 7's session exercises the paths: fills, taker fills of a replace, rejects.
  if (seed == 7) {
    CHECK(total > 2000);
    CHECK(taker > 0);
    CHECK(rejects > 0);
  }
}

TEST_CASE("population: configurations outside its scope are refused") {
  const std::string cfg_path = testutil::tmp_path("population_scope.toml");
  std::ofstream(cfg_path) << kConfig;
  BacktestConfig c = BacktestConfig::from_config(Config::load(cfg_path));
  CHECK(check_population(c).empty());
  BacktestConfig j = c;
  j.transport.order_out.jitter = microseconds(10);
  CHECK_FALSE(check_population(j).empty());
  BacktestConfig l2 = c;
  l2.transport.fill_model = sim::FillModel::L2Queue;
  CHECK_FALSE(check_population(l2).empty());
  BacktestConfig own = c;
  own.transport.own_orders_in_feed = true;
  CHECK_FALSE(check_population(own).empty());
  const std::vector<LaneSpec> lanes{lane_from_config(j)};
  CHECK_THROWS_AS(static_cast<void>(run_population(j, lanes, nullptr)), std::invalid_argument);
}
