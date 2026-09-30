// Backtests with the simulated venues' accounts ([backtest.balances], sim/sim_account.hpp): the
// engine's balance table, its BalanceShort check, the venue's own refusal, basic_mm's sizing and
// xmm's side pulling as they work live; the configuration, a journal's snapshot as the start, and
// replay.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <fmt/format.h>

#include <climits>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

// synthetic_config with the instrument's assets named (BTC / USDT) and the L2 queue model.
BacktestConfig spot_config(std::uint64_t seed, Duration duration) {
  BacktestConfig c = synthetic_config(seed, duration);
  Instrument i = c.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  c.instruments = t;
  c.transport.fill_model = sim::FillModel::L2Queue;
  c.transport.queue_conservatism_bps = 0;
  c.strategy = "basic_mm";
  return c;
}

std::vector<sim::SimAccountConfig> account(const char* btc, const char* usdt) {
  return {sim::SimAccountConfig{VenueId{0},
                                {sim::SimBalance{FixedString<8>("BTC"), nt(btc)},
                                 sim::SimBalance{FixedString<8>("USDT"), nt(usdt)}}}};
}

// Sends one resting sell of 0.01 once the venue has reported, and keeps the answer.
class SellOnce : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "sell_once"; }
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (sent_ || !ctx.balances_live() || !b.is_valid()) return;
    sent_ = true;
    const Price px = b.best_ask().price + ctx.instrument(id).ticks(100);
    static_cast<void>(ctx.send(NewOrderRequest::limit(id, Side::Sell, px, qt("0.01")).post_only()));
  }

 private:
  bool sent_ = false;
};

}  // namespace

TEST_CASE("backtest.balances: the engine refuses what the balance cannot cover, else the venue") {
  BacktestConfig cfg = spot_config(3, seconds(2));
  cfg.transport.accounts = account("0.005", "1000");
  const BacktestResult checked = run_backtest<SellOnce>(cfg);
  CHECK(checked.engine.risk_rejects_by_reason[RejectReason::BalanceShort] == 1);
  CHECK(checked.metrics.orders == 0);
  CHECK(checked.transport.rejects == 0);

  // [risk] check_balance = false: the order goes out and the venue refuses it, as Binance does.
  cfg.engine.balance.check = false;
  const BacktestResult sent = run_backtest<SellOnce>(cfg);
  CHECK(sent.engine.risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
  CHECK(sent.metrics.orders == 1);
  CHECK(sent.transport.rejects == 1);
  CHECK(sent.transport.rejects_balance == 1);
  CHECK(sent.engine.venue_rejects_by_reason[RejectReason::InsufficientBalance] == 1);

  // Enough base: accepted.
  cfg.transport.accounts = account("0.01", "1000");
  const BacktestResult ok = run_backtest<SellOnce>(cfg);
  CHECK(ok.metrics.orders == 1);
  CHECK(ok.transport.rejects == 0);
  CHECK(ok.transport.acks == 1);
}

TEST_CASE("backtest.balances: basic_mm quotes what the simulated account holds") {
  BacktestConfig free_cfg = spot_config(42, seconds(30));
  free_cfg.transport.fill_model = sim::FillModel::Matching;  // the coupled market
  BacktestConfig cfg = free_cfg;
  cfg.transport.accounts = account("0", "1000000");

  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  const BacktestResult r = session.run(session.backend().hooks, runner.get(), "basic_mm");
  const BacktestResult free_run = run_backtest<BasicMM>(free_cfg);

  const auto first_fill = [](const BacktestResult& x, Side side) {
    for (std::size_t i = 0; i < x.fills.size(); ++i) {
      if (x.fills.side[i] == static_cast<std::int8_t>(side)) return x.fills.ts[i];
    }
    return std::int64_t{-1};
  };
  const auto sells_before = [](const BacktestResult& x, std::int64_t ts) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < x.orders.size(); ++i) {
      n +=
          x.orders.side[i] == static_cast<std::int8_t>(Side::Sell) && x.orders.ts[i] < ts ? 1U : 0U;
    }
    return n;
  };
  // No base: no ask until a bid has filled; without the account it quotes both sides at once.
  const std::int64_t bought = first_fill(r, Side::Buy);
  REQUIRE(bought > 0);
  CHECK(sells_before(r, bought) == 0);
  CHECK(sells_before(r, INT64_MAX) > 0);
  CHECK(sells_before(free_run, bought) > 0);
  // The engine cut its quotes to the balance: neither it nor the venue refused anything.
  CHECK(r.engine.risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
  CHECK(r.transport.rejects_balance == 0);
  // The venue's account and the engine's position agree: every BTC held was bought here.
  const sim::SimAccounts* a = session.backend().transport.accounts();
  REQUIRE(a != nullptr);
  CHECK(a->total(VenueId{0}, "BTC").to_double() == doctest::Approx(r.metrics.final_position));
  CHECK(a->total(VenueId{0}, "BTC").raw >= 0);
  CHECK(a->free(VenueId{0}, "USDT").raw >= 0);
}

namespace {

constexpr std::int64_t kStartNs = 1'789'344'931'000'000'000LL;

// Venue a quotes AAA, venue b hedges on BBB; both in X against USD.
constexpr const char* kXmmConfig = R"(
[engine]
rng_seed = 5
min_requote_interval_ms = 0

[venues.a]
kind = "sim"
[venues.b]
kind = "sim"

[[instruments]]
venue = "a"
symbol = "AAA"
base = "X"
quote = "USD"
tick = "0.01"
lot = "0.001"

[[instruments]]
venue = "b"
symbol = "BBB"
base = "X"
quote = "USD"
tick = "0.01"
lot = "0.001"

[strategy]
name = "xmm"
[strategy.params]
quote_qty = 0.01
edge_bps = 0.5
slippage_bps = 0
hedge_tolerance_bps = 50
basis_halflife_s = 0
max_unhedged = 0.05
requote_threshold_ticks = 0

[risk]
max_order_qty = "1"
max_order_notional = "1000"
max_position = "1"
max_open_orders = 16
stale_md_ms = 0

[backtest]
fill_model = "l2_queue"
latency_fixed_us = 100
latency_jitter_us = 0
equity_bar_s = 1
markout_horizons_s = ""
)";

// A two-sided book every 20 ms around a walk, and a trade through one side every fourth step.
std::string feed(std::uint32_t inst, std::int64_t offset_ns, std::uint64_t seed) {
  std::string out = "ts_ns,type,inst,side,price,qty,seq\n";
  Xoshiro256ss rng(seed);
  std::int64_t mid = 10'000;  // ticks
  for (int k = 0; k < 1000; ++k) {
    const std::int64_t ts = kStartNs + k * 20'000'000LL + offset_ns;
    mid += static_cast<std::int64_t>(rng.next() % 3U) - 1;
    const auto seq = static_cast<std::uint64_t>(k) + 1;
    const std::string bid = decimal((mid - 2) * 1'000'000);
    const std::string ask = decimal((mid + 2) * 1'000'000);
    out += fmt::format("{0},S,{1},B,{2},5,{4}\n{0},S,{1},A,{3},5,{4}\n", ts, inst, bid, ask, seq);
    if (k % 4 == 3) {
      const bool sell = (rng.next() & 1U) != 0;
      const std::string through = decimal((mid + (sell ? -3 : 3)) * 1'000'000);
      out += fmt::format(
          "{},T,{},{},{},10,{}\n", ts + 1'000'000, inst, sell ? "A" : "B", through, seq);
    }
  }
  return out;
}

BacktestResult run_xmm(const std::string& extra, const std::string& tag) {
  const auto a = fastmm::test::tmp_dir() / ("balances_xmm_" + tag + "_a.csv");
  const auto b = fastmm::test::tmp_dir() / ("balances_xmm_" + tag + "_b.csv");
  std::ofstream(a) << feed(0, 0, 21);
  std::ofstream(b) << feed(1, 3'000'000, 21);
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(std::string(kXmmConfig) + extra));
  cfg.measure_wall_clock = false;
  std::unique_ptr<MdSource> src =
      open_data("csv:" + a.string() + ",venue=0; csv:" + b.string() + ",venue=1", &cfg.instruments);
  REQUIRE(src != nullptr);
  return run_backtest(cfg, "xmm", src.get());
}

// New orders on `inst` and `side` sent before `before`.
std::size_t orders_on(const BacktestResult& r,
                      std::uint32_t inst,
                      Side side,
                      std::int64_t before = INT64_MAX) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    n += r.orders.instrument[i] == inst && r.orders.side[i] == static_cast<std::int8_t>(side) &&
                 r.orders.kind[i] == kOrderKindNew && r.orders.ts[i] < before
             ? 1U
             : 0U;
  }
  return n;
}
std::int64_t first_buy_fill(const BacktestResult& r, std::uint32_t inst) {
  for (std::size_t i = 0; i < r.fills.size(); ++i) {
    if (r.fills.instrument[i] == inst && r.fills.side[i] == static_cast<std::int8_t>(Side::Buy))
      return r.fills.ts[i];
  }
  return INT64_MAX;
}

}  // namespace

TEST_CASE("backtest.balances: xmm does not quote a side the quote venue's account cannot fill") {
  const BacktestResult free_run = run_xmm("", "free");
  REQUIRE(orders_on(free_run, 0, Side::Buy) > 0);
  REQUIRE(orders_on(free_run, 0, Side::Sell) > 0);
  const BacktestResult r = run_xmm(R"(
[backtest.venues.a.balances]
X = "0"
USD = "100000"
[backtest.venues.b.balances]
X = "10"
USD = "100000"
)",
                                   "short");
  // No X on venue a: no ask there until a bid has filled; bids and the hedges go on as before.
  const std::int64_t bought = first_buy_fill(r, 0);
  REQUIRE(bought != INT64_MAX);
  CHECK(orders_on(r, 0, Side::Buy) > 0);
  CHECK(orders_on(r, 0, Side::Sell, bought) == 0);
  CHECK(orders_on(free_run, 0, Side::Sell, bought) > 0);
  CHECK(orders_on(r, 1, Side::Sell) > 0);  // the bids' hedges
  CHECK(r.engine.risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
  CHECK(r.transport.rejects_balance == 0);
}

TEST_CASE("backtest.balances: configuration") {
  const std::string base = R"(
[venues.a]
kind = "sim"
[venues.b]
kind = "sim"
[venues.c]
kind = "sim"
[[instruments]]
venue = "a"
symbol = "AAA"
base = "X"
quote = "USD"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "b"
symbol = "BBB"
base = "X"
quote = "USD"
tick = "0.01"
lot = "0.001"
)";
  SUBCASE("absent: no accounts") {
    const BacktestConfig c = BacktestConfig::from_config(Config::parse(base));
    CHECK(c.transport.accounts.empty());
    CHECK_FALSE(c.balances_from_journal);
  }
  SUBCASE("[backtest.balances] for every traded venue, a venue's own table instead") {
    const BacktestConfig c = BacktestConfig::from_config(Config::parse(base + R"(
[backtest]
balances_from_journal = true
[backtest.balances]
X = "1.5"
USD = 1000
[backtest.venues.b.balances]
USD = "7"
)"));
    CHECK(c.balances_from_journal);
    REQUIRE(c.transport.accounts.size() == 2);  // venue c trades nothing
    CHECK(c.transport.accounts[0].venue == VenueId{0});
    REQUIRE(c.transport.accounts[0].balances.size() == 2);
    CHECK(c.transport.accounts[0].balances[0].asset.view() == "USD");
    CHECK(c.transport.accounts[0].balances[0].amount == nt("1000"));
    CHECK(c.transport.accounts[0].balances[1].asset.view() == "X");
    CHECK(c.transport.accounts[0].balances[1].amount == nt("1.5"));
    CHECK(c.transport.accounts[1].venue == VenueId{1});
    REQUIRE(c.transport.accounts[1].balances.size() == 1);
    CHECK(c.transport.accounts[1].balances[0].amount == nt("7"));
    CHECK(c.warnings.empty());
  }
  SUBCASE("errors") {
    const auto parse = [&](const std::string& extra) {
      static_cast<void>(BacktestConfig::from_config(Config::parse(base + extra)));
    };
    CHECK_THROWS_WITH_AS(parse("[backtest.balances]\nX = \"-1\"\n"),
                         doctest::Contains("backtest.balances.X"),
                         ConfigError);
    CHECK_THROWS_WITH_AS(parse("[backtest.balances]\nX = \"abc\"\n"),
                         doctest::Contains("not a decimal amount"),
                         ConfigError);
    CHECK_THROWS_WITH_AS(parse("[backtest.venues.c.balances]\nX = \"1\"\n"),
                         doctest::Contains("no [[instruments]] trade on venue 'c'"),
                         ConfigError);
    CHECK_THROWS_WITH_AS(parse("[backtest.balances]\nVERYLONGNAME = \"1\"\n"),
                         doctest::Contains("1 to 8 characters"),
                         ConfigError);
  }
}

TEST_CASE("backtest.balances: a run with balances replays to the identical outbound hash") {
  BacktestConfig cfg = spot_config(9, seconds(20));
  cfg.transport.accounts = account("0.004", "150");
  const auto path = fastmm::test::tmp_dir() / "balances_replay.fmj";
  std::filesystem::remove(path);
  cfg.journal_out = path.string();
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.metrics.fills > 0);
  const ReplayResult rp = replay_journal(path.string(), cfg);
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.outbound_messages == rec.outbound_messages);
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());

  SUBCASE("the journal's first snapshot is the next run's start") {
    BacktestConfig next = spot_config(9, seconds(20));
    next.balances_from_journal = true;
    JournalSource src(path.string());
    REQUIRE(src.balance_snapshots()->size() == 1);
    BacktestSession session(next, &src, &BasicMM::schema());
    const sim::SimAccounts* a = session.backend().transport.accounts();
    REQUIRE(a != nullptr);
    CHECK(a->total(VenueId{0}, "BTC") == nt("0.004"));
    CHECK(a->total(VenueId{0}, "USDT") == nt("150"));
    // Not a journal: refused.
    BacktestConfig synthetic = next;
    CHECK_THROWS_AS(static_cast<void>(run_backtest(synthetic, "basic_mm")), std::invalid_argument);
  }
}
